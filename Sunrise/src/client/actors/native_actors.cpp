#include "native_actors.h"

#include <Windows.h>

#include <array>
#include <atomic>
#include <cmath>
#include <cstring>

namespace sunrise::client::actors::native {
namespace {

// Build 86657 image offsets, from Dawn's live-verified readers.
/** Handle directory used by the native handle resolver (original 30CAB0). */
constexpr std::uintptr_t kHandleDirectory = 0x2439C70;
/** Combat actor pool: +0x14 its slot count, +8 the row table, +0x10 the row stride. */
constexpr std::uintptr_t kActorPool = 0x1F9D7F0;
constexpr std::uintptr_t kActorTable = 0x1F9D7F8;
constexpr std::uintptr_t kActorStride = 0x1F9D800;
/** World entity rows: +4 flags (bit 2 set once retired), +0x4C the component bundle. */
constexpr std::uintptr_t kEntityTable = 0x1F93428;
constexpr std::uintptr_t kEntityStride = 0x1F93430;
/** CD6C20(health, region): the regional health fraction getter. */
constexpr std::uintptr_t kHealthFraction = 0xCD6C20;
constexpr std::array<std::uint8_t, 16> kHealthFractionPrefix{
    0x48, 0x83, 0xEC, 0x68, 0x44, 0x8B, 0x09, 0x4C, 0x8B, 0xD1, 0x48, 0x89, 0x4C, 0x24, 0x28, 0x41};

constexpr std::uint32_t kSourceKind = 0x8080948FU;
constexpr std::uint32_t kAiKind = 0x808082ECU;
constexpr std::uint32_t kCharacterKind = 0x80806832U;
constexpr std::uint32_t kHealthReferenceKind = 0x80804BEEU;
constexpr std::uint32_t kHealthRuntimeKind = 0x80804B8AU;
constexpr std::uint32_t kInvalidHandle = UINT32_MAX;
constexpr std::uint32_t kRowMask = 0x1FFFU;

constexpr std::uint64_t kPollIntervalMs = 100;
/** Bytes one poll may copy; a full table of bound actors stays well under it. */
constexpr std::size_t kReadBudget = std::size_t{4} << 20;
/** Component rows walked per resource bundle, and bundles per entity. */
constexpr std::uint64_t kMaximumComponentRows = 1024;
constexpr std::size_t kMaximumBundles = 64;

using HealthFraction = float(__fastcall*)(void*, std::int32_t) noexcept;

/** Bounded, faulting-safe reads of this process's memory. */
struct Reader {
    std::uintptr_t image{};
    std::size_t copied{};

    [[nodiscard]] bool copy(std::uintptr_t address, void* output, std::size_t size) noexcept {
        if (address < 0x10000 || address > UINTPTR_MAX - size || copied + size > kReadBudget) {
            return false;
        }
        copied += size;
        SIZE_T read = 0;
        return ReadProcessMemory(
                   GetCurrentProcess(), reinterpret_cast<const void*>(address), output, size, &read)
                   != 0
               && read == size;
    }

    template <typename T> [[nodiscard]] bool value(std::uintptr_t address, T& output) noexcept {
        return copy(address, &output, sizeof(T));
    }

    /** Resolves a native handle to its object, as the original resolver does. */
    [[nodiscard]] bool
    resolve(std::uint32_t handle, std::uintptr_t& base, std::uintptr_t* allocation = nullptr) noexcept {
        if (handle == kInvalidHandle) {
            return false;
        }
        std::uintptr_t directory = 0;
        std::uintptr_t registry = 0;
        if (!value(image + kHandleDirectory, directory) || !value(directory, registry)) {
            return false;
        }
        const auto shifted = static_cast<std::uint32_t>(static_cast<std::int32_t>(handle) >> 13);
        const std::uint64_t index =
            ((static_cast<std::uint64_t>(shifted) | 0xFFC0000ULL) >> 18) & (shifted & 0xFFFFU);
        std::array<std::byte, 0x38> row{};
        if (!copy(registry + static_cast<std::uintptr_t>(index) * 0x40U, row.data(), row.size())) {
            return false;
        }
        std::int32_t stride = 0;
        std::int32_t mask = 0;
        std::uintptr_t elements = 0;
        std::memcpy(&stride, row.data() + 0x30, sizeof(stride));
        std::memcpy(&mask, row.data() + 0x34, sizeof(mask));
        std::memcpy(&elements, row.data() + 8, sizeof(elements));
        if (stride <= 0 || stride > 0x100000) {
            return false;
        }
        const std::uintptr_t element =
            elements + static_cast<std::uintptr_t>(handle & kRowMask) * static_cast<std::uintptr_t>(stride);
        std::uint64_t relocation = 0;
        if (!value(element + 8, relocation)) {
            return false;
        }
        base = static_cast<std::uintptr_t>(
            element - (relocation & static_cast<std::uint64_t>(static_cast<std::int64_t>(mask))));
        if (allocation != nullptr) {
            *allocation = element;
        }
        return base >= 0x10000;
    }

    /**
     * Finds the one component of `kind` an entity owns by walking its resource bundles, as the
     * native iterator does. Aliased interface rows of the same component are accepted.
     */
    [[nodiscard]] bool component(std::uint32_t bundle,
                                 std::uint32_t entity,
                                 std::uint32_t kind,
                                 std::uintptr_t& result) noexcept {
        result = 0;
        std::array<std::uint32_t, kMaximumBundles> visited{};
        std::size_t used = 0;
        while (bundle != kInvalidHandle && used < visited.size()) {
            for (std::size_t i = 0; i < used; ++i) {
                if (visited[i] == bundle) {
                    return false;
                }
            }
            visited[used++] = bundle;
            std::uintptr_t base = 0;
            std::uintptr_t allocation = 0;
            std::uint32_t flags = 0;
            if (!resolve(bundle, base, &allocation) || !value(base, flags)) {
                return false;
            }
            if ((flags & 2U) == 0) {
                std::uint32_t type = 0;
                std::uintptr_t metadata = 0;
                std::uint64_t count = 0;
                std::int64_t relative = 0;
                if (!value(base + 4, type) || !resolve(type, metadata) || !value(metadata + 0x68, count)
                    || count > kMaximumComponentRows || !value(metadata + 0x70, relative)) {
                    return false;
                }
                if (count != 0
                    && (relative == 0 || relative > 0x1000000 || relative < -0x1000000
                        || metadata > UINTPTR_MAX - 0x1000080
                        || (relative < 0 && metadata + 0x80 < static_cast<std::uintptr_t>(-relative)))) {
                    return false;
                }
                const std::uintptr_t rows =
                    count == 0     ? std::uintptr_t{}
                    : relative < 0 ? metadata + 0x80 - static_cast<std::uintptr_t>(-relative)
                                   : metadata + 0x80 + static_cast<std::uintptr_t>(relative);
                for (std::uint64_t i = 0; i < count; ++i) {
                    std::int32_t offset = 0;
                    if (!value(rows + static_cast<std::uintptr_t>(i) * 24U + 0x14, offset) || offset < 0
                        || offset > 0x400000
                        || base > UINTPTR_MAX - static_cast<std::uintptr_t>(offset) - 0x30) {
                        return false;
                    }
                    const std::uintptr_t address = base + static_cast<std::uintptr_t>(offset);
                    if (result != 0 && address == result) {
                        continue;
                    }
                    std::uint32_t actual = 0;
                    if (!value(address + 4, actual)) {
                        return false;
                    }
                    if (actual != kind) {
                        continue;
                    }
                    std::uint32_t self = 0;
                    std::uint32_t owner = 0;
                    std::uintptr_t resolved = 0;
                    if (!value(address + 0x24, self) || self == kInvalidHandle
                        || !value(address + 0x2C, owner) || owner != entity || !resolve(self, resolved)
                        || resolved != address || (result != 0 && result != address)) {
                        return false;
                    }
                    result = address;
                }
            }
            if (!value(allocation + 0x18, bundle)) {
                return false;
            }
        }
        return bundle == kInvalidHandle && result != 0;
    }
};

/** Calls the native getter for region 0. Kept free of unwinding objects for __try. */
bool native_fraction(std::uintptr_t getter, std::uintptr_t health, float& output) noexcept {
    __try {
        output = reinterpret_cast<HealthFraction>(getter)(reinterpret_cast<void*>(health), 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return std::isfinite(output) && output >= 0.0F && output <= 1.0F;
}

/** Reads the authored source that spawned an actor. */
void read_source(Reader& read, std::uintptr_t row, Actor& actor) noexcept {
    std::uint32_t sourceHandle = 0;
    std::int64_t sourceOffset = 0;
    std::uintptr_t source = 0;
    if (!read.value(row + 0x38, sourceHandle) || !read.value(row + 0x40, sourceOffset) || sourceOffset < 0
        || sourceOffset > 0x1000000 || !read.resolve(sourceHandle, source)) {
        return;
    }
    source += static_cast<std::uintptr_t>(sourceOffset);
    std::uint32_t kind = 0;
    std::uint32_t generation = 0;
    std::uint32_t definitionHandle = 0;
    std::int64_t definitionOffset = 0;
    std::uintptr_t definition = 0;
    if (!read.value(source + 4, kind) || kind != kSourceKind || !read.value(source + 0x1FC, generation)
        || !read.value(source, definitionHandle) || !read.value(source + 8, definitionOffset)
        || definitionOffset < 0 || definitionOffset > 0x1000000
        || !read.resolve(definitionHandle, definition)) {
        return;
    }
    definition += static_cast<std::uintptr_t>(definitionOffset);
    std::uint32_t registry = 0;
    std::uint8_t type = 0;
    std::uint16_t slot = 0;
    if (!read.value(definition + 0x30, registry) || !read.value(definition + 0x34, type)
        || !read.value(definition + 0x36, slot)) {
        return;
    }
    actor.registryKey = registry;
    actor.sourceType = type;
    actor.sourceSlot = slot;
    actor.generation = generation;
    actor.sourceKnown = true;
}

/** True when the actor's AI owner is bound back to this actor and entity. */
[[nodiscard]] bool read_ai(Reader& read, std::uintptr_t row, const Actor& actor) noexcept {
    std::uint32_t parentHandle = 0;
    std::uintptr_t parent = 0;
    std::uint32_t kind = 0;
    std::uint32_t self = 0;
    std::uint32_t entity = 0;
    std::uint32_t owner = 0;
    return read.value(row + 0x50, parentHandle) && parentHandle != kInvalidHandle
           && read.resolve(parentHandle, parent) && read.value(parent + 4, kind) && kind == kAiKind
           && read.value(parent + 0x24, self) && self == parentHandle && read.value(parent + 0x2C, entity)
           && entity == actor.entity && read.value(parent + 0x1470, owner) && owner == actor.actor;
}

/** Finds the character's health component and samples it. */
void read_health(Reader& read, std::uintptr_t getter, std::uintptr_t entityRow, Actor& actor) noexcept {
    std::uint32_t bundle = 0;
    std::uintptr_t character = 0;
    if (!read.value(entityRow + 0x4C, bundle)
        || !read.component(bundle, actor.entity, kCharacterKind, character)) {
        return;
    }
    std::uint32_t characterActor = 0;
    std::uint32_t healthHandle = 0;
    std::uint32_t referenceKind = 0;
    std::int64_t referenceOffset = 0;
    std::uintptr_t health = 0;
    std::uint32_t runtimeKind = 0;
    std::uint32_t self = 0;
    std::uint32_t owner = 0;
    if (!read.value(character + 0xC0, characterActor) || characterActor != actor.actor
        || !read.value(character + 0x2E8, healthHandle) || !read.value(character + 0x2EC, referenceKind)
        || !read.value(character + 0x2F0, referenceOffset) || healthHandle == kInvalidHandle
        || referenceKind != kHealthReferenceKind || referenceOffset != 0
        || !read.resolve(healthHandle, health) || !read.value(health + 4, runtimeKind)
        || runtimeKind != kHealthRuntimeKind || !read.value(health + 0x24, self) || self != healthHandle
        || !read.value(health + 0x2C, owner) || owner != actor.entity) {
        return;
    }
    std::uint8_t flags = 0;
    if (read.value(health + 0x338, flags)) {
        actor.dead = (flags & 1U) != 0;
    }
    if (getter == 0) {
        return;
    }
    float fraction = 0.0F;
    if (!native_fraction(getter, health, fraction)) {
        return;
    }
    // The getter reads live components; a rebind during the call voids the sample.
    std::uint32_t again = 0;
    if (!read.value(character + 0x2E8, again) || again != healthHandle || !read.value(health + 0x24, again)
        || again != healthHandle) {
        return;
    }
    actor.health = fraction;
    actor.healthKnown = true;
}

SRWLOCK g_lock = SRWLOCK_INIT;
Snapshot g_published{};
/** Written only by the polling thread, under g_polling. */
Snapshot g_scratch{};
std::atomic<bool> g_polling{false};
std::atomic<std::uint64_t> g_lastPoll{0};

} // namespace

void poll() noexcept {
    const std::uint64_t now = GetTickCount64();
    if (now - g_lastPoll.load(std::memory_order_relaxed) < kPollIntervalMs) {
        return;
    }
    bool expected = false;
    if (!g_polling.compare_exchange_strong(expected, true, std::memory_order_acquire)) {
        return;
    }
    g_lastPoll.store(now, std::memory_order_relaxed);
    Reader read{reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)), 0};
    g_scratch.count = 0;
    g_scratch.tick = now;
    std::array<std::uint8_t, kHealthFractionPrefix.size()> prefix{};
    const std::uintptr_t getter =
        read.copy(read.image + kHealthFraction, prefix.data(), prefix.size()) && prefix == kHealthFractionPrefix
            ? read.image + kHealthFraction
            : 0;
    std::uintptr_t table = 0;
    std::uint32_t stride = 0;
    std::uint32_t slots = 0;
    std::uintptr_t entities = 0;
    std::uint32_t entityStride = 0;
    if (read.value(read.image + kActorTable, table) && read.value(read.image + kActorStride, stride)
        && stride >= 0x70 && stride <= 0x100000 && read.value(read.image + kEntityTable, entities)
        && read.value(read.image + kEntityStride, entityStride) && entityStride >= 0x50
        && entityStride <= 0x100000) {
        if (!read.value(read.image + kActorPool + 0x14, slots) || slots == 0 || slots > kCapacity) {
            slots = static_cast<std::uint32_t>(kCapacity);
        }
        for (std::uint32_t index = 0; index < slots && g_scratch.count < kCapacity; ++index) {
            const std::uintptr_t row = table + static_cast<std::uintptr_t>(index) * stride;
            Actor actor{};
            if (!read.value(row + 0x48, actor.actor) || actor.actor == kInvalidHandle
                || (actor.actor & kRowMask) != index || !read.value(row + 0x4C, actor.entity)
                || actor.entity == kInvalidHandle) {
                continue;
            }
            const std::uintptr_t entityRow =
                entities + static_cast<std::uintptr_t>(actor.entity & kRowMask) * entityStride;
            std::uint32_t entityFlags = 0;
            if (!read.value(entityRow + 4, entityFlags) || (entityFlags & 4U) != 0) {
                continue;
            }
            read_source(read, row, actor);
            actor.ai = read_ai(read, row, actor);
            read_health(read, getter, entityRow, actor);
            // Discard a row that was retired or rebound while it was being read.
            std::uint32_t again = 0;
            if (!read.value(row + 0x48, again) || again != actor.actor || !read.value(row + 0x4C, again)
                || again != actor.entity) {
                continue;
            }
            g_scratch.actors[g_scratch.count++] = actor;
        }
    }
    AcquireSRWLockExclusive(&g_lock);
    g_published.count = g_scratch.count;
    g_published.tick = g_scratch.tick;
    std::memcpy(g_published.actors.data(), g_scratch.actors.data(), sizeof(Actor) * g_scratch.count);
    ReleaseSRWLockExclusive(&g_lock);
    g_polling.store(false, std::memory_order_release);
}

void reset() noexcept {
    AcquireSRWLockExclusive(&g_lock);
    g_published.count = 0;
    g_published.tick = 0;
    ReleaseSRWLockExclusive(&g_lock);
}

void snapshot(Snapshot& output) noexcept {
    AcquireSRWLockShared(&g_lock);
    output.count = g_published.count;
    output.tick = g_published.tick;
    std::memcpy(output.actors.data(), g_published.actors.data(), sizeof(Actor) * g_published.count);
    ReleaseSRWLockShared(&g_lock);
    output.ageMs = output.tick == 0 ? UINT64_MAX : GetTickCount64() - output.tick;
}

bool find(std::uint32_t registryKey, std::uint32_t sourceSlot, Actor& output, std::uint64_t& ageMs) noexcept {
    bool found = false;
    AcquireSRWLockShared(&g_lock);
    for (std::size_t index = 0; index < g_published.count; ++index) {
        const Actor& actor = g_published.actors[index];
        if (actor.sourceKnown && actor.registryKey == registryKey && actor.sourceSlot == sourceSlot) {
            // Prefer a living actor when an old body from the same source is still in the table.
            if (!found || (output.dead && !actor.dead)) {
                output = actor;
                found = true;
            }
        }
    }
    const std::uint64_t tick = g_published.tick;
    ReleaseSRWLockShared(&g_lock);
    ageMs = tick == 0 ? UINT64_MAX : GetTickCount64() - tick;
    return found;
}

} // namespace sunrise::client::actors::native
