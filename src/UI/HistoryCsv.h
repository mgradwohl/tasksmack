#pragma once

// A history chart's series as CSV (#808), for the chart's "Copy data as CSV" menu item: one row per
// sample time, oldest first, with the wall-clock time and the seconds before now, then one column
// per series. Pure -- no ImGui -- so it is unit-testable; ChartWidgets.h gathers the series.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <format>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace UI::HistoryCsv
{

/// One series as the chart was handed it: `x` in seconds relative to now (negative: before it),
/// `y` its values. A NaN value is no reading (a gap) and is written as an empty cell.
struct Series
{
    std::string label;
    std::vector<double> x;
    std::vector<double> y;
};

/// The header of a series with no name of its own (a chart whose one series' label is "##..." only).
inline constexpr std::string_view UNNAMED_SERIES = "Value";

/// `field` as a CSV field (RFC 4180): quoted, with quotes doubled, when it holds a comma, quote or
/// line break.
[[nodiscard]] inline std::string escapeField(std::string_view field)
{
    if (field.find_first_of(",\"\r\n") == std::string_view::npos)
    {
        return std::string(field);
    }
    std::string quoted;
    quoted.reserve(field.size() + 2);
    quoted += '"';
    for (const char c : field)
    {
        if (c == '"')
        {
            quoted += '"';
        }
        quoted += c;
    }
    quoted += '"';
    return quoted;
}

/// `when` as ISO 8601 UTC to the millisecond: "2026-10-12T03:04:05.123Z".
[[nodiscard]] inline std::string formatUtc(std::chrono::system_clock::time_point when)
{
    return std::format("{:%FT%T}Z", std::chrono::floor<std::chrono::milliseconds>(when));
}

/// The CSV text of `series`, with `now` the wall-clock time their x = 0 stands for. Header:
/// "Time (UTC),Seconds ago,<label>,...". Rows: every distinct x of any series, ascending (oldest
/// first); a series with no sample at a row's x, or a NaN one, leaves its cell empty. Lines end in
/// "\r\n", as RFC 4180 has them and spreadsheets expect.
[[nodiscard]] inline std::string build(std::span<const Series> series, std::chrono::system_clock::time_point now)
{
    // Each x's cells, one per series (empty where the series has no reading there).
    std::map<double, std::vector<std::string>> rows;
    for (std::size_t column = 0; column < series.size(); ++column)
    {
        const Series& one = series[column];
        const std::size_t count = std::min(one.x.size(), one.y.size());
        for (std::size_t i = 0; i < count; ++i)
        {
            if (!std::isfinite(one.x[i]))
            {
                continue;
            }
            std::vector<std::string>& cells = rows[one.x[i]];
            cells.resize(series.size());
            if (std::isfinite(one.y[i]))
            {
                cells[column] = std::format("{}", one.y[i]);
            }
        }
    }

    std::string csv = "Time (UTC),Seconds ago";
    for (const Series& one : series)
    {
        csv += ',';
        csv += escapeField(one.label.empty() ? UNNAMED_SERIES : std::string_view(one.label));
    }
    csv += "\r\n";
    for (const auto& [x, cells] : rows)
    {
        const auto offset = std::chrono::duration_cast<std::chrono::system_clock::duration>(std::chrono::duration<double>(x));
        csv += formatUtc(now + offset);
        csv += std::format(",{:.3f}", -x + 0.0); // + 0.0: x = 0 is "0.000", not "-0.000"
        for (const std::string& cell : cells)
        {
            csv += ',';
            csv += cell;
        }
        csv += "\r\n";
    }
    return csv;
}

} // namespace UI::HistoryCsv
