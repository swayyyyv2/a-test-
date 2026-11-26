/**
 * The HUD overlay stack. Overlays are fixed windows in a top corner: each one draws its own
 * content, and the stack places it under the one before it in the same corner.
 */

#include <array>
#include <algorithm>
#include <cmath>
#include <cfloat>
#include <cstddef>
#include <cstdint>
#include <imgui.h>
#include <string_view>

#include "../../../client/actors/native_actors.h"
#include "../scaling/dpi/ui_dpi_scaling.h"
#include "overlay.h"
#include "boss_race_icons.h"
#include "overlays/ui_hud_logo_overlay.h"
#include "overlays/ui_hud_mission_script_overlay.h"
#include "overlays/ui_hud_mission_variables_overlay.h"
#include "overlays/ui_hud_sensor_events_overlay.h"
#include "overlays/ui_hud_session_overlay.h"
#include "overlays/ui_hud_startup_hint_overlay.h"
#include "overlays/ui_hud_status_overlay.h"
#include "store/hud_settings_store.h"
#include "../../../server/activity/mission/mission_script_runtime.h"
#include "../../../state/activity/mission/runtime.h"

namespace sunrise::core::ui::hud {
namespace {

/** The screen corner an overlay stacks in. */
enum class Corner : std::uint8_t {
    topLeft,
    topRight,
};

/** One overlay's identity, frame entry, starting switch state and placement. */
struct Entry {
    const char* displayName;
    /** Key the switch file stores this overlay under. It must outlive every table change. */
    const char* storageKey;
    const char* windowId;
    void (*draw)() noexcept;
    bool startsOn;
    Corner corner;
    /** Authored window width, or zero to fit the content. A fixed width keeps a log steady. */
    float width;
};

/** 24 pixels keep the stack clear of the viewport corner, as the other overlays do. */
constexpr float kViewportMargin = 24.0F;
/** 8 pixels separate two stacked overlays. */
constexpr float kOverlayGap = 8.0F;
/** Wide enough for a long key and a quoted string value side by side. */
constexpr float kVariableLogWidth = 520.0F;
/** A top-left window hangs from its top-left corner. */
constexpr ImVec2 kTopLeftPivot{0.0F, 0.0F};
/** A top-right window hangs from its top-right corner, so it grows leftwards. */
constexpr ImVec2 kTopRightPivot{1.0F, 0.0F};
/** Overlays carry no decoration, take no input, and are never saved. */
constexpr ImGuiWindowFlags kOverlayFlags =
    ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav
    | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize
    | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoMove;
/** Overlay count, taken from the enum so the table and the switches cannot disagree. */
constexpr std::size_t kOverlayCount = static_cast<std::size_t>(Overlay::count);
/** Status-line count, taken from the enum for the same reason. */
constexpr std::size_t kStatusLineCount = static_cast<std::size_t>(StatusLine::count);
/** Every switch the file carries: one per overlay, then one per status line. */
constexpr std::size_t kSwitchCount = kOverlayCount + kStatusLineCount;

/** Every overlay, in Overlay order. The menu lists them and the corner stacks them in it. */
constexpr std::array<Entry, kOverlayCount> kOverlays{
    Entry{"Sunrise Card",
          "sunrise_card",
          "##sunrise_hud_card",
          &overlays::logo::draw,
          true,
          Corner::topLeft,
          0.0F},
    Entry{"Startup Hint",
          "startup_hint",
          "##sunrise_hud_startup_hint",
          &overlays::startup_hint::draw,
          true,
          Corner::topLeft,
          0.0F},
    // Diagnostic overlays start off because an ordinary run does not need them on screen.
    Entry{"Current Status",
          "current_status",
          "##sunrise_hud_status",
          &overlays::status::draw,
          false,
          Corner::topLeft,
          0.0F},
    Entry{"Session",
          "session",
          "##sunrise_hud_session",
          &overlays::session::draw,
          false,
          Corner::topLeft,
          0.0F},
    Entry{"Client Activity Messages",
          "sensor_events",
          "##sunrise_hud_sensor_events",
          &overlays::sensor_events::draw,
          false,
          Corner::topLeft,
          0.0F},
    Entry{"Mission Script",
          "mission_script",
          "##sunrise_hud_mission_script",
          &overlays::mission_script::draw,
          false,
          Corner::topLeft,
          0.0F},
    Entry{"Mission Variables",
          "mission_variables",
          "##sunrise_hud_mission_variables",
          &overlays::mission_variables::draw,
          false,
          Corner::topRight,
          kVariableLogWidth},
};

/** One status line's identity and starting switch state. */
struct LineEntry {
    const char* displayName;
    /** Key the switch file stores this line under. It must outlive every table change. */
    const char* storageKey;
    bool startsOn;
};

/** Every status line, in StatusLine order. The overlay draws them in it. */
constexpr std::array<LineEntry, kStatusLineCount> kStatusLines{
    LineEntry{"Activity", "status_activity", true},
    LineEntry{"Bubble", "status_bubble", true},
    LineEntry{"Slice set", "status_slice_set", true},
    LineEntry{"Closest spawn", "status_closest_spawn", true},
};

/** @return The switch state every overlay starts with. */
[[nodiscard]] constexpr std::array<bool, kOverlayCount> starting_state() noexcept {
    std::array<bool, kOverlayCount> state{};
    for (std::size_t index = 0; index < state.size(); ++index) {
        state[index] = kOverlays[index].startsOn;
    }
    return state;
}

/** @return The switch state every status line starts with. */
[[nodiscard]] constexpr std::array<bool, kStatusLineCount> starting_line_state() noexcept {
    std::array<bool, kStatusLineCount> state{};
    for (std::size_t index = 0; index < state.size(); ++index) {
        state[index] = kStatusLines[index].startsOn;
    }
    return state;
}

/** Switch state. Only the presentation thread reads or writes it. */
std::array<bool, kOverlayCount> g_enabled{starting_state()};
/** Status-line switch state, on the same thread. */
std::array<bool, kStatusLineCount> g_lineEnabled{starting_line_state()};

namespace mission = server::activity::mission;
namespace mission_state = state::activity::mission;
mission::DiagnosticsSnapshot g_bossPrograms{};
mission_state::Snapshot g_bossState{};

/** Finds one mission variable in the current authoritative snapshot. */
[[nodiscard]] const mission_state::VariableValue*
boss_variable(const mission_state::MissionState& state, std::string_view name) noexcept {
    for (std::size_t index = 0; index < state.variableCount; ++index) {
        const auto& variable = state.variables[index];
        if (std::string_view{variable.key.bytes.data(), variable.key.length} == name) {
            return &variable.value;
        }
    }
    return nullptr;
}

// Shared top HUD for mission bosses. Keep phase and immunity in the label row
// so changes in encounter state never add a second health track.
[[nodiscard]] bool draw_boss_health(const ImGuiViewport& viewport, std::string_view name,
                                    std::string_view phase, float health, bool immune,
                                    std::string_view race) noexcept {
    const float scale = scaling::dpi::pixels(1.0F);
    const float width = (std::min)(740.0F * scale, viewport.WorkSize.x * 0.62F);
    if (width < 80.0F * scale || !std::isfinite(health)) return false;
    health = (std::clamp)(health, 0.0F, 1.0F);
    const float left = viewport.WorkPos.x + (viewport.WorkSize.x - width) * 0.5F;
    const float right = left + width;
    const float bottom = viewport.WorkPos.y + (std::max)(viewport.WorkSize.y * 0.065F,
                                                         33.0F * scale + ImGui::GetTextLineHeight());
    const float top = bottom - 7.0F * scale;
    const float labelY = top - ImGui::GetTextLineHeight() - 5.0F * scale;
    auto* draw = ImGui::GetForegroundDrawList();
    if (immune) phase = "IMMUNE";
    float nameEnd = right;
    if (!phase.empty()) {
        const float phaseWidth = ImGui::CalcTextSize(phase.data(), phase.data() + phase.size()).x;
        if (phaseWidth < width * 0.45F) {
            const float phaseX = right - phaseWidth;
            draw->AddText({phaseX + scale, labelY + scale}, IM_COL32(0, 0, 0, 215),
                          phase.data(), phase.data() + phase.size());
            draw->AddText({phaseX, labelY}, IM_COL32(230, 230, 224, 255),
                          phase.data(), phase.data() + phase.size());
            nameEnd = phaseX - 10.0F * scale;
        }
    }
    float nameX = left;
    if (const auto* icon = race_icons::find(race)) {
        const float pixel = 24.0F * scale / 48.0F;
        const float iconTop = top - 3.0F * scale - icon->height * pixel;
        for (int y = 0; y < icon->height; ++y) {
            const char* row = icon->alpha + y * icon->width;
            for (int x = 0; x < icon->width;) {
                const int first = x++;
                while (x < icon->width && row[x] == row[first]) ++x;
                const int alpha = (row[first] - '0') * 255 / 9;
                if (alpha) {
                    draw->AddRectFilled({left + first * pixel, iconTop + y * pixel},
                                        {left + x * pixel, iconTop + (y + 1) * pixel},
                                        IM_COL32(248, 247, 240, alpha));
                }
            }
        }
        nameX += icon->width * pixel + 6.0F * scale;
    }
    draw->PushClipRect({nameX, labelY}, {nameEnd, top}, true);
    draw->AddText({nameX + scale, labelY + scale}, IM_COL32(0, 0, 0, 220),
                  name.data(), name.data() + name.size());
    draw->AddText({nameX, labelY}, IM_COL32(248, 247, 240, 255),
                  name.data(), name.data() + name.size());
    draw->PopClipRect();
    draw->AddRectFilled({left - scale, top - scale}, {right + scale, bottom + scale},
                        IM_COL32(16, 14, 12, 220));
    draw->AddRectFilled({left, top}, {right, bottom}, IM_COL32(58, 53, 48, 230));
    if (health > 0.0F) {
        draw->AddRectFilled({left, top}, {left + width * health, bottom},
                            immune ? IM_COL32(177, 175, 170, 255) : IM_COL32(234, 169, 55, 255));
    }
    return true;
}

/** Draws the Menagerie boss's observed health above the ordinary HUD stack. */
[[nodiscard]] bool draw_menagerie_boss_bar(const ImGuiViewport& viewport) noexcept {
    mission::snapshot(g_bossPrograms);
    for (std::size_t index = 0; index < g_bossPrograms.instanceCount; ++index) {
        const auto& instance = g_bossPrograms.instances[index];
        if (std::string_view{instance.activityId.data()} != "act/007f/0d9ff5e7"
            || !instance.vmActive || instance.vmFaulted
            || !mission_state::state_snapshot(instance.binding, g_bossState)) {
            continue;
        }
        const auto& state = g_bossState.state;
        const auto* stageValue = boss_variable(state, "menagerie_trial.stage");
        if (stageValue == nullptr
            || stageValue->kind != mission_state::VariableValueKind::integer) {
            continue;
        }
        const auto stage = stageValue->integerValue;
        if (stage != 4 && stage != 5) continue;
        const std::string_view observedKey = stage == 4
            ? "menagerie_trial.spawn_observed.4" : "menagerie_trial.spawn_observed.5";
        const std::string_view deadKey = stage == 4
            ? "menagerie_trial.dead.4.1" : "menagerie_trial.dead.5.1";
        const auto* observed = boss_variable(state, observedKey);
        const auto* dead = boss_variable(state, deadKey);
        if (observed == nullptr || observed->kind != mission_state::VariableValueKind::boolean
            || !observed->booleanValue
            || (dead != nullptr && dead->kind == mission_state::VariableValueKind::boolean
                && dead->booleanValue)) {
            continue;
        }
        double health = 1.0;
        if (const auto* value = boss_variable(state, "menagerie_trial.boss.health");
            value != nullptr && value->kind == mission_state::VariableValueKind::real) {
            health = (std::clamp)(value->realValue, 0.0, 1.0);
        }
        return draw_boss_health(viewport, stage == 4 ? "HASAPIKO" : "PAGOURI", {},
                                static_cast<float>(health), false, "vex");
    }
    return false;
}

/**
 * A public-event style banner any mission script can show by writing these variables:
 *   hud.banner.title    string, required: the event name
 *   hud.banner.detail   string: the current objective line
 *   hud.banner.progress number 0..1: the progress bar
 *   hud.banner.heroic   boolean: heroic colours and tag
 *   hud.banner.status   string: "active" (default), "complete" or "failed"
 * Clearing hud.banner.title hides it. It reads the authoritative mission state, like the
 * Menagerie boss bar, so no new script call is needed.
 */
[[nodiscard]] bool draw_script_event_banner(const ImGuiViewport& viewport, float top) noexcept {
    mission::snapshot(g_bossPrograms);
    for (std::size_t index = 0; index < g_bossPrograms.instanceCount; ++index) {
        const auto& instance = g_bossPrograms.instances[index];
        if (!instance.vmActive || instance.vmFaulted
            || !mission_state::state_snapshot(instance.binding, g_bossState)) {
            continue;
        }
        const auto& state = g_bossState.state;
        const auto* title = boss_variable(state, "hud.banner.title");
        if (title == nullptr || title->kind != mission_state::VariableValueKind::string
            || title->stringLength == 0) {
            continue;
        }
        const auto text = [&](std::string_view name) -> std::string_view {
            const auto* value = boss_variable(state, name);
            if (value == nullptr || value->kind != mission_state::VariableValueKind::string) {
                return {};
            }
            return {value->stringValue.data(),
                    (std::min)(static_cast<std::size_t>(value->stringLength),
                               value->stringValue.size())};
        };
        double progress = 0.0;
        if (const auto* value = boss_variable(state, "hud.banner.progress"); value != nullptr) {
            if (value->kind == mission_state::VariableValueKind::real) {
                progress = value->realValue;
            } else if (value->kind == mission_state::VariableValueKind::integer) {
                progress = static_cast<double>(value->integerValue);
            }
        }
        progress = (std::clamp)(progress, 0.0, 1.0);
        bool heroic = false;
        if (const auto* value = boss_variable(state, "hud.banner.heroic");
            value != nullptr && value->kind == mission_state::VariableValueKind::boolean) {
            heroic = value->booleanValue;
        }
        const std::string_view status = text("hud.banner.status");
        const bool complete = status == "complete";
        const bool failed = status == "failed";
        const std::string_view name{title->stringValue.data(),
                                    (std::min)(static_cast<std::size_t>(title->stringLength),
                                               title->stringValue.size())};
        const std::string_view detail = text("hud.banner.detail");

        const float scale = scaling::dpi::pixels(1.0F);
        const float width = (std::min)(460.0F * scale, viewport.WorkSize.x - 80.0F * scale);
        if (width <= 0.0F) {
            return false;
        }
        const ImU32 accent = failed    ? IM_COL32(196, 72, 64, 255)
                             : heroic  ? IM_COL32(232, 186, 74, 255)
                             : complete ? IM_COL32(120, 196, 120, 255)
                                        : IM_COL32(236, 236, 236, 255);
        const ImVec2 start{viewport.WorkPos.x + (viewport.WorkSize.x - width) * 0.5F,
                           viewport.WorkPos.y + top};
        const float pad = 10.0F * scale;
        const float lineHeight = ImGui::GetTextLineHeight();
        const float height = pad * 2.0F + lineHeight * 2.0F + 6.0F * scale + 8.0F * scale;
        ImDrawList* draw = ImGui::GetForegroundDrawList();
        draw->AddRectFilled(start, {start.x + width, start.y + height},
                            IM_COL32(14, 16, 22, 200), 4.0F * scale);
        // A coloured strip on the left, like the game's event banner.
        draw->AddRectFilled(start, {start.x + 4.0F * scale, start.y + height}, accent,
                            4.0F * scale, ImDrawFlags_RoundCornersLeft);
        const char* kind = failed     ? "PUBLIC EVENT FAILED"
                           : complete ? "PUBLIC EVENT COMPLETE"
                           : heroic   ? "HEROIC PUBLIC EVENT"
                                      : "PUBLIC EVENT";
        const float textX = start.x + pad + 4.0F * scale;
        draw->AddText({textX, start.y + pad}, accent, kind);
        const ImVec2 kindSize = ImGui::CalcTextSize(kind);
        draw->AddText({textX + kindSize.x + 10.0F * scale, start.y + pad},
                      IM_COL32(240, 236, 228, 255), name.data(), name.data() + name.size());
        if (!detail.empty()) {
            draw->AddText({textX, start.y + pad + lineHeight + 2.0F * scale},
                          IM_COL32(200, 200, 205, 230), detail.data(),
                          detail.data() + detail.size());
        }
        const ImVec2 barStart{textX, start.y + height - pad - 8.0F * scale};
        const ImVec2 barEnd{start.x + width - pad, barStart.y + 8.0F * scale};
        draw->AddRectFilled(barStart, barEnd, IM_COL32(40, 42, 50, 230), 2.0F * scale);
        if (progress > 0.0) {
            draw->AddRectFilled(
                barStart,
                {barStart.x + (barEnd.x - barStart.x) * static_cast<float>(progress), barEnd.y},
                accent, 2.0F * scale);
        }
        return true;
    }
    return false;
}

/**
 * A raid-style boss bar any mission script can show by writing these variables:
 *   hud.boss.name      string, required: the boss name, drawn in capitals
 *   hud.boss.subtitle  string: a smaller line under the name
 *   hud.boss.health    number 0..1: remaining health (default 1)
 *   hud.boss.race      string: fallen, hive, vex, or cabal
 *   hud.boss.immune    boolean: greys the fill and shows IMMUNE
 *   hud.boss.native_registry, hud.boss.native_slot  integers: the boss's spawning source
 *                      (an entry of context.native_actors); when both are set and the client
 *                      can read that actor, the bar shows its exact native health instead
 * Clearing hud.boss.name hides the bar.
 */
[[nodiscard]] bool draw_script_boss_bar(const ImGuiViewport& viewport) noexcept {
    mission::snapshot(g_bossPrograms);
    for (std::size_t index = 0; index < g_bossPrograms.instanceCount; ++index) {
        const auto& instance = g_bossPrograms.instances[index];
        if (!instance.vmActive || instance.vmFaulted
            || !mission_state::state_snapshot(instance.binding, g_bossState)) {
            continue;
        }
        const auto& state = g_bossState.state;
        const auto* title = boss_variable(state, "hud.boss.name");
        if (title == nullptr || title->kind != mission_state::VariableValueKind::string
            || title->stringLength == 0) {
            continue;
        }
        const auto number = [&](std::string_view name, double fallback) {
            const auto* value = boss_variable(state, name);
            if (value == nullptr) return fallback;
            if (value->kind == mission_state::VariableValueKind::real) return value->realValue;
            if (value->kind == mission_state::VariableValueKind::integer) {
                return static_cast<double>(value->integerValue);
            }
            return fallback;
        };
        float health = static_cast<float>((std::clamp)(number("hud.boss.health", 1.0), 0.0, 1.0));
        // A script that names the boss's spawning source gets the game's own health fraction,
        // read every 100 ms on this machine, instead of its own damage-event estimate.
        const auto* nativeRegistry = boss_variable(state, "hud.boss.native_registry");
        const auto* nativeSlot = boss_variable(state, "hud.boss.native_slot");
        if (nativeRegistry != nullptr && nativeSlot != nullptr
            && nativeRegistry->kind == mission_state::VariableValueKind::integer
            && nativeSlot->kind == mission_state::VariableValueKind::integer) {
            ::sunrise::client::actors::native::Actor actor{};
            std::uint64_t age = 0;
            if (::sunrise::client::actors::native::find(static_cast<std::uint32_t>(nativeRegistry->integerValue),
                                             static_cast<std::uint32_t>(nativeSlot->integerValue),
                                             actor,
                                             age)
                && actor.healthKnown && age < 2000) {
                health = actor.dead ? 0.0F : (std::clamp)(actor.health, 0.0F, 1.0F);
            }
        }
        bool immune = false;
        if (const auto* value = boss_variable(state, "hud.boss.immune");
            value != nullptr && value->kind == mission_state::VariableValueKind::boolean) {
            immune = value->booleanValue;
        }
        std::string_view subtitle;
        if (const auto* value = boss_variable(state, "hud.boss.subtitle");
            value != nullptr && value->kind == mission_state::VariableValueKind::string) {
            subtitle = {value->stringValue.data(),
                        (std::min)(static_cast<std::size_t>(value->stringLength),
                                   value->stringValue.size())};
        }
        // Upper-case copy of the name, as the game prints boss names.
        char name[80]{};
        const std::size_t nameLength = (std::min)(static_cast<std::size_t>(title->stringLength),
                                                  (std::min)(title->stringValue.size(), sizeof(name) - 1));
        for (std::size_t c = 0; c < nameLength; ++c) {
            const char ch = title->stringValue[c];
            name[c] = (ch >= 'a' && ch <= 'z') ? static_cast<char>(ch - 'a' + 'A') : ch;
        }
        std::string_view race;
        if (const auto* value = boss_variable(state, "hud.boss.race");
            value && value->kind == mission_state::VariableValueKind::string) {
            race = {value->stringValue.data(),
                    (std::min)(static_cast<std::size_t>(value->stringLength), value->stringValue.size())};
        } else if (std::string_view{name} == "SKOLAS, THE PERFECTED") {
            race = "fallen";
        }
        return draw_boss_health(viewport, name, subtitle, health, immune, race);
    }
    return false;
}

/** @param overlay Overlay to check. @return True when it names one of the entries. */
[[nodiscard]] bool in_range(Overlay overlay) noexcept {
    return static_cast<std::size_t>(overlay) < kOverlayCount;
}

/** @param line Status line to check. @return True when it names one of the entries. */
[[nodiscard]] bool in_range(StatusLine line) noexcept {
    return static_cast<std::size_t>(line) < kStatusLineCount;
}

/** @return Every file key paired with the switch state it holds now. */
[[nodiscard]] std::array<store::Switch, kSwitchCount> switch_state() noexcept {
    std::array<store::Switch, kSwitchCount> switches{};
    for (std::size_t index = 0; index < kOverlayCount; ++index) {
        switches[index] = {kOverlays[index].storageKey, g_enabled[index]};
    }
    for (std::size_t index = 0; index < kStatusLineCount; ++index) {
        switches[kOverlayCount + index] = {kStatusLines[index].storageKey, g_lineEnabled[index]};
    }
    return switches;
}

/** Writes every switch. One file holds them all, so a partial save would drop the rest. */
void save_switches() noexcept {
    const std::array<store::Switch, kSwitchCount> switches = switch_state();
    // A failed write is reported by the store and never blocks the switch itself.
    (void)store::save(switches);
}

/**
 * Draws one overlay at a fixed position.
 * @param entry Overlay to draw.
 * @param position The corner the window hangs from, in final framebuffer pixels.
 * @return Size the window took, which is known only after its content is submitted.
 */
[[nodiscard]] ImVec2 draw_overlay(const Entry& entry, const ImVec2& position) noexcept {
    ImGui::SetNextWindowPos(position,
                            ImGuiCond_Always,
                            entry.corner == Corner::topRight ? kTopRightPivot : kTopLeftPivot);
    if (entry.width > 0.0F) {
        // Pinning both bounds keeps the width fixed while the height still fits the content.
        const float width = scaling::dpi::pixels(entry.width);
        ImGui::SetNextWindowSizeConstraints({width, 0.0F}, {width, FLT_MAX});
    }
    const bool submitContents = ImGui::Begin(entry.windowId, nullptr, kOverlayFlags);
    if (submitContents) {
        entry.draw();
    }
    // Read inside the window, because the size belongs to it and not to the caller's window.
    const ImVec2 size = ImGui::GetWindowSize();
    ImGui::End();
    return size;
}

} // namespace

/** Resolves the switch file and applies the saved state. */
void initialize(void* module) noexcept {
    store::initialize(module);
    std::array<store::Switch, kSwitchCount> switches = switch_state();
    store::load(switches);
    for (std::size_t index = 0; index < kOverlayCount; ++index) {
        g_enabled[index] = switches[index].on;
    }
    for (std::size_t index = 0; index < kStatusLineCount; ++index) {
        g_lineEnabled[index] = switches[kOverlayCount + index].on;
    }
}

/** Drops the switch file path. The switches keep their values. */
void shutdown() noexcept {
    store::shutdown();
}

/** @return The overlay's menu label. */
const char* display_name(Overlay overlay) noexcept {
    return in_range(overlay) ? kOverlays[static_cast<std::size_t>(overlay)].displayName : "";
}

/** @return True while the overlay draws. */
bool enabled(Overlay overlay) noexcept {
    return in_range(overlay) && g_enabled[static_cast<std::size_t>(overlay)];
}

/** Switches one overlay on or off and saves every switch at once. */
void set_enabled(Overlay overlay, bool on) noexcept {
    if (!in_range(overlay) || g_enabled[static_cast<std::size_t>(overlay)] == on) {
        return;
    }
    g_enabled[static_cast<std::size_t>(overlay)] = on;
    save_switches();
}

/** @return The status line's menu label. */
const char* display_name(StatusLine line) noexcept {
    return in_range(line) ? kStatusLines[static_cast<std::size_t>(line)].displayName : "";
}

/** @return True while the status line draws. */
bool enabled(StatusLine line) noexcept {
    return in_range(line) && g_lineEnabled[static_cast<std::size_t>(line)];
}

/** Switches one status line on or off and saves every switch at once. */
void set_enabled(StatusLine line, bool on) noexcept {
    if (!in_range(line) || g_lineEnabled[static_cast<std::size_t>(line)] == on) {
        return;
    }
    g_lineEnabled[static_cast<std::size_t>(line)] = on;
    save_switches();
}

/** Draws every enabled overlay, stacked down its own top corner. */
bool draw(bool interfaceEnabled) noexcept {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    if (!interfaceEnabled || viewport == nullptr) {
        return false;
    }
    const float margin = scaling::dpi::pixels(kViewportMargin);
    const float gap = scaling::dpi::pixels(kOverlayGap);
    ImVec2 left{viewport->WorkPos.x + margin, viewport->WorkPos.y + margin};
    ImVec2 right{viewport->WorkPos.x + viewport->WorkSize.x - margin, viewport->WorkPos.y + margin};
    // The startup hint sits beside whichever overlay owns the top-left row, or on the corner when
    // none does. It is drawn after the stack, once that row's width is known, and never moves
    // the overlays stacked below.
    ImVec2 hintPosition = left;
    bool leftDrawn = false;
    bool drawn = false;
    for (std::size_t index = 0; index < kOverlayCount; ++index) {
        if (!g_enabled[index] || static_cast<Overlay>(index) == Overlay::startupHint) {
            continue;
        }
        const bool topRight = kOverlays[index].corner == Corner::topRight;
        ImVec2& position = topRight ? right : left;
        const ImVec2 size = draw_overlay(kOverlays[index], position);
        if (!topRight && !leftDrawn) {
            hintPosition.x += size.x + gap;
            leftDrawn = true;
        }
        position.y += size.y + gap;
        drawn = true;
    }
    // The hint's switch and layout row live at its own overlay index.
    constexpr auto hintIndex = static_cast<std::size_t>(Overlay::startupHint);
    if (g_enabled[hintIndex]) {
        const float progress = overlays::startup_hint::progress();
        if (progress > 0.0F) {
            // One style alpha fades the hint and everything drawn inside it together.
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, progress);
            (void)draw_overlay(kOverlays[hintIndex], hintPosition);
            ImGui::PopStyleVar();
            drawn = true;
        }
    }
    const bool bossBar = draw_menagerie_boss_bar(*viewport) || draw_script_boss_bar(*viewport);
    const bool banner = draw_script_event_banner(
        *viewport, bossBar ? viewport->WorkSize.y * 0.15F : scaling::dpi::pixels(20.0F));
    return bossBar || banner || drawn;
}

} // namespace sunrise::core::ui::hud
