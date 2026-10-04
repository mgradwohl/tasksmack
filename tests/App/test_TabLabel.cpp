/// @file test_TabLabel.cpp
/// @brief Tests for App::TabLabel: main tab labels whose ImGui ID does not follow their text (#1140)

#include "App/TabLabel.h"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

namespace App
{
namespace
{

TEST(TabLabelTest, LabelShowsIconAndTextAndEndsInItsStableId)
{
    const std::string label = TabLabel::make("[i]", "bash", TabLabel::PROCESS_DETAILS_TAB_ID);
    EXPECT_EQ(label, "[i]  bash###ProcessDetailsTab");
    EXPECT_EQ(TabLabel::idPart(label), TabLabel::PROCESS_DETAILS_TAB_ID);
}

TEST(TabLabelTest, RenamingTheProcessKeepsTheTabId)
{
    // #1140: `exec sleep 600` in the selected shell keeps its PID, so the selection survives and
    // only the tab's text changes. ImGui identifies the tab by the hashed part of its label, which
    // must therefore be the same before and after, or ImGui switches to another tab.
    const std::string before = TabLabel::make("[i]", "bash", TabLabel::PROCESS_DETAILS_TAB_ID);
    const std::string after = TabLabel::make("[i]", "sleep", TabLabel::PROCESS_DETAILS_TAB_ID);
    EXPECT_NE(before, after);
    EXPECT_EQ(TabLabel::idPart(before), TabLabel::idPart(after));
}

TEST(TabLabelTest, ANameContainingTheSeparatorDoesNotChangeTheId)
{
    // ImGui restarts its hash at every "###", so the last one -- the suffix -- decides the ID even
    // when a process name has one of its own.
    const std::string label = TabLabel::make("[i]", "odd###name", TabLabel::PROCESS_DETAILS_TAB_ID);
    EXPECT_EQ(TabLabel::idPart(label), TabLabel::PROCESS_DETAILS_TAB_ID);
}

TEST(TabLabelTest, MainTabsHaveDistinctIds)
{
    EXPECT_NE(TabLabel::SYSTEM_TAB_ID, TabLabel::PROCESSES_TAB_ID);
    EXPECT_NE(TabLabel::SYSTEM_TAB_ID, TabLabel::PROCESS_DETAILS_TAB_ID);
    EXPECT_NE(TabLabel::PROCESSES_TAB_ID, TabLabel::PROCESS_DETAILS_TAB_ID);
}

TEST(TabLabelTest, IdPartOfALabelWithoutSeparatorIsTheWholeLabel)
{
    constexpr std::string_view plain = "Processes";
    EXPECT_EQ(TabLabel::idPart(plain), plain);
}

} // namespace
} // namespace App
