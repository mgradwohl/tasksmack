#include "App/Panels/ProcessTableFlags.h"

#include <gtest/gtest.h>
#include <imgui.h>

namespace App
{
namespace
{

using ProcessTableFlags::BASE;
using ProcessTableFlags::forProcessTable;

TEST(ProcessTableFlagsTest, ListViewOffersSorting)
{
    EXPECT_NE(forProcessTable(/*treeViewEnabled=*/false) & ImGuiTableFlags_Sortable, 0);
}

TEST(ProcessTableFlagsTest, TreeViewDoesNotOfferSorting)
{
    // Tree view renders in natural parent/child order and ignores the sort specs, so a sortable
    // header would accept the click and change nothing (#926).
    EXPECT_EQ(forProcessTable(/*treeViewEnabled=*/true) & ImGuiTableFlags_Sortable, 0);
}

TEST(ProcessTableFlagsTest, SortingIsTheOnlyDifferenceBetweenTheModes)
{
    // Guards against a future edit dropping resizing, reordering, scrolling or hideable columns
    // from one mode while adjusting the other.
    EXPECT_EQ(forProcessTable(true), forProcessTable(false) & ~ImGuiTableFlags_Sortable);
}

TEST(ProcessTableFlagsTest, BothModesKeepTheSharedTableBehaviours)
{
    for (const bool treeView : {false, true})
    {
        const ImGuiTableFlags flags = forProcessTable(treeView);
        EXPECT_EQ(flags & BASE, BASE) << "tree view: " << treeView;
        // ScrollX/ScrollY and SizingFixedFit are load-bearing for the column-width behaviour and
        // the frozen header row; losing them silently would be hard to spot by eye.
        EXPECT_NE(flags & ImGuiTableFlags_ScrollX, 0);
        EXPECT_NE(flags & ImGuiTableFlags_ScrollY, 0);
        EXPECT_NE(flags & ImGuiTableFlags_SizingFixedFit, 0);
    }
}

} // namespace
} // namespace App
