#pragma once

// Hold Ctrl to freeze the Processes pane, as Windows Task Manager does (#928). While frozen the panel
// stops adopting new snapshot generations, so rows neither update nor re-sort and the row under the
// pointer stays put long enough to click it. Sampling carries on underneath; releasing Ctrl adopts the
// latest generation straight away.
//
// Pure, so the decision is unit-testable without an ImGui context: the panel gathers the inputs from
// ImGuiIO and the window state each frame and feeds them in.

namespace App::ProcessDisplayFreeze
{

/// How long the gesture must be held before the pane freezes. Long enough that the Ctrl of a chord
/// (Ctrl+= for font size, Ctrl+Shift+M for Render Metrics) is joined by its other key before the
/// freeze shows, so a chord does not flash the indicator; short enough to be done well before the
/// pointer reaches a row.
inline constexpr double ENGAGE_DELAY_SECONDS = 0.15;

/// One frame's input state.
struct Inputs
{
    bool appFocused = false;        ///< The application window has OS focus (!ImGuiIO::AppFocusLost)
    bool ctrlHeld = false;          ///< ImGuiIO::KeyCtrl
    bool otherModifierHeld = false; ///< Shift, Alt or Super held as well
    bool otherKeyHeld = false;      ///< Any non-modifier keyboard key held as well (the C of Ctrl+C)
    bool textInputActive = false;   ///< A text field (the filter box) has keyboard focus (ImGuiIO::WantTextInput)
    bool panelHovered = false;      ///< The pointer is over the Processes pane
    bool panelFocused = false;      ///< The Processes pane has ImGui focus
};

/// True when this frame's inputs are the freeze gesture: Ctrl alone, held in a focused application
/// over (or with focus in) the Processes pane, with no text field taking the keystrokes.
[[nodiscard]] constexpr bool isFreezeGesture(const Inputs& in) noexcept
{
    return in.appFocused && in.ctrlHeld && !in.otherModifierHeld && !in.otherKeyHeld && !in.textInputActive &&
           (in.panelHovered || in.panelFocused);
}

/// Turns the per-frame gesture into the frozen state: the gesture must last ENGAGE_DELAY_SECONDS,
/// and once another key joins Ctrl (a shortcut) the freeze stays off until Ctrl is released, so the
/// gaps between repeated Ctrl+= presses don't freeze the pane either.
class Tracker
{
  public:
    /// Feeds one frame's inputs, at `nowSeconds` on any monotonic clock, and returns whether the pane
    /// is frozen this frame.
    constexpr bool update(const Inputs& in, double nowSeconds) noexcept
    {
        if (!in.ctrlHeld || !in.appFocused)
        {
            m_ChordLatched = false;
        }
        else if (in.otherModifierHeld || in.otherKeyHeld)
        {
            m_ChordLatched = true;
        }

        if (!isFreezeGesture(in) || m_ChordLatched)
        {
            m_GestureActive = false;
            m_Frozen = false;
            return false;
        }

        if (!m_GestureActive)
        {
            m_GestureActive = true;
            m_GestureStartSeconds = nowSeconds;
        }
        m_Frozen = (nowSeconds - m_GestureStartSeconds) >= ENGAGE_DELAY_SECONDS;
        return m_Frozen;
    }

    /// Unfreezes at once and forgets any gesture in progress (e.g. the Processes tab was left).
    constexpr void reset() noexcept
    {
        *this = Tracker{};
    }

    [[nodiscard]] constexpr bool frozen() const noexcept
    {
        return m_Frozen;
    }

  private:
    double m_GestureStartSeconds = 0.0;
    bool m_GestureActive = false;
    bool m_ChordLatched = false;
    bool m_Frozen = false;
};

} // namespace App::ProcessDisplayFreeze
