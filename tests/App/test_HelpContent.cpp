/// @file test_HelpContent.cpp
/// @brief The Help window's ImGui-free content (#172): the shortcut filter, the tab overview and the
/// column reference's platform notes.

#include "App/HelpContent.h"
#include "App/KeyboardShortcuts.h"
#include "App/ProcessColumnConfig.h"

#include <gtest/gtest.h>

#include <cstddef>

namespace App::HelpContent
{
namespace
{

using KeyboardShortcuts::SHORTCUT_HELP;

TEST(HelpContentTest, ABlankFilterKeepsEveryShortcut)
{
    for (const char* blank : {"", " ", "\t "})
    {
        EXPECT_EQ(countMatches(matchingShortcuts(blank)), SHORTCUT_HELP.size());
    }
}

TEST(HelpContentTest, TheFilterNarrowsTheListIgnoringCase)
{
    const ShortcutMask quit = matchingShortcuts("QUIT");
    ASSERT_EQ(countMatches(quit), 1U);
    for (std::size_t i = 0; i < SHORTCUT_HELP.size(); ++i)
    {
        EXPECT_EQ(quit.at(i), SHORTCUT_HELP.at(i).keys == "F10") << i;
    }

    // Keys match too, and an area's name keeps every shortcut in it.
    const std::size_t ctrl = countMatches(matchingShortcuts("ctrl"));
    EXPECT_GT(ctrl, 1U);
    EXPECT_LT(ctrl, SHORTCUT_HELP.size());
    const std::size_t details = countMatches(matchingShortcuts(" process details "));
    EXPECT_GE(details, 2U);
    EXPECT_LT(details, SHORTCUT_HELP.size());
}

TEST(HelpContentTest, AFilterMatchingNothingKeepsNothing)
{
    EXPECT_EQ(countMatches(matchingShortcuts("no such shortcut")), 0U);
}

TEST(HelpContentTest, EveryTabHasANameAndASummary)
{
    for (const TabSummary& tab : TAB_OVERVIEW)
    {
        EXPECT_FALSE(tab.name.empty());
        EXPECT_FALSE(tab.summary.empty());
    }
}

TEST(HelpContentTest, OnlyTheNetworkColumnsCarryAPlatformNote)
{
    for (const ProcessColumn col : allProcessColumns())
    {
        const bool network = col == ProcessColumn::NetSent || col == ProcessColumn::NetReceived;
        EXPECT_EQ(columnNotePlatform(col), network ? "On Windows:" : "") << toIndex(col);
    }
}

} // namespace
} // namespace App::HelpContent
