#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <string>

namespace UI::Format
{

[[nodiscard]] inline auto percentToInt(double percent) -> int
{
    const double clamped = std::max(percent, 0.0);
    return static_cast<int>(std::lround(clamped));
}

[[nodiscard]] inline auto percentCompact(double percent) -> std::string
{
    return std::format("{}%", percentToInt(percent));
}

struct ByteUnit
{
    const char* suffix = "B";
    double scale = 1.0;
    int decimals = 0;
};

[[nodiscard]] inline auto unitForTotalBytes(uint64_t totalBytes) -> ByteUnit
{
    constexpr double KIB = 1024.0;
    constexpr double MIB = 1024.0 * 1024.0;
    constexpr double GIB = 1024.0 * 1024.0 * 1024.0;

    const double total = static_cast<double>(totalBytes);
    if (total >= GIB)
    {
        return {.suffix = "GB", .scale = GIB, .decimals = 1};
    }
    if (total >= MIB)
    {
        return {.suffix = "MB", .scale = MIB, .decimals = 0};
    }
    if (total >= KIB)
    {
        return {.suffix = "KB", .scale = KIB, .decimals = 0};
    }
    return {.suffix = "B", .scale = 1.0, .decimals = 0};
}

[[nodiscard]] inline auto bytesUsedTotalCompact(uint64_t usedBytes, uint64_t totalBytes) -> std::string
{
    if (totalBytes == 0)
    {
        return {};
    }

    const ByteUnit unit = unitForTotalBytes(totalBytes);
    const double used = static_cast<double>(usedBytes) / unit.scale;
    const double total = static_cast<double>(totalBytes) / unit.scale;

    if (unit.decimals == 1)
    {
        return std::format("{:.1f}/{:.1f} {}", used, total, unit.suffix);
    }

    return std::format("{:.0f}/{:.0f} {}", used, total, unit.suffix);
}

[[nodiscard]] inline auto bytesUsedTotalPercentCompact(uint64_t usedBytes, uint64_t totalBytes, double percent) -> std::string
{
    const std::string usedTotal = bytesUsedTotalCompact(usedBytes, totalBytes);
    if (usedTotal.empty())
    {
        return {};
    }

    return std::format("{} {}", usedTotal, percentCompact(percent));
}

} // namespace UI::Format
