#include "dash.h"
#include "dash_visual.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <optional>

#include "../../../core/logging/log.h"
#include "../../../core/ui/runtime/ui_visibility_runtime.h"
#include "../../../state/account/account_state.h"
#include "../../../state/runtime/runtime.h"
#include "../../input/window_focus.h"
#include "../../movement/movement_settings_store.h"
#include "../fly/fly.h"
#include "../noclip/runtime.h"
#include "../polled_input/runtime.h"
#include "../teleport/runtime.h"

namespace sunrise::client::hooks::dash {
namespace {

namespace bindings = state::account::settings::bindings;
constexpr SHORT kHeld = static_cast<SHORT>(0x8000);
constexpr float kMinimumLengthSquared = 0.000001F;
constexpr ULONGLONG kRequestLifetimeMs = 250;
constexpr std::array<bindings::Action, 6> kActions{
    bindings::Action::moveForward, bindings::Action::moveBackward,
    bindings::Action::moveLeft, bindings::Action::moveRight,
    bindings::Action::holdCrouch, bindings::Action::toggleCrouch};
constexpr ULONGLONG kSlideHoldMs = 120;

SRWLOCK g_lock{SRWLOCK_INIT};
std::atomic_bool g_keyDown{false};
std::atomic_bool g_wanted{false};
std::atomic<ULONGLONG> g_slideUntil{0};
std::array<bindings::Binding, kActions.size()> g_bindings{};
bool g_bindingsRead{};
bool g_pending{};
ULONGLONG g_requestedAt{};
ULONGLONG g_activeUntil{};
ULONGLONG g_nextReady{};
float g_directionX{};
float g_directionY{};
std::uint32_t g_slideKey{};

[[nodiscard]] bool half_down(const std::optional<std::uint16_t>& half) noexcept {
    if (!half) return false;
    const std::uint32_t key = teleport::action_key(*half);
    return key != 0 && (GetAsyncKeyState(static_cast<int>(key)) & kHeld) != 0;
}

void read_bindings() noexcept {
    if (g_bindingsRead) return;
    const state::AccountState account = state::bound_account_snapshot();
    if (!account.settings.keyBindings.configured) return;
    for (std::size_t i = 0; i < kActions.size(); ++i) {
        g_bindings[i] = account.settings.keyBindings.values[static_cast<std::size_t>(kActions[i])];
    }
    g_bindingsRead = true;
}

[[nodiscard]] std::uint32_t bound_key(const bindings::Binding& binding) noexcept {
    if (binding.primary) {
        const std::uint32_t key = teleport::action_key(*binding.primary);
        if (key != 0) return key;
    }
    return binding.secondary ? teleport::action_key(*binding.secondary) : 0;
}

[[nodiscard]] bool direction(float& x, float& y) noexcept {
    teleport::Vector forward{};
    if (!teleport::camera_forward(forward)) return false;
    const float horizontalLength = std::hypot(forward[0], forward[1]);
    if (horizontalLength <= kMinimumLengthSquared) return false;
    const float fx = forward[0] / horizontalLength;
    const float fy = forward[1] / horizontalLength;
    read_bindings();
    std::array<bool, 4> pressed{};
    if (g_bindingsRead) {
        for (std::size_t i = 0; i < pressed.size(); ++i) {
            pressed[i] = half_down(g_bindings[i].primary) || half_down(g_bindings[i].secondary);
        }
    }
    const float forwardScale = static_cast<float>(pressed[0]) - static_cast<float>(pressed[1]);
    const float rightScale = static_cast<float>(pressed[3]) - static_cast<float>(pressed[2]);
    x = fx * forwardScale + fy * rightScale;
    y = fy * forwardScale - fx * rightScale;
    const float length = std::hypot(x, y);
    if (length <= kMinimumLengthSquared) { x = fx; y = fy; }
    else { x /= length; y /= length; }
    return true;
}

} // namespace

void poll() noexcept {
    const client::movement::Settings settings = client::movement::get();
    const bool eligible = settings.dashEnabled && settings.dashKey != client::movement::kNoKey
                          && !settings.flyEnabled && !settings.noclipEnabled;
    const ULONGLONG now = GetTickCount64();
    const bool interfaceVisible = core::ui::runtime::snapshot().visible;
    const ULONGLONG slideUntil = g_slideUntil.load(std::memory_order_acquire);
    if (slideUntil != 0 && (now >= slideUntil || !eligible || interfaceVisible)) {
        g_slideUntil.store(0, std::memory_order_release);
        polled_input::release_dash_key();
    }
    const bool down = eligible && input::game_focused()
                      && (GetAsyncKeyState(static_cast<int>(settings.dashKey)) & kHeld) != 0;
    const bool wasDown = g_keyDown.exchange(down, std::memory_order_acq_rel);
    if (!down || wasDown || interfaceVisible) return;
    float x{}, y{};
    if (!direction(x, y)) return;
    std::uint32_t slideKey = 0;
    if (!settings.dashBlinkMode && settings.dashSlideAnimation && g_bindingsRead) {
        slideKey = bound_key(g_bindings[4]);
        if (slideKey == 0) slideKey = bound_key(g_bindings[5]);
    }
    AcquireSRWLockExclusive(&g_lock);
    if (now >= g_nextReady && now >= g_activeUntil) {
        g_directionX = x;
        g_directionY = y;
        g_slideKey = slideKey;
        g_requestedAt = now;
        g_pending = true;
        g_wanted.store(true, std::memory_order_release);
    }
    ReleaseSRWLockExclusive(&g_lock);
}

bool wanted() noexcept { return g_wanted.load(std::memory_order_acquire); }

bool before_step(void* body) noexcept {
    if (body == nullptr || !wanted()) return false;
    const client::movement::Settings settings = client::movement::get();
    const ULONGLONG now = GetTickCount64();
    float x{}, y{};
    bool active = false;
    bool started = false;
    std::uint32_t slideKey = 0;
    AcquireSRWLockExclusive(&g_lock);
    if (!settings.dashEnabled || settings.flyEnabled || settings.noclipEnabled) {
        g_pending = false;
        g_activeUntil = 0;
    } else {
        if (g_pending) {
            g_pending = false;
            if (now - g_requestedAt <= kRequestLifetimeMs && now >= g_nextReady) {
                const float duration = settings.dashBlinkMode
                                           ? (std::min)(settings.dashDuration, 0.10F)
                                           : settings.dashDuration;
                g_activeUntil = now + static_cast<ULONGLONG>(duration * 1000.0F);
                g_nextReady = now + static_cast<ULONGLONG>(settings.dashCooldown * 1000.0F);
                slideKey = !settings.dashBlinkMode && settings.dashSlideAnimation ? g_slideKey : 0;
                started = true;
            }
        }
        active = now < g_activeUntil;
        x = g_directionX;
        y = g_directionY;
    }
    g_wanted.store(g_pending || active, std::memory_order_release);
    ReleaseSRWLockExclusive(&g_lock);
    if (!active) return false;
    noclip::Vector velocity{};
    noclip::read_body_velocity(body, velocity);
    const float speed = settings.dashBlinkMode
                            ? (std::min)(settings.dashSpeed * 1.8F,
                                         client::movement::kMaximumDashSpeed)
                            : settings.dashSpeed;
    velocity[0] = x * speed;
    velocity[1] = y * speed;
    noclip::write_body_velocity(body, velocity);
    if (started) {
        visual::begin();
        if (slideKey != 0) {
            polled_input::hold_dash_key(slideKey);
            g_slideUntil.store(now + kSlideHoldMs, std::memory_order_release);
        }
        core::log::write(core::log::Channel::client, core::log::Level::info,
                         settings.dashBlinkMode ? "ev=dash stage=start mode=blink result=ok"
                                                : "ev=dash stage=start mode=burst result=ok");
    }
    return true;
}

void after_step(void* body) noexcept {
    if (body == nullptr) return;
    float directionX{}, directionY{};
    AcquireSRWLockShared(&g_lock);
    directionX = g_directionX;
    directionY = g_directionY;
    ReleaseSRWLockShared(&g_lock);
    noclip::Vector position{};
    noclip::read_body_position(body, position);
    visual::sample(position, directionX, directionY);
    noclip::Vector velocity{};
    noclip::read_body_velocity(body, velocity);
    const float length = std::hypot(velocity[0], velocity[1], velocity[2]);
    if (length > fly::kPublishedSpeedCap) {
        const float scale = fly::kPublishedSpeedCap / length;
        for (float& lane : velocity) lane *= scale;
        noclip::write_body_velocity(body, velocity);
    }
}

void reset() noexcept {
    AcquireSRWLockExclusive(&g_lock);
    g_pending = false;
    g_activeUntil = 0;
    g_nextReady = 0;
    g_wanted.store(false, std::memory_order_release);
    ReleaseSRWLockExclusive(&g_lock);
    g_keyDown.store(false, std::memory_order_release);
    g_slideUntil.store(0, std::memory_order_release);
    polled_input::release_dash_key();
    visual::reset();
}

} // namespace sunrise::client::hooks::dash
