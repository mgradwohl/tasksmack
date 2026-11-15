#pragma once

// The CPU Cores tab's NUMA nodes card (#3088): whether it shows, its heading, and its table's columns
// and cells. Pure (no ImGui calls), so it is unit-tested; NumaNodesView draws it.

#include "Domain/Numeric.h"
#include "Domain/SystemSnapshot.h"
#include "UI/Format.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <span>
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

/// The table's columns with each node's total known (Linux): Used, Free and Total.
inline constexpr std::array<Column, 6> COLUMNS_WITH_TOTALS{
    Column{.name = "Node", .tooltip = "The operating system's number for the node", .weight = 0.4F}, // A number: narrow
    Column{.name = "CPUs", .tooltip = "The node's logical processors, numbered as on this tab"},
    Column{.name = "CPU", .tooltip = "The node's processors' average utilization"},
    Column{.name = "Used", .tooltip = "The node's memory in use: its total less its free memory, with page cache counted as used"},
    Column{.name = "Free", .tooltip = "The node's free memory (MemFree): page cache is not counted as free"},
    Column{.name = "Total", .tooltip = "The node's memory"},
};

/// Without them (Windows, which reports only what is available on each node): Available alone.
inline constexpr std::array<Column, 4> COLUMNS_AVAILABLE_ONLY{
    Column{.name = "Node", .tooltip = "The operating system's number for the node", .weight = 0.4F},
    Column{.name = "CPUs", .tooltip = "The node's logical processors, numbered as on this tab"},
    Column{.name = "CPU", .tooltip = "The node's processors' average utilization"},
    Column{
        .name = "Available",
        .tooltip = "The node's available memory: free, zeroed and standby (cache) pages. "
                   "Windows doesn't report each node's total, so there is no used figure",
    },
};

/// The table's columns (#3808: fixed arrays, not a vector built each frame).
[[nodiscard]] constexpr std::span<const Column> columns(bool totalsKnown) noexcept
{
    return totalsKnown ? std::span<const Column>(COLUMNS_WITH_TOTALS) : std::span<const Column>(COLUMNS_AVAILABLE_ONLY);
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

/// The card's heading, columns and every node's row, as one table to draw.
struct Table
{
    std::string heading;
    std::span<const Column> columns;
    std::vector<std::vector<Cell>> rows;
};

/// Whether @p a and @p b would make the same row: every field the row reads, a NaN CPU reading equal to another.
[[nodiscard]] inline bool sameRowInputs(const Domain::NumaNodeSnapshot& a, const Domain::NumaNodeSnapshot& b) noexcept
{
    const bool sameCpu = (std::isnan(a.cpuPercent) && std::isnan(b.cpuPercent)) || a.cpuPercent == b.cpuPercent;
    return a.id == b.id && a.cpus == b.cpus && a.cpuCount == b.cpuCount && sameCpu && a.memTotalBytes == b.memTotalBytes &&
           a.memFreeBytes == b.memFreeBytes && a.memUsedBytes == b.memUsedBytes;
}

/// The table, built when what it shows changes rather than every frame (#3808): keyed on the nodes (each field a
/// row reads) and whether their totals are known, the whole input of heading(), columns() and row(). A new
/// sample's CPU readings rebuild it, about once a second; a frame between samples draws the same strings. UI thread
/// only, as the card is.
class TableCache
{
  public:
    [[nodiscard]] const Table& get(const Domain::SystemSnapshot& snap)
    {
        const bool same = m_Built && m_TotalsKnown == snap.numaNodeTotalsKnown && m_Nodes.size() == snap.numaNodes.size() &&
                          std::equal(m_Nodes.begin(), m_Nodes.end(), snap.numaNodes.begin(), sameRowInputs);
        if (!same)
        {
            m_Nodes = snap.numaNodes;
            m_TotalsKnown = snap.numaNodeTotalsKnown;
            m_Table.heading = heading(snap);
            m_Table.columns = columns(m_TotalsKnown);
            m_Table.rows.clear();
            m_Table.rows.reserve(m_Nodes.size());
            for (const Domain::NumaNodeSnapshot& node : m_Nodes)
            {
                m_Table.rows.push_back(row(node, m_TotalsKnown));
            }
            m_Built = true;
            ++m_Builds;
        }
        return m_Table;
    }

    /// How many times get() has built the table (tests).
    [[nodiscard]] std::size_t builds() const noexcept
    {
        return m_Builds;
    }

  private:
    std::vector<Domain::NumaNodeSnapshot> m_Nodes;
    Table m_Table;
    std::size_t m_Builds = 0;
    bool m_TotalsKnown = false;
    bool m_Built = false;
};

} // namespace App::NumaNodesText
