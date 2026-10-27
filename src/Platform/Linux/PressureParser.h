#pragma once

// Linux pressure stall information (#3062): /proc/pressure/{cpu,memory,io}, one line per kind,
//   some avg10=0.12 avg60=0.05 avg300=0.01 total=123456
//   full avg10=0.00 avg60=0.00 avg300=0.00 total=0
// Pure, so it is unit-tested on text (test_PressureParser.cpp).

#include "Platform/SystemTypes.h"

#include <charconv>
#include <optional>
#include <string_view>
#include <system_error>

namespace Platform::PressureParser
{

/// The value of `key=` in @p line, or nullopt when the key is missing or its value isn't a number.
[[nodiscard]] inline std::optional<double> field(std::string_view line, std::string_view key) noexcept
{
    std::size_t at = 0;
    while ((at = line.find(key, at)) != std::string_view::npos)
    {
        const bool startsWord = at == 0 || line[at - 1] == ' ';
        const std::size_t valueAt = at + key.size();
        if (startsWord && valueAt < line.size() && line[valueAt] == '=')
        {
            const char* first = line.data() + valueAt + 1;
            const char* last = line.data() + line.size();
            double value = 0.0;
            const auto [end, error] = std::from_chars(first, last, value);
            if (error != std::errc{} || end == first)
            {
                return std::nullopt;
            }
            return value;
        }
        at = valueAt;
    }
    return std::nullopt;
}

/// One `some` or `full` line's averages; nullopt when any of the three is missing.
[[nodiscard]] inline std::optional<PressureStall> stall(std::string_view line) noexcept
{
    const auto avg10 = field(line, "avg10");
    const auto avg60 = field(line, "avg60");
    const auto avg300 = field(line, "avg300");
    if (!avg10.has_value() || !avg60.has_value() || !avg300.has_value())
    {
        return std::nullopt;
    }
    return PressureStall{.avg10 = *avg10, .avg60 = *avg60, .avg300 = *avg300};
}

/// A whole /proc/pressure/<resource> file. Lines other than `some` and `full`, and malformed ones, are
/// ignored, so a resource the file doesn't describe comes back with neither.
[[nodiscard]] inline ResourcePressure parse(std::string_view text)
{
    ResourcePressure pressure;
    while (!text.empty())
    {
        const std::size_t lineEnd = text.find('\n');
        const std::string_view line = text.substr(0, lineEnd);
        text = lineEnd == std::string_view::npos ? std::string_view{} : text.substr(lineEnd + 1);
        if (line.starts_with("some "))
        {
            pressure.some = stall(line.substr(5));
        }
        else if (line.starts_with("full "))
        {
            pressure.full = stall(line.substr(5));
        }
    }
    return pressure;
}

} // namespace Platform::PressureParser
