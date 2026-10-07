#include "App/Panels/ProcessTreeIndent.h"

#include <gtest/gtest.h>

namespace App
{
namespace
{

using ProcessTreeIndent::clampedIndent;

// Mirrors ProcessesPanel's constants closely enough to keep these cases readable.
constexpr float INDENT_PER_LEVEL = 16.0F;
constexpr float RESERVED = 60.0F; // expander + minimum readable name slice
constexpr float ROOMY_CELL = 400.0F;

TEST(ProcessTreeIndentTest, RootRowsAreNotIndented)
{
    EXPECT_FLOAT_EQ(clampedIndent(0, INDENT_PER_LEVEL, ROOMY_CELL, RESERVED), 0.0F);
}

TEST(ProcessTreeIndentTest, NegativeDepthIsTreatedAsRoot)
{
    // Defensive: depth is produced by the tree flatten walk, but a negative value must not produce
    // a negative indent, which ImGui would apply as an outdent into the previous column.
    EXPECT_FLOAT_EQ(clampedIndent(-3, INDENT_PER_LEVEL, ROOMY_CELL, RESERVED), 0.0F);
}

TEST(ProcessTreeIndentTest, IndentScalesWithDepthWhenThereIsRoom)
{
    EXPECT_FLOAT_EQ(clampedIndent(1, INDENT_PER_LEVEL, ROOMY_CELL, RESERVED), 16.0F);
    EXPECT_FLOAT_EQ(clampedIndent(5, INDENT_PER_LEVEL, ROOMY_CELL, RESERVED), 80.0F);
}

TEST(ProcessTreeIndentTest, IndentIsClampedSoControlsStayInsideTheCell)
{
    // A 100px cell reserving 60px can only afford 40px of indent, even though depth 5 asks for 80.
    EXPECT_FLOAT_EQ(clampedIndent(5, INDENT_PER_LEVEL, 100.0F, RESERVED), 40.0F);
}

TEST(ProcessTreeIndentTest, CellNarrowerThanTheReservationYieldsNoIndent)
{
    // This is the case that makes deep parents untoggleable if it goes wrong: when the user has
    // dragged the Name column narrower than the expander needs, the indent must give way entirely
    // rather than push the expand/collapse button out of its cell.
    EXPECT_FLOAT_EQ(clampedIndent(9, INDENT_PER_LEVEL, 40.0F, RESERVED), 0.0F);
    EXPECT_FLOAT_EQ(clampedIndent(9, INDENT_PER_LEVEL, 0.0F, RESERVED), 0.0F);
}

TEST(ProcessTreeIndentTest, IndentNeverExceedsWhatTheDepthAsksFor)
{
    // A very wide cell must not stretch the indent past depth * indentPerLevel; the clamp is a
    // ceiling, not a target.
    EXPECT_FLOAT_EQ(clampedIndent(2, INDENT_PER_LEVEL, 10'000.0F, RESERVED), 32.0F);
}

TEST(ProcessTreeIndentTest, NonPositiveIndentPerLevelDisablesIndenting)
{
    EXPECT_FLOAT_EQ(clampedIndent(4, 0.0F, ROOMY_CELL, RESERVED), 0.0F);
    EXPECT_FLOAT_EQ(clampedIndent(4, -8.0F, ROOMY_CELL, RESERVED), 0.0F);
}

TEST(ProcessTreeIndentTest, ResultIsNeverNegative)
{
    // std::max(0, cellWidth - reserved) is what guarantees this; a negative indent would shift the
    // name left into the PID column.
    for (const int depth : {1, 3, 7})
    {
        for (const float cell : {0.0F, 5.0F, 59.0F, 61.0F})
        {
            EXPECT_GE(clampedIndent(depth, INDENT_PER_LEVEL, cell, RESERVED), 0.0F);
        }
    }
}

// ========== Font-relative sizes (#971) ==========

// The em multiples reproduce the fixed pixel sizes they replace at the reference em (the Medium
// preset on a 1.0 display scale: 8pt at 96 DPI = 32/3 px), so the tree is unchanged there.
TEST(ProcessTreeIndentTest, EmSizesReproduceTheOldPixelsAtTheReferenceFont)
{
    constexpr float REFERENCE_EM = 32.0F / 3.0F;
    EXPECT_FLOAT_EQ(ProcessTreeIndent::INDENT_PER_LEVEL_EM * REFERENCE_EM, 16.0F);
    EXPECT_FLOAT_EQ(ProcessTreeIndent::MIN_NAME_WIDTH_EM * REFERENCE_EM, 72.0F);
}

// The reported case: at Extra Large on a 175% display an em is about 33px, and a 16px indent was
// under half a character. One level must now be visibly more than a character wide.
TEST(ProcessTreeIndentTest, IndentPerLevelIsMoreThanACharacterAtAnyFont)
{
    for (const float em : {8.0F, 32.0F / 3.0F, 33.0F, 37.33F})
    {
        EXPECT_GT(ProcessTreeIndent::INDENT_PER_LEVEL_EM * em, em);
    }
}

// ========== Tree view's Name width (#1209) ==========

TEST(ProcessTreeIndentTest, TreeViewWidensANarrowNameColumn)
{
    constexpr float em = 16.0F;
    EXPECT_FLOAT_EQ(ProcessTreeIndent::treeViewNameWidth(120.0F, em), ProcessTreeIndent::TREE_NAME_WIDTH_EM * em);
    // Room for the expander, a few levels of indent and the list's own name width.
    EXPECT_GT(ProcessTreeIndent::TREE_NAME_WIDTH_EM,
              ProcessTreeIndent::MIN_NAME_WIDTH_EM + (3.0F * ProcessTreeIndent::INDENT_PER_LEVEL_EM));
}

TEST(ProcessTreeIndentTest, TreeViewNeverNarrowsANameColumnTheUserWidened)
{
    EXPECT_FLOAT_EQ(ProcessTreeIndent::treeViewNameWidth(900.0F, 16.0F), 900.0F);
    EXPECT_FLOAT_EQ(ProcessTreeIndent::treeViewNameWidth(120.0F, 0.0F), 120.0F); // No usable em: unchanged
}

TEST(ProcessTreeIndentTest, LeavingTreeViewRestoresOnlyAWidthTheUserDidNotChange)
{
    EXPECT_TRUE(ProcessTreeIndent::shouldRestoreNameWidth(360.0F, 360.0F));
    EXPECT_TRUE(ProcessTreeIndent::shouldRestoreNameWidth(360.0F, 360.5F));  // Layout rounding
    EXPECT_FALSE(ProcessTreeIndent::shouldRestoreNameWidth(360.0F, 420.0F)); // Resized in tree view: kept
    EXPECT_FALSE(ProcessTreeIndent::shouldRestoreNameWidth(0.0F, 120.0F));   // Tree view did not widen it
}

TEST(ProcessTreeIndentTest, AutomaticTreeNameWidthIsNotSavedAsTheListsWidth)
{
    // #1209: closing in tree view saved tree view's Name width, which the next launch -- in list view
    // -- then restored. The list's width is saved instead while Name is still at tree view's width.
    const auto saved =
        ProcessTreeIndent::nameWidthToSave(/*treeView=*/true, /*widenedTo=*/360.0F, /*currentWidth=*/360.0F, /*widthBeforeTree=*/120.0F);
    ASSERT_TRUE(saved.has_value());
    EXPECT_FLOAT_EQ(saved.value_or(-1.0F), 120.0F); // value_or: ASSERT above has checked it
}

TEST(ProcessTreeIndentTest, UserChosenNameWidthIsSavedAsItIs)
{
    // Resized in tree view: the user's width, saved as it is.
    EXPECT_FALSE(ProcessTreeIndent::nameWidthToSave(true, 360.0F, 420.0F, 120.0F).has_value());
    // Tree view did not widen it (already wide enough).
    EXPECT_FALSE(ProcessTreeIndent::nameWidthToSave(true, 0.0F, 500.0F, 500.0F).has_value());
    // List view: whatever it is, it is the list's.
    EXPECT_FALSE(ProcessTreeIndent::nameWidthToSave(false, 360.0F, 360.0F, 120.0F).has_value());
}

} // namespace
} // namespace App
