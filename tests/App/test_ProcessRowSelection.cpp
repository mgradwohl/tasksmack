/// @file test_ProcessRowSelection.cpp
/// @brief Which selected Processes row is the primary one (#1926): the row state each row is drawn in,
/// the primary row's marker geometry, and the Details tab's text while several rows are selected.

#include "App/Panels/ProcessRowSelection.h"

#include <gtest/gtest.h>
#include <imgui.h>

#include <cstddef>
#include <string>

namespace App::ProcessRowSelection
{
namespace
{

// ========== Row state ==========

TEST(ProcessRowSelectionTest, AnUnselectedRowIsNeverMarked)
{
    EXPECT_EQ(rowState(false, false, 0), RowState::None);
    EXPECT_EQ(rowState(false, false, 3), RowState::None);
    // The primary process taken out of the selection by Ctrl+click is drawn unselected.
    EXPECT_EQ(rowState(false, true, 2), RowState::None);
}

TEST(ProcessRowSelectionTest, ALoneSelectedRowIsPlainlySelected)
{
    EXPECT_EQ(rowState(true, true, 1), RowState::Selected);
}

TEST(ProcessRowSelectionTest, OnlyThePrimaryRowOfSeveralIsMarked)
{
    for (const std::size_t count : {std::size_t{2}, std::size_t{3}, std::size_t{500}})
    {
        EXPECT_EQ(rowState(true, true, count), RowState::Primary) << count;
        EXPECT_EQ(rowState(true, false, count), RowState::Selected) << count;
    }
}

// ========== Marker geometry ==========

TEST(ProcessRowSelectionTest, TheMarkerIsABarAtTheLeftEdgeAndAnOutline)
{
    const Marker marker = markerFor(ImVec2(10.0F, 100.0F), ImVec2(810.0F, 120.0F), 0.0F, 1000.0F, 18.0F);
    EXPECT_FLOAT_EQ(marker.barMin.x, 10.0F);
    EXPECT_FLOAT_EQ(marker.barMax.x, 14.0F); // round(18 * 0.2) = 4 px
    EXPECT_FLOAT_EQ(marker.barMin.y, 100.0F);
    EXPECT_FLOAT_EQ(marker.barMax.y, 120.0F);
    EXPECT_FLOAT_EQ(marker.outlineMin.x, 10.0F);
    EXPECT_FLOAT_EQ(marker.outlineMax.x, 810.0F);
    EXPECT_FLOAT_EQ(marker.outlineMin.y, 100.0F);
    EXPECT_FLOAT_EQ(marker.outlineMax.y, 120.0F);
    EXPECT_FLOAT_EQ(marker.outlineThickness, 1.0F);
}

TEST(ProcessRowSelectionTest, TheMarkerScalesWithTheTextButStaysVisible)
{
    const Marker small = markerFor(ImVec2(0.0F, 0.0F), ImVec2(500.0F, 10.0F), 0.0F, 500.0F, 6.0F);
    EXPECT_FLOAT_EQ(small.barMax.x - small.barMin.x, MIN_BAR_WIDTH_PX);
    EXPECT_FLOAT_EQ(small.outlineThickness, 1.0F);
    const Marker large = markerFor(ImVec2(0.0F, 0.0F), ImVec2(500.0F, 40.0F), 0.0F, 500.0F, 32.0F);
    EXPECT_FLOAT_EQ(large.barMax.x - large.barMin.x, 6.0F); // round(32 * 0.2)
    EXPECT_FLOAT_EQ(large.outlineThickness, 2.0F);
}

// The row spans every column, also those scrolled out of view: the marker keeps to the part in view.
TEST(ProcessRowSelectionTest, TheMarkerKeepsToTheVisibleSpan)
{
    const Marker marker = markerFor(ImVec2(-300.0F, 0.0F), ImVec2(1500.0F, 20.0F), 20.0F, 900.0F, 18.0F);
    EXPECT_FLOAT_EQ(marker.barMin.x, 20.0F);
    EXPECT_FLOAT_EQ(marker.barMax.x, 24.0F);
    EXPECT_FLOAT_EQ(marker.outlineMin.x, 20.0F);
    EXPECT_FLOAT_EQ(marker.outlineMax.x, 900.0F);
}

TEST(ProcessRowSelectionTest, ARowNarrowerThanTheBarIsNotOverdrawn)
{
    const Marker marker = markerFor(ImVec2(0.0F, 0.0F), ImVec2(3.0F, 20.0F), 0.0F, 100.0F, 18.0F);
    EXPECT_FLOAT_EQ(marker.barMax.x, 3.0F);
    EXPECT_FLOAT_EQ(marker.outlineMax.x, 3.0F);
}

// ========== Details tab text ==========

TEST(ProcessRowSelectionTest, TheDetailsTabNamesThePidOnlyWhileSeveralAreSelected)
{
    std::string text;
    formatDetailsTabText(text, "rust-analyzer", 3476, 1);
    EXPECT_EQ(text, "rust-analyzer");
    formatDetailsTabText(text, "rust-analyzer", 3476, 2);
    EXPECT_EQ(text, "rust-analyzer (PID 3476)");
    formatDetailsTabText(text, "rust-analyzer", 3476, 0);
    EXPECT_EQ(text, "rust-analyzer");
}

} // namespace
} // namespace App::ProcessRowSelection
