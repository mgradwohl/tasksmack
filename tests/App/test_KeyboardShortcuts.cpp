/// @file test_KeyboardShortcuts.cpp
/// @brief The function-key map and its gate (#170): each key's action, nothing while typing, while a
/// popup or modal is open, or with a modifier held, and the Help window's list matching the map.

#include "App/KeyboardShortcuts.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>

namespace App::KeyboardShortcuts
{
namespace
{

TEST(KeyboardShortcutsTest, EachFunctionKeyMapsToItsAction)
{
    const InputState idle{};
    EXPECT_EQ(actionFor(FunctionKey::F1, idle), ShortcutAction::ShowHelp);
    EXPECT_EQ(actionFor(FunctionKey::F2, idle), ShortcutAction::OpenSettings);
    EXPECT_EQ(actionFor(FunctionKey::F5, idle), ShortcutAction::ToggleTreeView);
    EXPECT_EQ(actionFor(FunctionKey::F9, idle), ShortcutAction::KillSelected);
    EXPECT_EQ(actionFor(FunctionKey::F10, idle), ShortcutAction::Quit);
}

TEST(KeyboardShortcutsTest, NoKeyFiresWhileBlocked)
{
    // One blocking condition at a time: typing, a popup or modal, each modifier.
    const std::array<InputState, 6> blocked{{
        {.textInputActive = true},
        {.popupOpen = true},
        {.ctrl = true},
        {.shift = true},
        {.alt = true},
        {.super = true},
    }};
    for (std::size_t i = 0; i < blocked.size(); ++i)
    {
        SCOPED_TRACE("blocking state " + std::to_string(i));
        EXPECT_FALSE(bareShortcutsAllowed(blocked.at(i)));
        for (const FunctionKeyBinding& binding : FUNCTION_KEY_BINDINGS)
        {
            SCOPED_TRACE(std::string(binding.keyLabel));
            EXPECT_EQ(actionFor(binding.key, blocked.at(i)), ShortcutAction::None);
        }
    }
}

TEST(KeyboardShortcutsTest, BindingsAreUniqueAndLabelled)
{
    for (std::size_t i = 0; i < FUNCTION_KEY_BINDINGS.size(); ++i)
    {
        const FunctionKeyBinding& binding = FUNCTION_KEY_BINDINGS.at(i);
        SCOPED_TRACE(std::string(binding.keyLabel));
        EXPECT_NE(binding.action, ShortcutAction::None);
        EXPECT_FALSE(binding.keyLabel.empty());
        EXPECT_FALSE(binding.description.empty());
        for (std::size_t j = i + 1; j < FUNCTION_KEY_BINDINGS.size(); ++j)
        {
            EXPECT_NE(binding.key, FUNCTION_KEY_BINDINGS.at(j).key);
            EXPECT_NE(binding.action, FUNCTION_KEY_BINDINGS.at(j).action);
        }
    }
}

TEST(KeyboardShortcutsTest, KeyLabelsForTooltips)
{
    EXPECT_EQ(keyLabelFor(ShortcutAction::ToggleTreeView), "F5");
    EXPECT_EQ(keyLabelFor(ShortcutAction::KillSelected), "F9");
    EXPECT_EQ(keyLabelFor(ShortcutAction::Quit), "F10");
    EXPECT_TRUE(keyLabelFor(ShortcutAction::None).empty());
}

TEST(KeyboardShortcutsTest, F1OpensHelpNotAbout)
{
    // #172: F1 is Help; About is reached from Help's footer and from Settings.
    EXPECT_EQ(actionFor(FunctionKey::F1, InputState{}), ShortcutAction::ShowHelp);
    EXPECT_EQ(keyLabelFor(ShortcutAction::ShowHelp), "F1");
}

TEST(KeyboardShortcutsTest, EveryAreaHasAHeadingAndAShortcut)
{
    for (const ShortcutArea area : SHORTCUT_AREAS)
    {
        EXPECT_FALSE(areaLabel(area).empty());
        EXPECT_TRUE(std::ranges::any_of(SHORTCUT_HELP, [area](const ShortcutHelpEntry& entry) { return entry.area == area; }));
    }
}

TEST(KeyboardShortcutsTest, HelpListStartsWithTheFunctionKeys)
{
    // The Help window's list leads with the function keys, worded as the map words them, so the two
    // cannot drift apart.
    ASSERT_GE(SHORTCUT_HELP.size(), FUNCTION_KEY_BINDINGS.size());
    for (std::size_t i = 0; i < FUNCTION_KEY_BINDINGS.size(); ++i)
    {
        SCOPED_TRACE(std::string(FUNCTION_KEY_BINDINGS.at(i).keyLabel));
        EXPECT_EQ(SHORTCUT_HELP.at(i).keys, FUNCTION_KEY_BINDINGS.at(i).keyLabel);
        EXPECT_EQ(SHORTCUT_HELP.at(i).description, FUNCTION_KEY_BINDINGS.at(i).description);
    }
    for (const ShortcutHelpEntry& entry : SHORTCUT_HELP)
    {
        EXPECT_FALSE(entry.keys.empty());
        EXPECT_FALSE(entry.description.empty());
    }
}

TEST(KeyboardShortcutsTest, OnlyF5AndF9DependOnTheTab)
{
    EXPECT_TRUE(isTabShortcut(ShortcutAction::ToggleTreeView));
    EXPECT_TRUE(isTabShortcut(ShortcutAction::KillSelected));
    EXPECT_FALSE(isTabShortcut(ShortcutAction::ShowHelp));
    EXPECT_FALSE(isTabShortcut(ShortcutAction::OpenSettings));
    EXPECT_FALSE(isTabShortcut(ShortcutAction::Quit));
    EXPECT_FALSE(isTabShortcut(ShortcutAction::None));
}

TEST(KeyboardShortcutsTest, TabShortcutsGoToTheTabOnShow)
{
    EXPECT_EQ(tabShortcutTarget(ShortcutAction::ToggleTreeView, "Processes"), ShortcutTarget::Processes);
    EXPECT_EQ(tabShortcutTarget(ShortcutAction::ToggleTreeView, "ProcessDetails"), ShortcutTarget::None);
    EXPECT_EQ(tabShortcutTarget(ShortcutAction::ToggleTreeView, "SystemOverview"), ShortcutTarget::None);
    EXPECT_EQ(tabShortcutTarget(ShortcutAction::KillSelected, "Processes"), ShortcutTarget::Processes);
    EXPECT_EQ(tabShortcutTarget(ShortcutAction::KillSelected, "ProcessDetails"), ShortcutTarget::ProcessDetails);
    EXPECT_EQ(tabShortcutTarget(ShortcutAction::KillSelected, "SystemOverview"), ShortcutTarget::None); // No selection
    EXPECT_EQ(tabShortcutTarget(ShortcutAction::ShowHelp, "Processes"), ShortcutTarget::None);
    EXPECT_EQ(tabShortcutTarget(ShortcutAction::Quit, "ProcessDetails"), ShortcutTarget::None);
}

TEST(KeyboardShortcutsTest, AFrameRequestIsTakenOnce)
{
    FrameRequest request;
    EXPECT_FALSE(request.pending());
    EXPECT_FALSE(request.take());
    request.request();
    EXPECT_TRUE(request.pending());
    EXPECT_TRUE(request.take());
    EXPECT_FALSE(request.take()); // Acted on once
    request.expire();
    EXPECT_FALSE(request.take());
}

TEST(KeyboardShortcutsTest, AFrameRequestNothingTookIsDroppedAtTheFrameEnd)
{
    // F9 for a tab that was not drawn this frame: nothing takes it, the frame ends, and a later
    // render of that tab must not act on it (Copilot review on #1471).
    FrameRequest request;
    request.request();
    request.expire();
    EXPECT_FALSE(request.pending());
    EXPECT_FALSE(request.take());

    // A request made and taken in the same frame is unaffected by that frame's expiry.
    request.request();
    EXPECT_TRUE(request.take());
    request.expire();
    EXPECT_FALSE(request.take());
}

} // namespace
} // namespace App::KeyboardShortcuts
