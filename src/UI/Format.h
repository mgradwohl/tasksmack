#pragma once

#include "UI/Numeric.h"

#include <algorithm>
#include <cmath>
#include <concepts>
#include <cstdint>
#include <format>
#include <functional>
#include <limits>
#include <string>
#include <string_view>
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

template<std::floating_point T> [[nodiscard]] inline auto percentToInt(T percent) -> int
{
    const T clamped = std::max(percent, static_cast<T>(0));
    return toIntSaturated(std::lround(static_cast<double>(clamped)));
}

template<std::floating_point T> [[nodiscard]] inline auto percentCompact(T percent) -> std::string
{
    return std::format("{:L}%", percentToInt(percent));
}

template<std::integral T> [[nodiscard]] inline auto percentCompact(T percent) -> std::string
{
    return std::format("{:L}%", percent);
}

[[nodiscard]] inline auto formatId(std::int64_t value) -> std::string
{
    return std::format("{}", value);
}

template<std::integral T> [[nodiscard]] inline auto formatIntLocalized(T value) -> std::string
{
    return std::format("{:L}", value);
}

[[nodiscard]] inline auto formatUIntLocalized(std::uint64_t value) -> std::string
{
    return formatIntLocalized(value);
}

[[nodiscard]] inline auto formatDoubleLocalized(double value, int decimals) -> std::string
{
    return std::format("{:.{}Lf}", static_cast<long double>(value), decimals);
}

template<std::floating_point T> [[nodiscard]] inline auto percentOneDecimalLocalized(T percent) -> std::string
{
    return std::format("{:.1Lf}%", static_cast<long double>(percent));
}

template<std::integral T> [[nodiscard]] inline auto formatCountWithLabel(T value, std::string_view label) -> std::string
{
    return std::format("{} {}", formatIntLocalized(value), label);
}

template<typename T, typename Formatter>
    requires std::invocable<Formatter, const T&>
[[nodiscard]] inline auto formatOrDash(const T& value, Formatter&& formatter) -> std::string
{
    if (value <= T{0})
    {
        return "-";
    }

    return std::invoke(std::forward<Formatter>(formatter), value);
}

[[nodiscard]] inline auto formatHoursMinutes(std::uint64_t hours, std::uint64_t minutes) -> std::string
{
    return std::format("{}h {}m", hours, minutes);
}

[[nodiscard]] inline auto formatUptimeShort(std::uint64_t seconds) -> std::string
{
    if (seconds == 0)
    {
        return {};
    }

    const std::uint64_t days = seconds / 86400;
    const std::uint64_t hours = (seconds % 86400) / 3600;
    const std::uint64_t minutes = (seconds % 3600) / 60;

    if (days > 0)
    {
        return std::format("Up: {}d {}h {}m", days, hours, minutes);
    }
    if (hours > 0)
    {
        return std::format("Up: {}h {}m", hours, minutes);
    }

    return std::format("Up: {}m", minutes);
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
        return {.suffix = "GB", .scale = 1024.0 * 1024.0 * 1024.0, .decimals = 1};
    }
    if (absBytes >= 1024.0 * 1024.0)
    {
        return {.suffix = "MB", .scale = 1024.0 * 1024.0, .decimals = 1};
    }
    if (absBytes >= 1024.0)
    {
        return {.suffix = "KB", .scale = 1024.0, .decimals = 1};
    }
    return {.suffix = "B", .scale = 1.0, .decimals = 1};
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
    return std::format("{:.{}Lf} {}", static_cast<long double>(value), unit.decimals, unit.suffix);
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

// ============================================================================
// Decimal-aligned numeric parts for table column rendering
// ============================================================================

/// Parts of a numeric value split for decimal-point alignment.
/// The three parts should be rendered as: [wholePart right-aligned][decimalPart][unitPart]
/// This allows decimal points to align vertically regardless of digit count.
struct AlignedNumericParts
{
    std::string wholePart;   ///< Digits + decimal point, e.g. "123,456." (render right-aligned)
    std::string decimalPart; ///< Fractional digits only, e.g. "98" (fixed width, left-aligned)
    std::string unitPart;    ///< Unit suffix like " MB" or "%" (fixed width, left-aligned)
};

/// Split a byte value into parts for decimal-aligned rendering
[[nodiscard]] inline auto splitBytesForAlignment(double bytes, ByteUnit unit) -> AlignedNumericParts
{
    const double value = bytes / unit.scale;
    auto wholeValue = static_cast<std::int64_t>(value);

    AlignedNumericParts parts;

    if (unit.decimals > 0)
    {
        // Extract fractional part and round to 1 decimal place
        const double fractional = std::abs(value - static_cast<double>(wholeValue));
        auto fractionalDigit = static_cast<int>(std::round(fractional * 10.0));

        // Handle rounding overflow (e.g., 0.95 -> 10 -> carry to whole part)
        if (fractionalDigit >= 10)
        {
            fractionalDigit = 0;
            wholeValue += (value >= 0) ? 1 : -1;
        }

        // Whole part includes decimal point
        parts.wholePart = std::format("{:L}.", wholeValue);
        // Single digit for fractional part
        parts.decimalPart = std::format("{}", fractionalDigit);
    }
    else
    {
        parts.wholePart = std::format("{:L}", wholeValue);
    }

    parts.unitPart = std::format(" {}", unit.suffix);
    return parts;
}

/// Split a bytes-per-second value into parts for decimal-aligned rendering
[[nodiscard]] inline auto splitBytesPerSecForAlignment(double bytesPerSec, ByteUnit unit) -> AlignedNumericParts
{
    auto parts = splitBytesForAlignment(bytesPerSec, unit);
    parts.unitPart = std::format(" {}/s", unit.suffix);
    return parts;
}

/// Split a percentage value (0-100) into parts for decimal-aligned rendering
[[nodiscard]] inline auto splitPercentForAlignment(double percent, int decimals = 1) -> AlignedNumericParts
{
    auto wholeValue = static_cast<std::int64_t>(percent);

    AlignedNumericParts parts;

    if (decimals > 0)
    {
        // Extract fractional part and round to 1 decimal place
        const double fractional = std::abs(percent - static_cast<double>(wholeValue));
        auto fractionalDigit = static_cast<int>(std::round(fractional * 10.0));

        // Handle rounding overflow (e.g., 0.95 -> 10 -> carry to whole part)
        if (fractionalDigit >= 10)
        {
            fractionalDigit = 0;
            wholeValue += (percent >= 0) ? 1 : -1;
        }

        // Whole part includes decimal point
        parts.wholePart = std::format("{:L}.", wholeValue);
        // Single digit for fractional part
        parts.decimalPart = std::format("{}", fractionalDigit);
    }
    else
    {
        parts.wholePart = std::format("{:L}", wholeValue);
    }

    parts.unitPart = "%";
    return parts;
}

/// Split a power value (watts) into parts for decimal-aligned rendering
[[nodiscard]] inline auto splitPowerForAlignment(double watts) -> AlignedNumericParts
{
    if (watts <= 0.0)
    {
        return {.wholePart = "0.", .decimalPart = "0", .unitPart = " W"};
    }

    const double absWatts = std::abs(watts);
    double displayValue = watts;
    const char* unitSuffix = "W";

    if (absWatts >= 1.0)
    {
        // Keep as watts
    }
    else if (absWatts >= 0.001)
    {
        displayValue = watts * 1000.0;
        unitSuffix = "mW";
    }
    else
    {
        displayValue = watts * 1'000'000.0;
        unitSuffix = "µW";
    }

    auto wholeValue = static_cast<std::int64_t>(displayValue);
    const double fractional = std::abs(displayValue - static_cast<double>(wholeValue));
    auto fractionalDigit = static_cast<int>(std::round(fractional * 10.0));

    // Handle rounding overflow (e.g., 0.95 -> 10 -> carry to whole part)
    if (fractionalDigit >= 10)
    {
        fractionalDigit = 0;
        wholeValue += (displayValue >= 0) ? 1 : -1;
    }

    AlignedNumericParts parts;
    // Whole part includes decimal point
    parts.wholePart = std::format("{:L}.", wholeValue);
    // Single digit for fractional part
    parts.decimalPart = std::format("{}", fractionalDigit);
    parts.unitPart = std::format(" {}", unitSuffix);
    return parts;
}

[[nodiscard]] inline auto formatCountPerSecond(double value) -> std::string
{
    if (value >= 1'000'000.0)
    {
        return std::format("{:.1Lf}M/s", static_cast<long double>(value) / 1'000'000.0L);
    }
    if (value >= 1'000.0)
    {
        return std::format("{:.1Lf}K/s", static_cast<long double>(value) / 1'000.0L);
    }
    return std::format("{:.1Lf}/s", static_cast<long double>(value));
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
    const auto totalSecs = std::llround(totalSeconds);
    constexpr long long secondsPerHour = 60LL * 60LL;
    constexpr long long secondsPerMinute = 60LL;
    const auto hours = totalSecs / secondsPerHour;
    const auto minutes = (totalSecs / secondsPerMinute) % 60;
    const auto secs = totalSecs % 60;

    if (hours > 0)
    {
        return std::format("{}:{:02}:{:02}", hours, minutes, secs);
    }

    return std::format("{}:{:02}", minutes, secs);
}

[[nodiscard]] inline auto formatCpuAffinityMask(std::uint64_t mask) -> std::string
{
    if (mask == 0)
    {
        return "-";
    }

    std::string result;
    result.reserve(64); // Reserve space for typical affinity string (avoid reallocations)
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
        return std::format("{:.2Lf} W", static_cast<long double>(watts));
    }
    if (absWatts >= 0.001)
    {
        return std::format("{:.2Lf} mW", static_cast<long double>(watts) * 1000.0L);
    }

    return std::format("{:.2Lf} µW", static_cast<long double>(watts) * 1'000'000.0L);
}

} // namespace UI::Format
