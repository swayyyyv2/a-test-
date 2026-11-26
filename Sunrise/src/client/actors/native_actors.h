#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

/**
 * Read-only view of the client's live combat actors: which authored source spawned each one,
 * whether its AI and health components are bound, and its exact health fraction as the game's
 * own regional-health getter reports it.
 *
 * Ported from Dawn's live-verified readers for build 86657 (coo_enemy_readiness.h,
 * gateway_native_read.h, omega_boss_health.cpp). Every read goes through ReadProcessMemory and
 * every identity is re-checked, so a stale or moved actor is skipped rather than misread. The
 * only native call is the health getter, behind a prologue check and a structured exception
 * guard. Nothing is written.
 */
namespace sunrise::client::actors::native {

/** One live actor as the client last read it. */
struct Actor {
    /** The actor's own handle; its low 13 bits are its row. */
    std::uint32_t actor{UINT32_MAX};
    /** Its world entity handle. */
    std::uint32_t entity{UINT32_MAX};
    /** Registry key of the authored object whose source spawned it; 0 when unread. */
    std::uint32_t registryKey{};
    /** Slot type of that source (1 for squad member sources). */
    std::uint32_t sourceType{};
    /** Slot index of that source on its object. */
    std::uint32_t sourceSlot{};
    /** The source's spawn generation. */
    std::uint32_t generation{};
    /** Health fraction 0..1 from the native getter; meaningful only when healthKnown. */
    float health{};
    /** The source definition was read and matched. */
    bool sourceKnown{};
    /** The character's health component was found and the getter returned a valid fraction. */
    bool healthKnown{};
    /** The health component's death bit is set. */
    bool dead{};
    /** The actor's AI owner is bound back to it. */
    bool ai{};
};

/** At most this many actors are published; the native pool holds no more than this. */
inline constexpr std::size_t kCapacity = 256;

/** The last complete read. */
struct Snapshot {
    std::array<Actor, kCapacity> actors{};
    std::size_t count{};
    /** GetTickCount64 when it was read; 0 before the first read. */
    std::uint64_t tick{};
    /** Milliseconds since that read when snapshot() copied it; UINT64_MAX before the first read. */
    std::uint64_t ageMs{UINT64_MAX};
};

/** Re-reads the actor table, at most every 100 ms. Call it per frame, on a game thread. */
void poll() noexcept;

/** Drops the published actors. */
void reset() noexcept;

/** Copies the last read. Any thread may call it. */
void snapshot(Snapshot& output) noexcept;

/**
 * Finds the actor one authored source spawned.
 * @param registryKey Registry key of the source's object.
 * @param sourceSlot Slot index of the source.
 * @param output Receives the actor when found.
 * @param ageMs Receives how old the read is, in milliseconds.
 * @return True when a live actor from that source was in the last read.
 */
[[nodiscard]] bool find(std::uint32_t registryKey,
                        std::uint32_t sourceSlot,
                        Actor& output,
                        std::uint64_t& ageMs) noexcept;

} // namespace sunrise::client::actors::native
