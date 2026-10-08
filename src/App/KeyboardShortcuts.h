#pragma once

// The application's keyboard shortcuts (#160, #170): the htop-style function keys and the gate that
// keeps them, and the Processes table's navigation keys, from firing while the user is typing or a
// dialog is up. The older chords (Ctrl+=/-, Ctrl+Shift+M, Alt/Ctrl+Space) are handled elsewhere and
// are not gated by it. ImGui-free, so the
// key map and the gate are unit-tested directly (test_KeyboardShortcuts.cpp); the thin ImGui
// adapter that reads this frame's keys is App/KeyboardInput.cpp, and the Processes table's
// row-stepping math is App/Panels/ProcessTableNavigation.h.

#include <array>
#include <cstdint>
#include <string_view>
#include <utility>

namespace App::KeyboardShortcuts
{

/// The function keys TaskSmack binds, as htop does.
enum class FunctionKey : std::uint8_t
{
    F1,
    F2,
    F5,
    F9,
    F10,
};

/// What a function key asks the shell to do.
enum class ShortcutAction : std::uint8_t
{
    None,
    ShowAbout,      ///< F1: the About dialog, which lists these shortcuts (there is no separate help)
    OpenSettings,   ///< F2: the Settings dialog
    ToggleTreeView, ///< F5: Processes list <-> tree (Processes tab only)
    KillSelected,   ///< F9: the Kill confirm dialog for the selected process(es); never kills directly
    Quit,           ///< F10: the window's normal close request, so settings are saved
};

/// One function key, what it does, and how the About dialog and tooltips name it.
struct FunctionKeyBinding
{
    FunctionKey key = FunctionKey::F1;
    ShortcutAction action = ShortcutAction::None;
    std::string_view keyLabel;
    std::string_view description;
};

/// The function-key map, in key order. The single source for the shell's dispatch and for the
/// shortcut list in the About dialog.
inline constexpr std::array<FunctionKeyBinding, 5> FUNCTION_KEY_BINDINGS{{
    {.key = FunctionKey::F1, .action = ShortcutAction::ShowAbout, .keyLabel = "F1", .description = "About and keyboard shortcuts"},
    {.key = FunctionKey::F2, .action = ShortcutAction::OpenSettings, .keyLabel = "F2", .description = "Settings"},
    {.key = FunctionKey::F5, .action = ShortcutAction::ToggleTreeView, .keyLabel = "F5", .description = "Processes: list / tree view"},
    {.key = FunctionKey::F9,
     .action = ShortcutAction::KillSelected,
     .keyLabel = "F9",
     .description = "Kill the selected process(es) (asks first)"},
    {.key = FunctionKey::F10, .action = ShortcutAction::Quit, .keyLabel = "F10", .description = "Quit"},
}};

/// The keyboard and dialog state a shortcut is checked against.
struct InputState
{
    bool textInputActive = false; ///< A text field has the keyboard (ImGuiIO::WantTextInput)
    bool popupOpen = false;       ///< Any popup, menu or modal dialog is open
    bool ctrl = false;
    bool shift = false;
    bool alt = false;
    bool super = false;
};

/// Whether a bare (unmodified) shortcut key may act at all: never while a text field is being typed
/// in, nor while a popup or modal is open (its own keys, Escape included, belong to it). A held
/// modifier means another chord: Ctrl is the Processes pane's freeze gesture (#928) and the font-size
/// chords, Shift+F10 is ImGui's context-menu key, Alt+F4 the window manager's.
[[nodiscard]] constexpr bool bareShortcutsAllowed(const InputState& state) noexcept
{
    return !state.textInputActive && !state.popupOpen && !state.ctrl && !state.shift && !state.alt && !state.super;
}

/// What @p key does in @p state: its binding when bare shortcuts are allowed, otherwise None.
[[nodiscard]] constexpr ShortcutAction actionFor(FunctionKey key, const InputState& state) noexcept
{
    if (!bareShortcutsAllowed(state))
    {
        return ShortcutAction::None;
    }
    for (const FunctionKeyBinding& binding : FUNCTION_KEY_BINDINGS)
    {
        if (binding.key == key)
        {
            return binding.action;
        }
    }
    return ShortcutAction::None;
}

/// The label of the key bound to @p action ("F5"), or empty when none is, for tooltips.
[[nodiscard]] constexpr std::string_view keyLabelFor(ShortcutAction action) noexcept
{
    for (const FunctionKeyBinding& binding : FUNCTION_KEY_BINDINGS)
    {
        if (binding.action == action)
        {
            return binding.keyLabel;
        }
    }
    return {};
}

/// One line of the About dialog's shortcut list.
struct ShortcutHelpEntry
{
    std::string_view keys;
    std::string_view description;
};

/// Every keyboard shortcut, for the About dialog: the function keys above, then the Processes table's
/// navigation (ProcessTableNavigation.h) and the chords that predate them.
inline constexpr std::array<ShortcutHelpEntry, 16> SHORTCUT_HELP{{
    {.keys = "F1", .description = "About and keyboard shortcuts"},
    {.keys = "F2", .description = "Settings"},
    {.keys = "F5", .description = "Processes: list / tree view"},
    {.keys = "F9", .description = "Kill the selected process(es) (asks first)"},
    {.keys = "F10", .description = "Quit"},
    {.keys = "Up / Down, k / j", .description = "Processes: previous / next row"},
    {.keys = "Page Up / Page Down", .description = "Processes: one page up / down"},
    {.keys = "Home / End, g / G", .description = "Processes: first / last row"},
    {.keys = "Left / Right", .description = "Tree view: collapse / expand, then parent / first child"},
    {.keys = "Ctrl + click", .description = "Processes: add a row to, or remove it from, the selection"},
    {.keys = "Shift + click", .description = "Processes: select the rows from the last one clicked (Ctrl + Shift adds them)"},
    {.keys = "Ctrl + A", .description = "Processes: select every row shown"},
    {.keys = "Hold Ctrl", .description = "Processes: pause updates while held"},
    {.keys = "Ctrl + = / Ctrl + -, Ctrl + keypad + / -", .description = "Larger / smaller text"},
    {.keys = "Ctrl + Shift + M", .description = "Render metrics overlay"},
    {.keys = "Alt + Space / Ctrl + Space", .description = "Window menu"},
}};

/// Which panel a tab-sensitive shortcut goes to.
enum class ShortcutTarget : std::uint8_t
{
    None,
    Processes,      ///< The Processes tab's table
    ProcessDetails, ///< The Process Details tab
};

/// Whether @p action depends on the tab on show (F5, F9): the shell dispatches these only after the
/// tab bar has taken this frame's tab click, so they go to the tab actually drawn this frame (#170).
[[nodiscard]] constexpr bool isTabShortcut(ShortcutAction action) noexcept
{
    return action == ShortcutAction::ToggleTreeView || action == ShortcutAction::KillSelected;
}

/// The panel a tab-sensitive @p action goes to with @p activeTab (the shell's tab event name) on show:
/// F5 to the Processes table only; F9 to Processes or Process Details, each with its own confirm.
/// Anything else, or another tab (the System tab has no selection): None.
[[nodiscard]] constexpr ShortcutTarget tabShortcutTarget(ShortcutAction action, std::string_view activeTab) noexcept
{
    if (action == ShortcutAction::ToggleTreeView)
    {
        return (activeTab == "Processes") ? ShortcutTarget::Processes : ShortcutTarget::None;
    }
    if (action == ShortcutAction::KillSelected)
    {
        if (activeTab == "Processes")
        {
            return ShortcutTarget::Processes;
        }
        if (activeTab == "ProcessDetails")
        {
            return ShortcutTarget::ProcessDetails;
        }
    }
    return ShortcutTarget::None;
}

/// A one-shot request that lives for a single frame (#170): made by a shortcut, taken by the panel's
/// render that frame, and expired at the frame's end if nothing took it, so a request made for a panel
/// that was not drawn can never act on a later frame.
class FrameRequest
{
  public:
    void request() noexcept
    {
        m_Pending = true;
    }

    /// Whether a request was made this frame; taking it clears it, so it is acted on once.
    [[nodiscard]] bool take() noexcept
    {
        return std::exchange(m_Pending, false);
    }

    /// The frame is over: drop a request nothing took.
    void expire() noexcept
    {
        m_Pending = false;
    }

    [[nodiscard]] bool pending() const noexcept
    {
        return m_Pending;
    }

  private:
    bool m_Pending = false;
};

} // namespace App::KeyboardShortcuts
