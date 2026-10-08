/// @file test_KeyboardShortcuts.cpp
/// @brief The function-key map and its gate (#170): each key's action, nothing while typing, while a
/// popup or modal is open, or with a modifier held, and the About dialog's list matching the map.

#include "App/KeyboardShortcuts.h"

#include <gtest/gtest.h>

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
    EXPECT_EQ(actionFor(FunctionKey::F1, idle), ShortcutAction::ShowAbout);
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

TEST(KeyboardShortcutsTest, AboutListStartsWithTheFunctionKeys)
{
    // The About dialog's list leads with the function keys, worded as the map words them, so the two
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

} // namespace
} // namespace App::KeyboardShortcuts
