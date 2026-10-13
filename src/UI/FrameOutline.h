#pragma once

// The outline that makes a checkbox, text field, combo box or radio button visible against the
// surface it sits on (#1923).
//
// Every bundled theme fills these controls with [ui.frame] background, within 1.0:1 to 1.8:1 of the
// window, card or popup behind them, and ImGui draws them without a border (FrameBorderSize 0). An
// unchecked checkbox then read as a plain label -- invisible outright in Windows Light and Ubuntu
// Light -- and a text field was a faint tint recognisable only by its placeholder.
//
// The outline is scoped to those controls rather than switched on globally: ImGui draws every frame
// border in the one ImGuiCol_Border, so a global FrameBorderSize would also outline every button,
// collapsing header, chart frame and modal title bar, and recolour every popup's edge. Buttons and
// headers carry their own fills and need no outline; the theme's quieter window border stays theirs.
//
// So draw a checkbox with outlinedCheckbox(), a combo with beginOutlinedCombo() / endOutlinedCombo(),
// and a text field or radio button inside a FrameOutlineScope.

#include "UI/Theme.h"

#include <imgui.h>

namespace UI::Widgets
{

/// The outline's thickness in pixels. A hairline, like the window and child borders, and unscaled
/// for the same reason (Theme::applyImGuiStyle()): it reads as an edge, not as part of the content.
inline constexpr float FRAME_OUTLINE_SIZE = 1.0F;

/// Pushes the outline: FRAME_OUTLINE_SIZE in the theme's frameBorder, which is held to
/// ColorContrast::FRAME_BORDER_CONTRAST_MIN on the window, cards and popups. Pair with
/// popFrameOutline(). Layout does not change: ImGui draws the border inside the frame's rectangle.
inline void pushFrameOutline()
{
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, FRAME_OUTLINE_SIZE);
    ImGui::PushStyleColor(ImGuiCol_Border, Theme::get().scheme().frameBorder);
}

/// Pops what pushFrameOutline() pushed.
inline void popFrameOutline()
{
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}

/// While alive, frame-drawn controls -- text fields, checkboxes, radio buttons -- are outlined.
/// Construct one immediately before the control, in a block of its own, so nothing after it is.
class FrameOutlineScope
{
  public:
    FrameOutlineScope()
    {
        pushFrameOutline();
    }

    ~FrameOutlineScope()
    {
        popFrameOutline();
    }

    FrameOutlineScope(const FrameOutlineScope&) = delete;
    FrameOutlineScope& operator=(const FrameOutlineScope&) = delete;
    FrameOutlineScope(FrameOutlineScope&&) = delete;
    FrameOutlineScope& operator=(FrameOutlineScope&&) = delete;
};

/// ImGui::Checkbox(), outlined so the box shows while unticked.
inline bool outlinedCheckbox(const char* label, bool* value)
{
    const FrameOutlineScope outline;
    return ImGui::Checkbox(label, value);
}

/// ImGui::BeginCombo() with the preview box outlined. Call endOutlinedCombo(), not EndCombo(), when it
/// returns true. The outline stays pushed while the drop-down is open, so the pushes and pops pair up
/// outside the drop-down's window rather than straddling it; the drop-down's edge takes the outline
/// colour too, which ties it to the box it opened from.
inline bool beginOutlinedCombo(const char* label, const char* preview, ImGuiComboFlags flags = ImGuiComboFlags_None)
{
    pushFrameOutline();
    const bool open = ImGui::BeginCombo(label, preview, flags);
    if (!open)
    {
        popFrameOutline();
    }
    return open;
}

/// Ends a combo beginOutlinedCombo() opened.
inline void endOutlinedCombo()
{
    ImGui::EndCombo();
    popFrameOutline();
}

} // namespace UI::Widgets
