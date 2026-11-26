#pragma once

#include "../teleport/runtime.h"

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11RenderTargetView;

namespace sunrise::client::hooks::dash::visual {

/** Opens one burst's fading visual history. */
void begin() noexcept;
/** Records the local player's post-simulation position and dash direction. */
void sample(const teleport::Vector& position, float directionX, float directionY) noexcept;
/** Draws the animated ghost trail and short screen cue on the overlay frame. */
[[nodiscard]] bool draw(ID3D11Device* device,
                        ID3D11DeviceContext* context,
                        ID3D11RenderTargetView* target) noexcept;
/** Clears visual state on hook teardown. */
void reset() noexcept;

} // namespace sunrise::client::hooks::dash::visual
