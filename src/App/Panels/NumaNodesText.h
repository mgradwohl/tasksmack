#pragma once

// The CPU Cores tab's NUMA nodes card (#3088): whether it shows, its heading, and its table's columns
// and cells. Pure (no ImGui calls), so it is unit-tested; NumaNodesView draws it.

#include "Domain/Numeric.h"
#include "Domain/SystemSnapshot.h"
#include "UI/Format.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace App::NumaNodesText
{

/// A value that is not known: an em dash, drawn muted with the reason on hover (as CPU Details does).
inline constexpr std::string_view UNAVAILABLE_TEXT = "\xE2\x80\x94";

/// One table cell.
struct Cell
{
    std::string text;
    bool available = true; ///< False: drawn muted, with `tooltip` saying why
    std::string tooltip;   ///< Hover text; empty for none

    bool operator==(const Cell&) const = default;
};

/// One column: its header (a string literal, as ImGui takes it) and the header's hover text.
struct Column
{
    const char* name = "";
    std::string_view tooltip;
    float weight = 1.0F; ///< Its share of the table's width
};

/// The card shows only on a machine with more than one node: with one, every processor and all the
/// memory are in it, and the table would repeat the rest of the tab (#3088).
[[nodiscard]] inline bool showNumaNodes(const Domain::SystemSnapshot& snap) noexcept
{
    return snap.numaNodes.size() > 1;
}

/// "NUMA nodes: 2"
[[nodiscard]] inline std::string heading(const Domain::SystemSnapshot& snap)
{
    return std::format("NUMA nodes: {}", UI::Format::formatIntLocalized(snap.numaNodes.size()));
}

/// The table's columns. With each node's total known (Linux): Used, Free and Total. Without (Windows,
/// which reports only what is available on each node): Available alone.
[[nodiscard]] inline std::vector<Column> columns(bool totalsKnown)
{
    std::vector<Column> result{
        {.name = "Node", .tooltip = "The operating system's number for the node", .weight = 0.4F}, // A number: narrow
        {.name = "CPUs", .tooltip = "The node's logical processors, numbered as on this tab"},
        {.name = "CPU", .tooltip = "The node's processors' average utilization"},
    };
    if (totalsKnown)
    {
        result.push_back(
            {.name = "Used", .tooltip = "The node's memory in use: its total less its free memory, with page cache counted as used"});
        result.push_back({.name = "Free", .tooltip = "The node's free memory (MemFree): page cache is not counted as free"});
        result.push_back({.name = "Total", .tooltip = "The node's memory"});
    }
    else
    {
        result.push_back({
            .name = "Available",
            .tooltip = "The node's available memory: free, zeroed and standby (cache) pages. "
                       "Windows doesn't report each node's total, so there is no used figure",
        });
    }
    return result;
}

/// One node's row, a cell per column() in order.
[[nodiscard]] inline std::vector<Cell> row(const Domain::NumaNodeSnapshot& node, bool totalsKnown)
{
    const auto bytes = [](std::uint64_t value)
    {
        return Cell{.text = UI::Format::formatBytes(Domain::Numeric::toDouble(value)), .available = true, .tooltip = {}};
    };
    const auto unavailable = [](std::string tooltip)
    {
        return Cell{.text = std::string(UNAVAILABLE_TEXT), .available = false, .tooltip = std::move(tooltip)};
    };

    std::vector<Cell> cells;
    cells.reserve(6);
    cells.push_back({.text = std::format("{}", node.id), .available = true, .tooltip = {}});
    if (node.cpus.empty())
    {
        constexpr const char* CPU_LESS = "This node has memory but no processors";
        cells.push_back({.text = "None", .available = false, .tooltip = CPU_LESS});
        cells.push_back(unavailable(CPU_LESS));
    }
    else
    {
        cells.push_back({
            .text = UI::Format::formatCpuAffinity(node.cpus.words()),
            .available = true,
            .tooltip = std::format("{} logical processors", UI::Format::formatIntLocalized(node.cpuCount)),
        });
        if (std::isnan(node.cpuPercent))
        {
            cells.push_back(unavailable("No reading yet: CPU use needs two samples"));
        }
        else
        {
            cells.push_back({.text = UI::Format::formatPercent(node.cpuPercent), .available = true, .tooltip = {}});
        }
    }

    if (totalsKnown)
    {
        if (node.memTotalBytes == 0)
        {
            constexpr const char* MEMORYLESS = "This node has processors but no memory of its own";
            cells.push_back(unavailable(MEMORYLESS));
            cells.push_back(unavailable(MEMORYLESS));
            cells.push_back(unavailable(MEMORYLESS));
        }
        else
        {
            Cell used = bytes(node.memUsedBytes);
            used.text += std::format(" ({})",
                                     UI::Format::formatPercent(100.0 * Domain::Numeric::toDouble(node.memUsedBytes) /
                                                               Domain::Numeric::toDouble(node.memTotalBytes)));
            cells.push_back(std::move(used));
            cells.push_back(bytes(node.memFreeBytes));
            cells.push_back(bytes(node.memTotalBytes));
        }
    }
    else
    {
        // A memoryless node reads 0 available, as a node with nothing left would: a reading either way.
        cells.push_back(bytes(node.memFreeBytes));
    }
    return cells;
}

} // namespace App::NumaNodesText
