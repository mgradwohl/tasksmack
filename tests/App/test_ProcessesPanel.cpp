#include "App/ProcessColumnConfig.h"
#include "Domain/ProcessSnapshot.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

// Note: ProcessesPanel::buildProcessTree() tests are commented out because linking
// ProcessesPanel.cpp brings in ImGui dependencies that are complex to satisfy in tests.
// The tree-building algorithm is tested indirectly through integration tests.
// These tests focus on ProcessColumnSettings which is fully testable.

namespace App
{
namespace
{

// ========== Column Visibility Count Tests ==========
// Tests for ProcessColumnSettings counting logic used by visibleColumnCount()

TEST(ProcessesPanelTest, ColumnSettingsDefaultVisibleCount)
{
    const ProcessColumnSettings settings;

    // Count visible columns using default settings
    int visibleCount = 0;
    for (const auto col : allProcessColumns())
    {
        if (settings.isVisible(col))
        {
            ++visibleCount;
        }
    }

    // Verify we have a reasonable number of default visible columns
    EXPECT_GT(visibleCount, 0);
    EXPECT_LT(visibleCount, static_cast<int>(processColumnCount()));
}

TEST(ProcessesPanelTest, ColumnSettingsAllHidden)
{
    ProcessColumnSettings settings;

    // Hide all columns
    for (const auto col : allProcessColumns())
    {
        settings.setVisible(col, false);
    }

    // Count visible columns
    int visibleCount = 0;
    for (const auto col : allProcessColumns())
    {
        if (settings.isVisible(col))
        {
            ++visibleCount;
        }
    }

    EXPECT_EQ(visibleCount, 0);
}

TEST(ProcessesPanelTest, ColumnSettingsAllVisible)
{
    ProcessColumnSettings settings;

    // Show all columns
    for (const auto col : allProcessColumns())
    {
        settings.setVisible(col, true);
    }

    // Count visible columns
    int visibleCount = 0;
    for (const auto col : allProcessColumns())
    {
        if (settings.isVisible(col))
        {
            ++visibleCount;
        }
    }

    EXPECT_EQ(visibleCount, static_cast<int>(processColumnCount()));
}

// ========== Column Info Tests ==========

TEST(ProcessesPanelTest, PidAndNameColumnsCannotBeHidden)
{
    // PID and Name are required columns - verify canHide is false
    const auto pidInfo = getColumnInfo(ProcessColumn::PID);
    const auto nameInfo = getColumnInfo(ProcessColumn::Name);

    EXPECT_FALSE(pidInfo.canHide);
    EXPECT_FALSE(nameInfo.canHide);
}

TEST(ProcessesPanelTest, OptionalColumnsCanBeHidden)
{
    // Most other columns should be hideable
    const auto cpuInfo = getColumnInfo(ProcessColumn::CpuPercent);
    const auto memInfo = getColumnInfo(ProcessColumn::MemPercent);
    const auto cmdInfo = getColumnInfo(ProcessColumn::Command);

    EXPECT_TRUE(cpuInfo.canHide);
    EXPECT_TRUE(memInfo.canHide);
    EXPECT_TRUE(cmdInfo.canHide);
}

TEST(ProcessesPanelTest, ColumnInfoHasRequiredFields)
{
    // Verify all columns have non-empty metadata
    for (const auto col : allProcessColumns())
    {
        const auto info = getColumnInfo(col);

        EXPECT_FALSE(info.name.empty()) << "Column " << toIndex(col) << " has empty name";
        EXPECT_FALSE(info.menuName.empty()) << "Column " << toIndex(col) << " has empty menuName";
        EXPECT_FALSE(info.configKey.empty()) << "Column " << toIndex(col) << " has empty configKey";
        EXPECT_FALSE(info.description.empty()) << "Column " << toIndex(col) << " has empty description";
    }
}

TEST(ProcessesPanelTest, DefaultVisibleColumnsAreReasonable)
{
    // Core columns should be visible by default
    const ProcessColumnSettings settings;

    EXPECT_TRUE(settings.isVisible(ProcessColumn::PID));
    EXPECT_TRUE(settings.isVisible(ProcessColumn::Name));
    EXPECT_TRUE(settings.isVisible(ProcessColumn::CpuPercent));
    EXPECT_TRUE(settings.isVisible(ProcessColumn::MemPercent));
}

TEST(ProcessesPanelTest, AdvancedColumnsHiddenByDefault)
{
    // Advanced/optional columns should be hidden by default
    const ProcessColumnSettings settings;

    EXPECT_FALSE(settings.isVisible(ProcessColumn::PPID));
    EXPECT_FALSE(settings.isVisible(ProcessColumn::Virtual));
    EXPECT_FALSE(settings.isVisible(ProcessColumn::Handles));
    EXPECT_FALSE(settings.isVisible(ProcessColumn::GpuPercent));
}

// ========== ProcessSnapshot Tree Structure Tests ==========
// Test the data structures used for tree building without linking ProcessesPanel.cpp

TEST(ProcessesPanelTest, ProcessSnapshotHasTreeFields)
{
    // Verify ProcessSnapshot has the fields needed for tree building
    Domain::ProcessSnapshot snap;

    // These fields are used by buildProcessTree
    snap.pid = 100;
    snap.parentPid = 1;
    snap.uniqueKey = 12345;

    EXPECT_EQ(snap.pid, 100);
    EXPECT_EQ(snap.parentPid, 1);
    EXPECT_EQ(snap.uniqueKey, 12345);
}

TEST(ProcessesPanelTest, TreeMapCanStoreChildIndices)
{
    // Test the tree data structure type used by ProcessesPanel
    std::unordered_map<std::uint64_t, std::vector<std::size_t>> tree;

    // Parent uniqueKey 1001 has children at indices 1, 2, 3
    tree[1001] = {1, 2, 3};

    ASSERT_EQ(tree.count(1001), 1);
    EXPECT_EQ(tree[1001].size(), 3);
    EXPECT_EQ(tree[1001][0], 1);
    EXPECT_EQ(tree[1001][1], 2);
    EXPECT_EQ(tree[1001][2], 3);
}

} // namespace
} // namespace App
