#pragma once

namespace sunrise::client::hooks::dash {

/** Captures a fresh dash-key press on the camera/input frame. */
void poll() noexcept;
/** True while a request or burst needs the local Havok body. */
[[nodiscard]] bool wanted() noexcept;
/** Applies the horizontal velocity before collision simulation. */
[[nodiscard]] bool before_step(void* body) noexcept;
/** Caps the published velocity after the collision step. */
void after_step(void* body) noexcept;
/** Drops pending input and burst state on hook teardown. */
void reset() noexcept;

} // namespace sunrise::client::hooks::dash
