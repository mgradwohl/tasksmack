/// @file test_TabLabel.cpp
/// @brief Tests for App::TabLabel: main tab labels whose ImGui ID does not follow their text (#1140)

#include "App/TabLabel.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <stdexcept>
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

/// `label`'s visible part (TabLabel::visiblePart(), ImGui's "##" rule) with the zero-width spaces taken
/// out: what a reader sees.
std::string shownText(std::string_view label)
{
    std::string shown{TabLabel::visiblePart(label)};
    for (std::size_t at = shown.find(TabLabel::HASH_BREAK); at != std::string::npos; at = shown.find(TabLabel::HASH_BREAK, at))
    {
        shown.erase(at, TabLabel::HASH_BREAK.size());
    }
    return shown;
}

TEST(TabLabelTest, IdPartMirrorsImGuisHashRestarts)
{
    // ImHashStr restarts at the first "###" it meets and skips it whole, so in "a####b" the restart
    // is at the first "#" and the hashed part is "#b", not the "b" after the last "###".
    EXPECT_EQ(TabLabel::idPart("a####b"), "#b");
    EXPECT_EQ(TabLabel::idPart("a###b###c"), "c");
    EXPECT_EQ(TabLabel::idPart("a######b"), "b");
    EXPECT_EQ(TabLabel::idPart("a###"), "");
}

TEST(TabLabelTest, VisiblePartEndsAtTheFirstDoubleHash)
{
    EXPECT_EQ(TabLabel::visiblePart("foo##bar"), "foo");
    EXPECT_EQ(TabLabel::visiblePart("foo#bar"), "foo#bar");
    EXPECT_EQ(TabLabel::visiblePart("[i]  foo###Id"), "[i]  foo");
}

TEST(TabLabelTest, ANameContainingHashesIsShownInFull)
{
    // #1244: ImGui hides everything from a label's first "##", so a process named "foo##bar" showed
    // as "foo". Every name must be shown whole, and the tab must keep its ID.
    for (const std::string_view name : {"foo##bar", "foo###bar", "foo####bar", "##", "###", "#", "a#b", "#lead", "trail#", "trail##"})
    {
        const std::string label = TabLabel::make("[i]", name, TabLabel::PROCESS_DETAILS_TAB_ID);
        EXPECT_EQ(shownText(label), std::string{"[i]  "} + std::string{name}) << "name: " << name;
        EXPECT_EQ(TabLabel::idPart(label), TabLabel::PROCESS_DETAILS_TAB_ID) << "name: " << name;
    }
}

TEST(TabLabelTest, ATrailingHashDoesNotRunIntoTheIdSeparator)
{
    // Unescaped, "trail#" + "###ProcessDetailsTab" is "trail####ProcessDetailsTab": ImGui restarts its
    // hash at the name's "#", hashes "#ProcessDetailsTab", and the tab's ID follows the name again.
    const std::string label = TabLabel::make("[i]", "trail#", TabLabel::PROCESS_DETAILS_TAB_ID);
    EXPECT_EQ(TabLabel::idPart(label), TabLabel::PROCESS_DETAILS_TAB_ID);
    EXPECT_EQ(label, std::string{"[i]  trail#"} + std::string{TabLabel::HASH_BREAK} + "###ProcessDetailsTab");
}

TEST(TabLabelTest, TextWithoutDoubleOrTrailingHashesIsUnchanged)
{
    std::string label;
    TabLabel::appendDisplayText(label, "a#b c#d");
    EXPECT_EQ(label, "a#b c#d");
}

TEST(TabLabelTest, ProcessDetailsWindowLabelShowsTheNameUnderAFixedId)
{
    // #1326 / #1244: the window title shows the whole name and keeps the "###ProcessDetails" ID, so
    // ImGui keeps the window's settings when the selection changes.
    const std::string named = TabLabel::makeProcessDetailsWindowLabel("[i]", "foo##bar");
    EXPECT_EQ(shownText(named), "[i]  foo##bar");
    EXPECT_EQ(TabLabel::idPart(named), "ProcessDetails");

    const std::string unnamed = TabLabel::makeProcessDetailsWindowLabel("[i]", "");
    EXPECT_EQ(shownText(unnamed), "[i]  Process Details");
    EXPECT_EQ(TabLabel::idPart(unnamed), "ProcessDetails");
}

TEST(TabLabelTest, CachedLabelRebuildsOnlyWhenTheTextChanges)
{
    // #1326: the same text returns the same cached string without calling the builder again.
    TabLabel::CachedLabel cache;
    int builds = 0;
    const auto build = [&builds](std::string_view text)
    {
        ++builds;
        return TabLabel::make("[i]", text, TabLabel::PROCESS_DETAILS_WINDOW_ID);
    };

    EXPECT_EQ(cache.get("bash", build), TabLabel::make("[i]", "bash", TabLabel::PROCESS_DETAILS_WINDOW_ID));
    EXPECT_EQ(builds, 1);

    EXPECT_EQ(cache.get("bash", build), TabLabel::make("[i]", "bash", TabLabel::PROCESS_DETAILS_WINDOW_ID));
    EXPECT_EQ(builds, 1) << "the same text must not rebuild the label";

    EXPECT_EQ(cache.get("sleep", build), TabLabel::make("[i]", "sleep", TabLabel::PROCESS_DETAILS_WINDOW_ID));
    EXPECT_EQ(builds, 2);
    EXPECT_EQ(cache.label(), TabLabel::make("[i]", "sleep", TabLabel::PROCESS_DETAILS_WINDOW_ID));
}

TEST(TabLabelTest, CachedLabelBuildsAnEmptyTextOnFirstUse)
{
    // An empty key must still be built once: nothing has been cached yet.
    TabLabel::CachedLabel cache;
    int builds = 0;
    const auto build = [&builds](std::string_view text)
    {
        ++builds;
        return TabLabel::makeProcessDetailsWindowLabel("[i]", text);
    };
    EXPECT_EQ(shownText(cache.get("", build)), "[i]  Process Details");
    EXPECT_EQ(shownText(cache.get("", build)), "[i]  Process Details");
    EXPECT_EQ(builds, 1);
}

TEST(TabLabelTest, CachedLabelKeepsItsLabelAndKeyTogetherWhenBuildingThrows)
{
    // A render exception is caught and the app carries on. A cache that recorded the new text before
    // building would then return the old label for the new text from then on.
    TabLabel::CachedLabel cache;
    const auto build = [](std::string_view text)
    {
        return std::string{text};
    };
    EXPECT_EQ(cache.get("old", build), "old");

    const auto failingBuild = [](std::string_view) -> std::string
    {
        throw std::runtime_error("build failed");
    };
    EXPECT_THROW(static_cast<void>(cache.get("new", failingBuild)), std::runtime_error);
    EXPECT_EQ(cache.label(), "old");

    // The next frame retries and builds the new label.
    EXPECT_EQ(cache.get("new", build), "new");
}

} // namespace
} // namespace App
