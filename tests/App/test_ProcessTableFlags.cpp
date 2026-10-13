#include "App/Panels/ProcessTableFlags.h"

#include <gtest/gtest.h>
#include <imgui.h>

namespace App
{
namespace
{

using ProcessTableFlags::BASE;
using ProcessTableFlags::forProcessTable;

TEST(ProcessTableFlagsTest, BothViewsOfferSorting)
{
    // Tree view sorts each parent's children (#838), so its headers take a sort as the list's do.
    EXPECT_NE(forProcessTable() & ImGuiTableFlags_Sortable, 0);
}

TEST(ProcessTableFlagsTest, TheTableKeepsItsSharedBehaviours)
{
    const ImGuiTableFlags flags = forProcessTable();
    EXPECT_EQ(flags & BASE, BASE);
    // ScrollX/ScrollY and SizingFixedFit are load-bearing for the column-width behaviour and the
    // frozen header row; losing them silently would be hard to spot by eye.
    EXPECT_NE(flags & ImGuiTableFlags_ScrollX, 0);
    EXPECT_NE(flags & ImGuiTableFlags_ScrollY, 0);
    EXPECT_NE(flags & ImGuiTableFlags_SizingFixedFit, 0);
}

} // namespace
} // namespace App
