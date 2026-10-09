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
    ShowHelp,       ///< F1: the Help window, which lists these shortcuts (#172); About is reached from it
    OpenSettings,   ///< F2: the Settings dialog
    ToggleTreeView, ///< F5: Processes list <-> tree (Processes tab only)
    KillSelected,   ///< F9: the Kill confirm dialog for the selected process(es); never kills directly
    Quit,           ///< F10: the window's normal close request, so settings are saved
};

/// One function key, what it does, and how the Help window and tooltips name it.
struct FunctionKeyBinding
{
    FunctionKey key = FunctionKey::F1;
    ShortcutAction action = ShortcutAction::None;
    std::string_view keyLabel;
    std::string_view description;
};

/// The function-key map, in key order. The single source for the shell's dispatch and for the
/// shortcut list in the Help window.
inline constexpr std::array<FunctionKeyBinding, 5> FUNCTION_KEY_BINDINGS{{
    {.key = FunctionKey::F1, .action = ShortcutAction::ShowHelp, .keyLabel = "F1", .description = "Help: shortcuts, columns and tabs"},
    {.key = FunctionKey::F2, .action = ShortcutAction::OpenSettings, .keyLabel = "F2", .description = "Settings"},
    {.key = FunctionKey::F5, .action = ShortcutAction::ToggleTreeView, .keyLabel = "F5", .description = "List / tree view"},
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

/// Where a shortcut works: the Help window groups its list by this, in this order (#172).
enum class ShortcutArea : std::uint8_t
{
    Global,
    Processes,
    ProcessDetails,
};

inline constexpr std::array<ShortcutArea, 3> SHORTCUT_AREAS{ShortcutArea::Global, ShortcutArea::Processes, ShortcutArea::ProcessDetails};

/// The heading the Help window gives @p area.
[[nodiscard]] constexpr std::string_view areaLabel(ShortcutArea area) noexcept
{
    switch (area)
    {
    case ShortcutArea::Global:
        return "Global";
    case ShortcutArea::Processes:
        return "Processes";
    case ShortcutArea::ProcessDetails:
        return "Process Details";
    }
    return {};
}

/// One line of the Help window's shortcut list.
struct ShortcutHelpEntry
{
    std::string_view keys;
    std::string_view description;
    ShortcutArea area = ShortcutArea::Global;
};

/// Every keyboard shortcut, for the Help window (#172): the function keys above, then the Processes
/// table's navigation (ProcessTableNavigation.h), Process Details' keys and the older chords.
inline constexpr std::array<ShortcutHelpEntry, 18> SHORTCUT_HELP{{
    {.keys = "F1", .description = "Help: shortcuts, columns and tabs", .area = ShortcutArea::Global},
    {.keys = "F2", .description = "Settings", .area = ShortcutArea::Global},
    {.keys = "F5", .description = "List / tree view", .area = ShortcutArea::Processes},
    {.keys = "F9", .description = "Kill the selected process(es) (asks first)", .area = ShortcutArea::Processes},
    {.keys = "F10", .description = "Quit", .area = ShortcutArea::Global},
    {.keys = "Up / Down, k / j", .description = "Previous / next row", .area = ShortcutArea::Processes},
    {.keys = "Page Up / Page Down", .description = "One page up / down", .area = ShortcutArea::Processes},
    {.keys = "Home / End, g / G", .description = "First / last row", .area = ShortcutArea::Processes},
    {.keys = "Left / Right", .description = "Tree view: collapse / expand, then parent / first child", .area = ShortcutArea::Processes},
    {.keys = "Ctrl + click", .description = "Add a row to, or remove it from, the selection", .area = ShortcutArea::Processes},
    {.keys = "Shift + click",
     .description = "Select the rows from the last one clicked (Ctrl + Shift adds them)",
     .area = ShortcutArea::Processes},
    {.keys = "Ctrl + A", .description = "Select every row shown", .area = ShortcutArea::Processes},
    {.keys = "Hold Ctrl", .description = "Pause updates while held", .area = ShortcutArea::Processes},
    {.keys = "F9", .description = "Kill the process shown (asks first)", .area = ShortcutArea::ProcessDetails},
    {.keys = "Left / Right, Page Up / Page Down, Home / End, 0",
     .description = "Priority slider, when focused: step by 1, step by 5, highest / lowest, normal",
     .area = ShortcutArea::ProcessDetails},
    {.keys = "Ctrl + = / Ctrl + -, Ctrl + keypad + / -", .description = "Larger / smaller text", .area = ShortcutArea::Global},
    {.keys = "Ctrl + Shift + M", .description = "Render metrics overlay", .area = ShortcutArea::Global},
    {.keys = "Alt + Space / Ctrl + Space", .description = "Window menu", .area = ShortcutArea::Global},
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
