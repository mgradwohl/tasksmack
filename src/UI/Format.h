#pragma once

#include "UI/Numeric.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <format>
#include <limits>
#include <string>
#include <utility>

namespace UI::Format
{

[[nodiscard]] inline auto toIntSaturated(long value) -> int
{
    if (!std::in_range<int>(value))
    {
        return std::numeric_limits<int>::max();
    }
    return static_cast<int>(value); // Safe: checked by std::in_range
}

[[nodiscard]] inline auto percentToInt(double percent) -> int
{
    const double clamped = std::max(percent, 0.0);
    return toIntSaturated(std::lround(clamped));
}

[[nodiscard]] inline auto percentToInt(float percent) -> int
{
    const float clamped = std::max(percent, 0.0F);
    return toIntSaturated(std::lroundf(clamped));
}

[[nodiscard]] inline auto percentCompact(double percent) -> std::string
{
    return std::format("{}%", percentToInt(percent));
}

[[nodiscard]] inline auto percentCompact(float percent) -> std::string
{
    return std::format("{}%", percentToInt(percent));
}

struct ByteUnit
{
    const char* suffix = "B";
    double scale = 1.0;
    int decimals = 0;
};

[[nodiscard]] inline auto chooseByteUnit(double bytes) -> ByteUnit
{
    const double absBytes = std::abs(bytes);
    if (absBytes >= 1024.0 * 1024.0 * 1024.0)
    {
        return {.suffix = "GB", .scale = 1024.0 * 1024.0 * 1024.0, .decimals = 2};
    }
    if (absBytes >= 1024.0 * 1024.0)
    {
        return {.suffix = "MB", .scale = 1024.0 * 1024.0, .decimals = 1};
    }
    if (absBytes >= 1024.0)
    {
        return {.suffix = "KB", .scale = 1024.0, .decimals = 1};
    }
    return {.suffix = "B", .scale = 1.0, .decimals = 0};
}

[[nodiscard]] inline auto unitForTotalBytes(std::uint64_t bytes) -> ByteUnit
{
    return chooseByteUnit(static_cast<double>(bytes));
}

[[nodiscard]] inline auto unitForBytesPerSecond(double bytesPerSec) -> ByteUnit
{
    return chooseByteUnit(bytesPerSec);
}

[[nodiscard]] inline auto formatBytesWithUnit(double bytes, ByteUnit unit) -> std::string
{
    const double value = bytes / unit.scale;
    return std::format("{:.{}f} {}", value, unit.decimals, unit.suffix);
}

[[nodiscard]] inline auto formatBytes(double bytes) -> std::string
{
    const auto unit = chooseByteUnit(bytes);
    return formatBytesWithUnit(bytes, unit);
}

[[nodiscard]] inline auto formatBytesPerSecWithUnit(double bytesPerSec, ByteUnit unit) -> std::string
{
    return formatBytesWithUnit(bytesPerSec, unit) + "/s";
}

[[nodiscard]] inline auto formatBytesPerSec(double bytesPerSec) -> std::string
{
    const auto unit = chooseByteUnit(bytesPerSec);
    return formatBytesPerSecWithUnit(bytesPerSec, unit);
}

[[nodiscard]] inline auto bytesUsedTotalPercentCompact(std::uint64_t usedBytes, std::uint64_t totalBytes, double percent) -> std::string
{
    const auto unit = unitForTotalBytes(std::max(usedBytes, totalBytes));
    const std::string usedStr = formatBytesWithUnit(static_cast<double>(usedBytes), unit);
    const std::string totalStr = formatBytesWithUnit(static_cast<double>(totalBytes), unit);
    return std::format("{} / {} ({})", usedStr, totalStr, percentCompact(percent));
}

[[nodiscard]] inline auto formatCpuTimeCompact(double totalSeconds) -> std::string
{
    const auto totalMs = static_cast<long long>(std::llround(totalSeconds * 1000.0));
    const auto hours = totalMs / (1000LL * 60LL * 60LL);
    const auto minutes = (totalMs / (1000LL * 60LL)) % 60LL;
    const auto secs = (totalMs / 1000LL) % 60LL;
    const auto centis = (totalMs / 10LL) % 100LL;

    if (hours > 0)
    {
        return std::format("{}:{:02}:{:02}.{:02}", hours, minutes, secs, centis);
    }

    return std::format("{}:{:02}.{:02}", minutes, secs, centis);
}

[[nodiscard]] inline auto formatCpuAffinityMask(std::uint64_t mask) -> std::string
{
    if (mask == 0)
    {
        return "-";
    }

    std::string result;
    int rangeStart = -1;
    int rangeEnd = -1;
    bool hasAny = false;

    for (int cpu = 0; cpu < 64; ++cpu)
    {
        const bool isSet = (mask & (1ULL << cpu)) != 0;

        if (isSet)
        {
            if (rangeStart == -1)
            {
                rangeStart = cpu;
                rangeEnd = cpu;
            }
            else
            {
                rangeEnd = cpu;
            }
        }
        else if (rangeStart != -1)
        {
            if (hasAny)
            {
                result += ',';
            }
            hasAny = true;

            if (rangeStart == rangeEnd)
            {
                result += std::format("{}", rangeStart);
            }
            else if (rangeStart + 1 == rangeEnd)
            {
                result += std::format("{},{}", rangeStart, rangeEnd);
            }
            else
            {
                result += std::format("{}-{}", rangeStart, rangeEnd);
            }

            rangeStart = -1;
            rangeEnd = -1;
        }
    }

    if (rangeStart != -1)
    {
        if (hasAny)
        {
            result += ',';
        }

        if (rangeStart == rangeEnd)
        {
            result += std::format("{}", rangeStart);
        }
        else if (rangeStart + 1 == rangeEnd)
        {
            result += std::format("{},{}", rangeStart, rangeEnd);
        }
        else
        {
            result += std::format("{}-{}", rangeStart, rangeEnd);
        }
    }

    return result;
}

/// Format power value with appropriate unit (W/mW/µW) based on magnitude
[[nodiscard]] inline auto formatPowerCompact(double watts) -> std::string
{
    if (watts <= 0.0)
    {
        return "-";
    }

    const double absWatts = std::abs(watts);
    if (absWatts >= 1.0)
    {
        return std::format("{:.2f} W", watts);
    }
    if (absWatts >= 0.001)
    {
        return std::format("{:.2f} mW", watts * 1000.0);
    }

    return std::format("{:.2f} µW", watts * 1'000'000.0);
}

} // namespace UI::Format
