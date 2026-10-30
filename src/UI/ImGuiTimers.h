#pragma once

// Dear ImGui's own timers, for a frame loop that draws static content only when something changes
// (#3156). ImGui advances them on each frame, by the real time since the last one (io.DeltaTime), and
// shows their effect on the frame that crosses a threshold: the text cursor's blink, a tooltip after
// its hover delay, a submenu opened by hovering. No input or publication comes when they elapse, so
// without a frame then they would show only at the loop's slow floor. secondsUntilImGuiTimerChange()
// says when the next one elapses, and requestFramesForImGuiTimers() asks the loop for that frame
// (Core::AnimationRequest::requestFrameWithin()).
//
// Two more things ImGui finishes over later frames, which a frame woken by a publication would leave
// half done until the floor: a window that appears hidden on its first frame (a tooltip, a popup, a
// dialog) or auto-fits over its next ones, and a table's columns, which auto-fit over three frames. And
// a mouse button held still repeats on its own (a scrollbar's paging): while one is down the loop keeps
// the idle rate, as it did before.

#include "Core/AnimationRequest.h"

#include <imgui.h>
#include <imgui_internal.h> // ImGuiContext: the hover, stationary and blink timers have no public accessor

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <limits>

namespace UI
{

/// The text cursor's blink, as ImGui's InputText draws it (imgui_widgets.cpp): shown while its timer is
/// at most 0 (just after typing) or within the first CURSOR_BLINK_ON_SECONDS of each
/// CURSOR_BLINK_PERIOD_SECONDS.
inline constexpr double CURSOR_BLINK_PERIOD_SECONDS = 1.2;
inline constexpr double CURSOR_BLINK_ON_SECONDS = 0.8;
/// How long ImGui waits, hovering a menu item with the mouse still, before it opens the submenu
/// (BeginMenuEx()'s timer fallback). Hard-coded in ImGui, like the blink.
inline constexpr double MENU_HOVER_OPEN_SECONDS = 0.3;
/// Added to each deadline, so the frame lands just after the threshold rather than on it: ImGui
/// compares with <= and >=, and the timers are floats.
inline constexpr double IMGUI_TIMER_MARGIN_SECONDS = 0.005;
/// The frame rate while a mouse button is held: the 20 fps idle rate every frame had before #3156, so
/// what repeats while it is held (a scrollbar paging, a repeat button) does so as it did.
inline constexpr double HELD_MOUSE_BUTTON_FPS = 20.0;

/// What secondsUntilImGuiTimerChange() reads, copied out of the ImGui context after a frame's widgets.
struct ImGuiTimerState
{
    double cursorBlinkSeconds = 0.0;      ///< The active text input's blink timer (ImGuiInputTextState::CursorAnim)
    double hoverDelaySeconds = 0.0;       ///< How long the delayed-hover item has been hovered (HoverItemDelayTimer)
    double mouseStationarySeconds = 0.0;  ///< How long the mouse has been still (MouseStationaryTimer)
    double hoveredIdSeconds = 0.0;        ///< How long the hovered item has been hovered (HoveredIdTimer)
    double hoverDelayShortSeconds = 0.0;  ///< style.HoverDelayShort
    double hoverDelayNormalSeconds = 0.0; ///< style.HoverDelayNormal
    double hoverStationarySeconds = 0.0;  ///< style.HoverStationaryDelay
    bool cursorBlinking = false;          ///< A text input is active, and its cursor blinks (io.ConfigInputTextCursorBlink)
    bool hoverDelayPending = false;       ///< An item asked for a delayed hover this frame (HoverItemDelayId)
    bool itemHovered = false;             ///< An item is hovered (HoveredId)
    bool layoutSettling = false;          ///< A window or table finishes its layout on the next frame (layoutNeedsAnotherFrame())
};

namespace Detail
{
/// The time until @p timer reaches @p threshold, or infinity once it has.
[[nodiscard]] constexpr double untilThreshold(double timer, double threshold) noexcept
{
    return (timer < threshold) ? threshold - timer : std::numeric_limits<double>::infinity();
}
} // namespace Detail

/// The time until the text cursor next shows or hides, for blink timer @p cursorBlinkSeconds.
[[nodiscard]] inline double secondsUntilCursorBlink(double cursorBlinkSeconds) noexcept
{
    if (!std::isfinite(cursorBlinkSeconds))
    {
        return std::numeric_limits<double>::infinity();
    }
    if (cursorBlinkSeconds <= 0.0)
    {
        // Shown since the last keystroke; the blink starts once the timer passes 0, still shown.
        return CURSOR_BLINK_ON_SECONDS - cursorBlinkSeconds;
    }
    const double phase = std::fmod(cursorBlinkSeconds, CURSOR_BLINK_PERIOD_SECONDS);
    return (phase <= CURSOR_BLINK_ON_SECONDS) ? CURSOR_BLINK_ON_SECONDS - phase : CURSOR_BLINK_PERIOD_SECONDS - phase;
}

/// The time until the next of ImGui's timers crosses a threshold that changes what is drawn, margin
/// included; 0 while a layout settles over the next frame; infinity when nothing is pending.
[[nodiscard]] inline double secondsUntilImGuiTimerChange(const ImGuiTimerState& state) noexcept
{
    if (state.layoutSettling)
    {
        return 0.0;
    }
    double soonest = std::numeric_limits<double>::infinity();
    if (state.cursorBlinking)
    {
        soonest = std::min(soonest, secondsUntilCursorBlink(state.cursorBlinkSeconds));
    }
    if (state.hoverDelayPending)
    {
        // A delayed tooltip: shown once the delay has passed (DelayShort, DelayNormal) with the mouse
        // still for HoverStationaryDelay (ImGuiHoveredFlags_Stationary).
        for (const double threshold : {state.hoverDelayShortSeconds, state.hoverDelayNormalSeconds})
        {
            soonest = std::min(soonest, Detail::untilThreshold(state.hoverDelaySeconds, threshold));
        }
        soonest = std::min(soonest, Detail::untilThreshold(state.mouseStationarySeconds, state.hoverStationarySeconds));
    }
    if (state.itemHovered)
    {
        // A menu item opens its submenu once hovered, with the mouse still, for MENU_HOVER_OPEN_SECONDS.
        soonest = std::min(soonest,
                           std::max(Detail::untilThreshold(state.hoveredIdSeconds, MENU_HOVER_OPEN_SECONDS),
                                    Detail::untilThreshold(state.mouseStationarySeconds, MENU_HOVER_OPEN_SECONDS)));
    }
    return std::isfinite(soonest) ? soonest + IMGUI_TIMER_MARGIN_SECONDS : soonest;
}

/// Whether a window drawn this frame is hidden for its first frames or still auto-fitting, or a table
/// drawn this frame still auto-fits a column: the next frame shows them as they settle.
/// Not const: ImPool::TryGetMapData() isn't, though it only reads.
[[nodiscard]] inline bool layoutNeedsAnotherFrame(ImGuiContext& g)
{
    for (const ImGuiWindow* window : g.Windows)
    {
        if (window->Active && (window->HiddenFramesCanSkipItems > 0 || window->HiddenFramesCannotSkipItems > 0 ||
                               window->AutoFitFramesX > 0 || window->AutoFitFramesY > 0))
        {
            return true;
        }
    }
    for (int index = 0; index < g.Tables.GetMapSize(); ++index)
    {
        const ImGuiTable* table = g.Tables.TryGetMapData(index);
        if (table == nullptr || table->LastFrameActive != g.FrameCount)
        {
            continue;
        }
        for (const ImGuiTableColumn& column : table->Columns)
        {
            if (column.AutoFitQueue != 0)
            {
                return true;
            }
        }
    }
    return false;
}

/// The timers of the current ImGui context, after this frame's widgets (before ImGui::Render()).
[[nodiscard]] inline ImGuiTimerState currentImGuiTimerState()
{
    ImGuiContext& g = *ImGui::GetCurrentContext(); // non-const for layoutNeedsAnotherFrame()
    const bool textInputActive = g.ActiveId != 0 && g.InputTextState.ID == g.ActiveId;
    return ImGuiTimerState{
        .cursorBlinkSeconds = static_cast<double>(g.InputTextState.CursorAnim),
        .hoverDelaySeconds = static_cast<double>(g.HoverItemDelayTimer),
        .mouseStationarySeconds = static_cast<double>(g.MouseStationaryTimer),
        .hoveredIdSeconds = static_cast<double>(g.HoveredIdTimer),
        .hoverDelayShortSeconds = static_cast<double>(g.Style.HoverDelayShort),
        .hoverDelayNormalSeconds = static_cast<double>(g.Style.HoverDelayNormal),
        .hoverStationarySeconds = static_cast<double>(g.Style.HoverStationaryDelay),
        .cursorBlinking = textInputActive && g.IO.ConfigInputTextCursorBlink,
        .hoverDelayPending = g.HoverItemDelayId != 0,
        .itemHovered = g.HoveredId != 0,
        .layoutSettling = layoutNeedsAnotherFrame(g),
    };
}

/// Asks the frame loop for the frame on which the next of ImGui's timers shows, or a layout settles
/// (#3156), and for the idle rate while a mouse button is held. Call once a frame, after its widgets and
/// before ImGui::Render().
inline void requestFramesForImGuiTimers()
{
    Core::AnimationRequest::requestFrameWithin(secondsUntilImGuiTimerChange(currentImGuiTimerState()));
    if (ImGui::IsAnyMouseDown())
    {
        Core::AnimationRequest::request(HELD_MOUSE_BUTTON_FPS);
    }
}

} // namespace UI
