/// @file test_NumaNodesText.cpp
/// @brief The CPU Cores tab's NUMA nodes card's text (#3088): shown only with more than one node, its
/// columns with and without each node's total (Linux and Windows), its cells for CPU-less, memoryless
/// and not-yet-sampled nodes, and the CPU Details block's NUMA nodes row.

#include "App/Panels/CpuDetailsText.h"
#include "App/Panels/NumaNodesText.h"
#include "Domain/SystemSnapshot.h"
#include "UI/Format.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace App
{
namespace
{

[[nodiscard]] Domain::NumaNodeSnapshot node(int id, std::size_t firstCpu, std::size_t lastCpu)
{
    Domain::NumaNodeSnapshot snap;
    snap.id = id;
    snap.cpus.setRange(firstCpu, lastCpu);
    snap.cpuCount = snap.cpus.count();
    snap.cpuPercent = 25.0;
    snap.memTotalBytes = 8ULL << 30U;
    snap.memFreeBytes = 2ULL << 30U;
    snap.memUsedBytes = 6ULL << 30U;
    return snap;
}

[[nodiscard]] std::vector<std::string_view> columnNames(bool totalsKnown)
{
    std::vector<std::string_view> names;
    for (const NumaNodesText::Column& column : NumaNodesText::columns(totalsKnown))
    {
        names.emplace_back(column.name);
    }
    return names;
}

TEST(NumaNodesTextTest, ShownOnlyWithMoreThanOneNode)
{
    Domain::SystemSnapshot snap;
    EXPECT_FALSE(NumaNodesText::showNumaNodes(snap));
    snap.numaNodes.push_back(node(0, 0, 7));
    EXPECT_FALSE(NumaNodesText::showNumaNodes(snap)) << "one node holds everything: nothing to add";
    snap.numaNodes.push_back(node(1, 8, 15));
    EXPECT_TRUE(NumaNodesText::showNumaNodes(snap));
    EXPECT_EQ(NumaNodesText::heading(snap), "NUMA nodes: 2");
}

TEST(NumaNodesTextTest, ColumnsFollowWhetherTotalsAreKnown)
{
    EXPECT_EQ(columnNames(true), (std::vector<std::string_view>{"Node", "CPUs", "CPU", "Used", "Free", "Total"}));
    EXPECT_EQ(columnNames(false), (std::vector<std::string_view>{"Node", "CPUs", "CPU", "Available"}));
    for (const bool totals : {true, false})
    {
        for (const NumaNodesText::Column& column : NumaNodesText::columns(totals))
        {
            EXPECT_FALSE(column.tooltip.empty()) << column.name;
        }
    }
}

TEST(NumaNodesTextTest, RowWithTotals)
{
    const auto cells = NumaNodesText::row(node(1, 8, 15), true);
    ASSERT_EQ(cells.size(), NumaNodesText::columns(true).size());
    EXPECT_EQ(cells[0].text, "1");
    EXPECT_EQ(cells[1].text, "8-15");
    EXPECT_EQ(cells[1].tooltip, "8 logical processors");
    EXPECT_EQ(cells[2].text, UI::Format::formatPercent(25.0));
    EXPECT_EQ(cells[3].text, UI::Format::formatBytes(static_cast<double>(6ULL << 30U)) + " (" + UI::Format::formatPercent(75.0) + ")");
    EXPECT_EQ(cells[4].text, UI::Format::formatBytes(static_cast<double>(2ULL << 30U)));
    EXPECT_EQ(cells[5].text, UI::Format::formatBytes(static_cast<double>(8ULL << 30U)));
    EXPECT_TRUE(std::ranges::all_of(cells, &NumaNodesText::Cell::available));
}

TEST(NumaNodesTextTest, RowWithoutTotalsShowsAvailableMemory)
{
    Domain::NumaNodeSnapshot windows = node(0, 0, 3);
    windows.memTotalBytes = 0;
    windows.memUsedBytes = 0;
    windows.memFreeBytes = 5ULL << 30U;
    const auto cells = NumaNodesText::row(windows, false);
    ASSERT_EQ(cells.size(), NumaNodesText::columns(false).size());
    EXPECT_EQ(cells[3].text, UI::Format::formatBytes(static_cast<double>(5ULL << 30U)));
    EXPECT_TRUE(cells[3].available);
}

TEST(NumaNodesTextTest, CpuLessMemorylessAndUnsampledNodesReadAsUnavailable)
{
    Domain::NumaNodeSnapshot cpuLess = node(2, 0, 0);
    cpuLess.cpus = {};
    cpuLess.cpuCount = 0;
    cpuLess.cpuPercent = std::numeric_limits<double>::quiet_NaN();
    auto cells = NumaNodesText::row(cpuLess, true);
    EXPECT_EQ(cells[1].text, "None");
    EXPECT_FALSE(cells[1].available);
    EXPECT_EQ(cells[2].text, NumaNodesText::UNAVAILABLE_TEXT);
    EXPECT_FALSE(cells[2].tooltip.empty());
    EXPECT_TRUE(cells[3].available) << "its memory is still read";

    Domain::NumaNodeSnapshot memoryless = node(3, 4, 7);
    memoryless.memTotalBytes = 0;
    memoryless.memFreeBytes = 0;
    memoryless.memUsedBytes = 0;
    cells = NumaNodesText::row(memoryless, true);
    for (std::size_t column = 3; column < cells.size(); ++column)
    {
        EXPECT_EQ(cells[column].text, NumaNodesText::UNAVAILABLE_TEXT) << column;
        EXPECT_FALSE(cells[column].available) << column;
    }

    Domain::NumaNodeSnapshot unsampled = node(0, 0, 3);
    unsampled.cpuPercent = std::numeric_limits<double>::quiet_NaN();
    cells = NumaNodesText::row(unsampled, true);
    EXPECT_EQ(cells[2].text, NumaNodesText::UNAVAILABLE_TEXT);
    EXPECT_FALSE(cells[2].available);
}

TEST(NumaNodesTextTest, CpuDetailsHasANumaRowOnlyWithMoreThanOneNode)
{
    Domain::SystemSnapshot snap;
    CpuDetailsText::Inputs inputs;
    inputs.snapshot = &snap;
    const auto hasNumaRow = [&inputs]
    {
        const auto rows = CpuDetailsText::buildRows(inputs);
        return std::ranges::find(rows, std::string_view{"NUMA nodes"}, &CpuDetailsText::Row::label) != rows.end();
    };
    EXPECT_FALSE(hasNumaRow());
    snap.numaNodes.push_back(node(0, 0, 7));
    EXPECT_FALSE(hasNumaRow());
    snap.numaNodes.push_back(node(1, 8, 15));
    EXPECT_TRUE(hasNumaRow());
    const auto rows = CpuDetailsText::buildRows(inputs);
    const auto numa = std::ranges::find(rows, std::string_view{"NUMA nodes"}, &CpuDetailsText::Row::label);
    ASSERT_NE(numa, rows.end());
    EXPECT_EQ(numa->value, "2");
}

} // namespace
} // namespace App
