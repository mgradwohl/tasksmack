#pragma once

// The ImGui side of the keyboard shortcuts (#160, #170): reads this frame's keys and dialog state and
// hands them to the ImGui-free decisions in KeyboardShortcuts.h and ProcessTableNavigation.h. Kept out
// of ShellLayer.cpp and ProcessesPanel.cpp, which the test binary does not link, so the real key
// handling runs headless in test_KeyboardInputRender.cpp. Every function here needs a frame.

#include "App/KeyboardShortcuts.h"
#include "App/Panels/ProcessTableNavigation.h"

namespace App::KeyboardInput
{

/// This frame's text-input, popup and modifier state.
[[nodiscard]] KeyboardShortcuts::InputState currentInputState();

/// The function-key shortcut pressed this frame (KeyboardShortcuts::FUNCTION_KEY_BINDINGS), or None:
/// also None while typing, while a popup or modal is open, or with a modifier held. A held key does
/// not repeat.
[[nodiscard]] KeyboardShortcuts::ShortcutAction pollFunctionKeys();

/// Whether the Processes table should take navigation keys this frame: the window being drawn (the
/// panel's) or one of its children is hovered or focused, nothing is being typed and no popup or
/// modal is open. Call inside the panel's window, outside the table.
[[nodiscard]] bool tableNavigationArmed();

/// Takes the navigation keys from ImGui's own keyboard navigation while the table is armed, so an
/// arrow press moves the selection rather than ImGui's focus rectangle (and does not scroll twice).
/// Call every frame the table is armed: ownership is read by ImGui at the next frame's start.
/// Left/Right are taken only in tree view, where they collapse and expand. @p ownerId is any ImGui ID
/// stable for the panel.
void claimNavigationKeys(unsigned int ownerId, bool treeView);

/// The navigation move pressed this frame (ProcessTableNavigation::commandFor()), None when there is
/// none. Arrows, j/k and Page Up/Down repeat while held; Home/End and g/G do not. Left/Right only in
/// tree view.
[[nodiscard]] ProcessTableNavigation::NavCommand pollNavigationCommand(bool treeView);

/// The height of the current table's scrolling area below its frozen header, for
/// ProcessTableNavigation::pageStep(). Call inside the table; 0 outside one.
[[nodiscard]] float tableScrollViewHeight();

/// Scrolls the current table just far enough that the last item drawn (a row's selectable) is in
/// view below the frozen header, without scrolling sideways. Call inside the table, right after it.
void scrollLastItemIntoView();

} // namespace App::KeyboardInput
