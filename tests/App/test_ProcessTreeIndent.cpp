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

} // namespace
} // namespace App
