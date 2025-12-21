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

[[nodiscard]] inline auto unitForTotalBytes(std::uint64_t totalBytes) -> ByteUnit
{
    constexpr std::uint64_t KIB = 1024ULL;
    constexpr std::uint64_t MIB = 1024ULL * 1024ULL;
    constexpr std::uint64_t GIB = 1024ULL * 1024ULL * 1024ULL;

    if (totalBytes >= GIB)
    {
        return {.suffix = "GB", .scale = UI::Numeric::toDouble(GIB), .decimals = 1};
    }
    if (totalBytes >= MIB)
    {
        return {.suffix = "MB", .scale = UI::Numeric::toDouble(MIB), .decimals = 0};
    }
    if (totalBytes >= KIB)
    {
        return {.suffix = "KB", .scale = UI::Numeric::toDouble(KIB), .decimals = 0};
    }
    return {.suffix = "B", .scale = 1.0, .decimals = 0};
}

[[nodiscard]] inline auto bytesUsedTotalCompact(std::uint64_t usedBytes, std::uint64_t totalBytes) -> std::string
{
    if (totalBytes == 0)
    {
        return {};
    }

    const ByteUnit unit = unitForTotalBytes(totalBytes);
    const double used = UI::Numeric::toDouble(usedBytes) / unit.scale;
    const double total = UI::Numeric::toDouble(totalBytes) / unit.scale;

    if (unit.decimals == 1)
    {
        return std::format("{:.1f}/{:.1f} {}", used, total, unit.suffix);
    }

    return std::format("{:.0f}/{:.0f} {}", used, total, unit.suffix);
}

[[nodiscard]] inline auto bytesUsedTotalPercentCompact(std::uint64_t usedBytes, std::uint64_t totalBytes, double percent) -> std::string
{
    const std::string usedTotal = bytesUsedTotalCompact(usedBytes, totalBytes);
    if (usedTotal.empty())
    {
        return {};
    }

    return std::format("{} {}", usedTotal, percentCompact(percent));
}

[[nodiscard]] inline auto bytesUsedTotalPercentCompact(std::uint64_t usedBytes, std::uint64_t totalBytes, float percent) -> std::string
{
    const std::string usedTotal = bytesUsedTotalCompact(usedBytes, totalBytes);
    if (usedTotal.empty())
    {
        return {};
    }

    return std::format("{} {}", usedTotal, percentCompact(percent));
}

[[nodiscard]] inline auto formatBytesWithUnit(std::uint64_t bytes, const ByteUnit& unit) -> std::string
{
    const double value = UI::Numeric::toDouble(bytes) / unit.scale;
    if (unit.decimals == 1)
    {
        return std::format("{:.1f} {}", value, unit.suffix);
    }
    return std::format("{:.0f} {}", value, unit.suffix);
}

[[nodiscard]] inline auto formatCpuTimeCompact(double seconds) -> std::string
{
    seconds = std::max(0.0, seconds);

    // Explicit: UI display, truncation acceptable.
    const auto totalMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::duration<double>(seconds)).count();

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

} // namespace UI::Format
