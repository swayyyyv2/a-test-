#include "dash_visual.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include <imgui.h>

#include "../../../core/ui/runtime/ui_visibility_runtime.h"
#include "../../movement/movement_settings_store.h"
#include "../graphics/renderer/world_lines.h"

namespace sunrise::client::hooks::dash::visual {
namespace {

namespace lines = graphics::renderer::world_lines;
constexpr std::size_t kHistory = 8;
constexpr std::size_t kEdgesPerGhost = 12;
constexpr ULONGLONG kSampleIntervalMs = 28;
constexpr ULONGLONG kGhostLifetimeMs = 360;
constexpr ULONGLONG kScreenLifetimeMs = 260;
constexpr float kPi = 3.14159265358979323846F;

struct Sample {
    teleport::Vector position{};
    float directionX{};
    float directionY{};
    ULONGLONG time{};
};

SRWLOCK g_lock{SRWLOCK_INIT};
std::array<Sample, kHistory> g_samples{};
std::size_t g_count{};
std::size_t g_next{};
ULONGLONG g_started{};
ULONGLONG g_lastSample{};

[[nodiscard]] lines::Vector3 point(const Sample& sample,
                                   float forward,
                                   float side,
                                   float height) noexcept {
    return {sample.position[0] + sample.directionX * forward + sample.directionY * side,
            sample.position[1] + sample.directionY * forward - sample.directionX * side,
            sample.position[2] + height};
}

void add_ghost(const Sample& sample,
               float phase,
               lines::Color color,
               std::array<lines::Edge, kHistory * kEdgesPerGhost + kHistory>& edges,
               std::size_t& count,
               lines::Sphere& head) noexcept {
    const float swing = std::sin(phase * 8.0F);
    const auto pelvis = point(sample, 0.0F, 0.0F, -0.18F);
    const auto chest = point(sample, 0.22F, 0.0F, 0.42F);
    const auto leftShoulder = point(sample, 0.19F, -0.27F, 0.43F);
    const auto rightShoulder = point(sample, 0.19F, 0.27F, 0.43F);
    const auto leftElbow = point(sample, -0.04F, -0.36F, 0.12F);
    const auto rightElbow = point(sample, -0.04F, 0.36F, 0.12F);
    const auto leftHand = point(sample, -0.30F - swing * 0.10F, -0.28F, -0.12F);
    const auto rightHand = point(sample, -0.30F + swing * 0.10F, 0.28F, -0.12F);
    const auto leftHip = point(sample, 0.0F, -0.17F, -0.20F);
    const auto rightHip = point(sample, 0.0F, 0.17F, -0.20F);
    const auto leftKnee = point(sample, 0.17F + swing * 0.16F, -0.17F, -0.59F);
    const auto rightKnee = point(sample, 0.17F - swing * 0.16F, 0.17F, -0.59F);
    const auto leftFoot = point(sample, 0.34F + swing * 0.24F, -0.17F, -0.95F);
    const auto rightFoot = point(sample, 0.34F - swing * 0.24F, 0.17F, -0.95F);
    const auto edge = [&](const lines::Vector3& a, const lines::Vector3& b) noexcept {
        edges[count++] = {a, b, color};
    };
    edge(pelvis, chest);
    edge(leftShoulder, rightShoulder);
    edge(leftShoulder, leftElbow);
    edge(leftElbow, leftHand);
    edge(rightShoulder, rightElbow);
    edge(rightElbow, rightHand);
    edge(leftHip, rightHip);
    edge(leftHip, leftKnee);
    edge(leftKnee, leftFoot);
    edge(rightHip, rightKnee);
    edge(rightKnee, rightFoot);
    edge(chest, point(sample, 0.30F, 0.0F, 0.69F));
    head = {point(sample, 0.34F, 0.0F, 0.83F), 0.17F, 10, color};
}

[[nodiscard]] bool screen_pulse(ULONGLONG now, ULONGLONG started, bool blink) noexcept {
    if (started == 0 || now < started || now - started >= kScreenLifetimeMs) return false;
    const float progress = static_cast<float>(now - started) / static_cast<float>(kScreenLifetimeMs);
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    if (display.x <= 0.0F || display.y <= 0.0F) return false;
    const ImVec2 centre{display.x * 0.5F, display.y * 0.5F};
    const float scale = (std::min)(display.x, display.y);
    const auto alpha = static_cast<int>((1.0F - progress) * 72.0F);
    const ImU32 color = blink ? IM_COL32(182, 126, 255, alpha)
                                : IM_COL32(115, 228, 255, alpha);
    ImDrawList* const canvas = ImGui::GetForegroundDrawList();
    canvas->AddCircle(centre, scale * (0.12F + progress * 0.17F), color, 48, 2.0F);
    for (int i = 0; i < 12; ++i) {
        const float angle = (2.0F * kPi * static_cast<float>(i)) / 12.0F;
        const float c = std::cos(angle), s = std::sin(angle);
        const float nearRadius = scale * (0.24F + progress * 0.11F);
        const float farRadius = nearRadius + scale * 0.13F;
        canvas->AddLine({centre.x + c * nearRadius, centre.y + s * nearRadius},
                        {centre.x + c * farRadius, centre.y + s * farRadius}, color, 1.5F);
    }
    return true;
}

} // namespace

void begin() noexcept {
    AcquireSRWLockExclusive(&g_lock);
    g_count = 0;
    g_next = 0;
    g_lastSample = 0;
    g_started = GetTickCount64();
    ReleaseSRWLockExclusive(&g_lock);
}

void sample(const teleport::Vector& position, float directionX, float directionY) noexcept {
    const ULONGLONG now = GetTickCount64();
    AcquireSRWLockExclusive(&g_lock);
    if (g_started != 0 && (g_lastSample == 0 || now - g_lastSample >= kSampleIntervalMs)) {
        g_samples[g_next] = {position, directionX, directionY, now};
        g_next = (g_next + 1) % kHistory;
        g_count = (std::min)(g_count + 1, kHistory);
        g_lastSample = now;
    }
    ReleaseSRWLockExclusive(&g_lock);
}

bool draw(ID3D11Device* device,
          ID3D11DeviceContext* context,
          ID3D11RenderTargetView* target) noexcept {
    const client::movement::Settings settings = client::movement::get();
    if (!settings.dashEnabled || core::ui::runtime::snapshot().visible) return false;
    std::array<Sample, kHistory> samples{};
    std::size_t count{};
    ULONGLONG started{};
    AcquireSRWLockShared(&g_lock);
    count = g_count;
    started = g_started;
    for (std::size_t i = 0; i < count; ++i) {
        samples[i] = g_samples[(g_next + kHistory - count + i) % kHistory];
    }
    ReleaseSRWLockShared(&g_lock);
    const ULONGLONG now = GetTickCount64();
    const bool pulseDrawn = screen_pulse(now, started, settings.dashBlinkMode);
    teleport::CameraPose camera{};
    if (count == 0 || !teleport::camera_pose(camera)) return pulseDrawn;
    std::array<lines::Edge, kHistory * kEdgesPerGhost + kHistory> edges{};
    std::array<lines::Sphere, kHistory> heads{};
    std::size_t edgeCount{}, headCount{};
    for (std::size_t i = 0; i < count; ++i) {
        const Sample& sample = samples[i];
        if (now < sample.time || now - sample.time >= kGhostLifetimeMs) continue;
        const float fade = 1.0F - static_cast<float>(now - sample.time) / static_cast<float>(kGhostLifetimeMs);
        const lines::Color color = settings.dashBlinkMode
                                       ? lines::Color{175, 115, 255,
                                                      static_cast<std::uint8_t>(fade * 120.0F)}
                                       : lines::Color{100, 225, 255,
                                                      static_cast<std::uint8_t>(fade * 100.0F)};
        const float phase = static_cast<float>(sample.time - started) / 1000.0F;
        add_ghost(sample, phase, color, edges, edgeCount, heads[headCount++]);
        if (i > 0 && samples[i - 1].time != 0 && now - samples[i - 1].time < kGhostLifetimeMs) {
            edges[edgeCount++] = {samples[i - 1].position, sample.position, color};
        }
    }
    if (headCount == 0) return pulseDrawn;
    lines::Batch batch{};
    batch.edges = std::span<const lines::Edge>(edges.data(), edgeCount);
    batch.spheres = std::span<const lines::Sphere>(heads.data(), headCount);
    batch.lineWidthPixels = 2.5F;
    return lines::draw(device, context, target, camera, batch).rendered || pulseDrawn;
}

void reset() noexcept {
    AcquireSRWLockExclusive(&g_lock);
    g_count = g_next = 0;
    g_started = g_lastSample = 0;
    ReleaseSRWLockExclusive(&g_lock);
}

} // namespace sunrise::client::hooks::dash::visual
