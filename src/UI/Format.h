#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <concepts>
#include <cstdint>
#include <ctime>
#include <format>
#include <functional>
#include <limits>
#include <locale>
#include <string>
#include <string_view>
#include <utility>

namespace UI::Format
{

// ============================================================================
// Locale caching for thousand separator
// ============================================================================

/// Get the locale's thousand separator character, cached per-thread for performance.
/// Uses the default C++ locale (which respects LC_* environment variables when imbued).
/// Returns '\0' if the locale has no thousand separator (C locale has empty grouping).
[[nodiscard]] inline auto getLocaleThousandSep() noexcept -> char
{
    // thread_local for thread safety, lazy-init via lambda for efficiency
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables,misc-const-correctness)
    thread_local char cachedSep = []
    {
        try
        {
            // Use std::locale() (the global C++ locale) rather than std::locale("")
            // which can crash on some libc++ configurations
            const auto& facet = std::use_facet<std::numpunct<char>>(std::locale());
            // Check if grouping is enabled - if empty, no separators should be inserted
            // This is how std::format("{:L}", ...) determines whether to use separators
            if (facet.grouping().empty())
            {
                return '\0'; // No grouping in this locale
            }
            return facet.thousands_sep();
        }
        catch (...)
        {
            return '\0'; // Fallback: no separator on error
        }
    }();
    return cachedSep;
}

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

/// Format Unix epoch timestamp to human-readable local date/time.
/// Returns "YYYY-MM-DD HH:MM:SS" format in local timezone.
/// Returns empty string if epochSeconds is 0 or conversion fails.
[[nodiscard]] inline auto formatEpochDateTime(std::uint64_t epochSeconds) -> std::string
{
    if (epochSeconds == 0)
    {
        return {};
    }

    // Guard against overflow when converting to time_t (which may be 32-bit on some platforms)
    constexpr auto maxTimeT = static_cast<std::uint64_t>(std::numeric_limits<std::time_t>::max());
    if (epochSeconds > maxTimeT)
    {
        // Out of range for this platform's time_t; cannot represent this epoch time
        return {};
    }

    // Convert epoch to local time using thread-safe localtime_r on POSIX or localtime_s on Windows
    const std::time_t epochTime = static_cast<std::time_t>(epochSeconds);
    std::tm localTm{};

#ifdef _WIN32
    // localtime_s returns non-zero errno_t on failure
    if (localtime_s(&localTm, &epochTime) != 0)
    {
        return {};
    }
#else
    // localtime_r returns nullptr on failure
    if (localtime_r(&epochTime, &localTm) == nullptr)
    {
        return {};
    }
#endif

    // Format: YYYY-MM-DD HH:MM:SS
    return std::format("{:04d}-{:02d}-{:02d} {:02d}:{:02d}:{:02d}",
                       localTm.tm_year + 1900,
                       localTm.tm_mon + 1,
                       localTm.tm_mday,
                       localTm.tm_hour,
                       localTm.tm_min,
                       localTm.tm_sec);
}

/// Format Unix epoch timestamp to a shorter format for table display.
/// Shows "HH:MM:SS" if today, "Yesterday HH:MM" if yesterday, else "MMM DD HH:MM".
/// Returns "-" if epochSeconds is 0 or conversion fails.
[[nodiscard]] inline auto formatEpochDateTimeShort(std::uint64_t epochSeconds) -> std::string
{
    if (epochSeconds == 0)
    {
        return "-";
    }

    // Guard against overflow when converting to time_t (which may be 32-bit on some platforms)
    constexpr auto maxTimeT = static_cast<std::uint64_t>(std::numeric_limits<std::time_t>::max());
    if (epochSeconds > maxTimeT)
    {
        // Out of range for this platform's time_t; cannot represent this epoch time
        return "-";
    }

    // Get current time and process start time in local timezone
    const std::time_t epochTime = static_cast<std::time_t>(epochSeconds);
    const std::time_t nowTime = std::time(nullptr);

    std::tm localTm{};
    std::tm nowTm{};

#ifdef _WIN32
    if (localtime_s(&localTm, &epochTime) != 0 || localtime_s(&nowTm, &nowTime) != 0)
    {
        return "-";
    }
#else
    if (localtime_r(&epochTime, &localTm) == nullptr || localtime_r(&nowTime, &nowTm) == nullptr)
    {
        return "-";
    }
#endif

    // Check if same day
    const bool isToday = (localTm.tm_year == nowTm.tm_year && localTm.tm_yday == nowTm.tm_yday);

    if (isToday)
    {
        // Today: show "HH:MM:SS"
        return std::format("{:02d}:{:02d}:{:02d}", localTm.tm_hour, localTm.tm_min, localTm.tm_sec);
    }

    // Check if yesterday using calendar-day comparison, including year boundaries
    bool isYesterday = false;
    if (localTm.tm_year == nowTm.tm_year)
    {
        // Same year: day-of-year must differ by exactly 1
        isYesterday = (nowTm.tm_yday - localTm.tm_yday == 1);
    }
    else if (localTm.tm_year + 1 == nowTm.tm_year)
    {
        // Previous year: local date must be the last day of its year, and today must be the first day
        const int localYear = localTm.tm_year + 1900;
        const bool isLeapYear = ((localYear % 4 == 0) && ((localYear % 100 != 0) || (localYear % 400 == 0)));
        const int daysInYear = isLeapYear ? 366 : 365;
        isYesterday = (localTm.tm_yday == (daysInYear - 1) && nowTm.tm_yday == 0);
    }

    if (isYesterday)
    {
        // Yesterday: show "Yesterday HH:MM"
        return std::format("Yesterday {:02d}:{:02d}", localTm.tm_hour, localTm.tm_min);
    }

    // Older: show "MMM DD HH:MM" (e.g., "Jan 15 14:23")
    constexpr std::array<std::string_view, 12> months = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    // tm_mon is guaranteed to be in range [0,11] by localtime_r/localtime_s for valid inputs,
    // but we check both bounds defensively and use .at() for automatic bounds checking
    const std::size_t monthIdx = (localTm.tm_mon >= 0 && localTm.tm_mon < 12) ? static_cast<std::size_t>(localTm.tm_mon) : 0;

    return std::format("{} {:2d} {:02d}:{:02d}", months.at(monthIdx), localTm.tm_mday, localTm.tm_hour, localTm.tm_min);
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

/// Zero-allocation version of AlignedNumericParts for high-frequency percent rendering.
/// Uses a fixed internal buffer and returns string_views into it.
/// Buffer is sized for percentages 0-100 with one decimal: max "100." = 4 chars + null.
struct AlignedPercentParts
{
    static constexpr std::size_t BUFFER_SIZE = 8;                                     // "100." + margin
    static constexpr std::size_t REQUIRED_SIZE = std::string_view{"100."}.size() + 1; // "100." + null terminator
    static_assert(BUFFER_SIZE >= REQUIRED_SIZE, "AlignedPercentParts::BUFFER_SIZE is too small for worst-case percent value");
    std::array<char, BUFFER_SIZE> buffer{};           // Internal storage
    std::string_view wholePart;                       // View into buffer: "XX." or "X."
    char decimalDigit = '0';                          // Single char: '0'-'9'
    static constexpr std::string_view unitPart = "%"; // Always "%" for percentages
};

/// Zero-allocation version of AlignedNumericParts for byte value rendering.
/// Uses a fixed internal buffer and returns string_views into it.
/// Buffer sized for: sign + 20 digits + 6 separators + decimal point + null = 29 chars
struct AlignedBytesParts
{
    static constexpr std::size_t BUFFER_SIZE = 32; // Enough for any int64_t with separators
    std::array<char, BUFFER_SIZE> buffer{};        // Internal storage for whole part
    std::size_t wholePartLen = 0;                  // Length of whole part in buffer
    char decimalDigit = '0';                       // Single char: '0'-'9'
    std::string_view unitPart;                     // " KB", " MB", etc. (from ByteUnit::suffix)

    /// Get the whole part as a string_view into the buffer
    [[nodiscard]] constexpr auto wholePart() const noexcept -> std::string_view
    {
        return {buffer.data(), wholePartLen};
    }
};

/// Zero-allocation fast path for splitting byte values for decimal-aligned rendering.
/// This function produces equivalent output to splitBytesForAlignment but avoids
/// std::format overhead and heap allocations. Use for high-frequency rendering.
[[nodiscard]] inline auto splitBytesForAlignmentFast(double bytes, ByteUnit unit) -> AlignedBytesParts
{
    const double value = bytes / unit.scale;
    auto wholeValue = static_cast<std::int64_t>(value);
    const bool isNegative = (wholeValue < 0);

    // Extract fractional part and round to 1 decimal place
    const double fractional = std::abs(value - static_cast<double>(wholeValue));
    auto fractionalDigit = static_cast<int>(std::round(fractional * 10.0));

    // Handle rounding overflow (e.g., 0.95 -> 10 -> carry to whole part)
    if (fractionalDigit >= 10)
    {
        fractionalDigit = 0;
        wholeValue += (value >= 0) ? 1 : -1;
    }

    AlignedBytesParts parts;
    std::size_t pos = 0;

    // Handle negative sign
    if (isNegative)
    {
        parts.buffer[pos++] = '-';
        wholeValue = -wholeValue; // Work with positive value for digit extraction
    }

    // Convert integer to string using std::to_chars (fast, no allocation)
    // Then insert thousand separators
    std::array<char, 24> digitBuf{}; // Enough for int64_t
    auto [endPtr, ec] = std::to_chars(digitBuf.data(), digitBuf.data() + digitBuf.size(), wholeValue);
    const std::size_t numDigits = static_cast<std::size_t>(endPtr - digitBuf.data());

    // Get locale separator ('\0' means no separators, e.g., C locale)
    const char sep = getLocaleThousandSep();

    // Insert digits with thousand separators (if separator is enabled)
    // For numDigits=6 (e.g., 123456), separators go after positions 0 and 3 (before digits 3 and 6)
    // Pattern: 123,456 - separator after every 3 digits from the right
    // firstGroupSize: number of digits before first separator (1-3)
    const std::size_t firstGroupSize = ((numDigits - 1) % 3) + 1;

    for (std::size_t i = 0; i < numDigits; ++i)
    {
        // Add separator before this digit if:
        // 1. sep != '\0' (locale has grouping enabled)
        // 2. We're past the first group
        // 3. We're at a group boundary
        if (sep != '\0' && i >= firstGroupSize && (i - firstGroupSize) % 3 == 0)
        {
            parts.buffer[pos++] = sep;
        }
        parts.buffer[pos++] = digitBuf[i];
    }

    // Add decimal point (unit.decimals is always 1 for byte units)
    if (unit.decimals > 0)
    {
        parts.buffer[pos++] = '.';
    }
    parts.buffer[pos] = '\0';

    parts.wholePartLen = pos;
    parts.decimalDigit = static_cast<char>('0' + fractionalDigit);

    // Unit part: " B", " KB", " MB", " GB" - we need to prepend space
    // Store as string_view pointing to static string
    static constexpr std::string_view unitB = " B";
    static constexpr std::string_view unitKB = " KB";
    static constexpr std::string_view unitMB = " MB";
    static constexpr std::string_view unitGB = " GB";

    // Match unit suffix to our static strings
    if (unit.suffix[0] == 'G')
    {
        parts.unitPart = unitGB;
    }
    else if (unit.suffix[0] == 'M')
    {
        parts.unitPart = unitMB;
    }
    else if (unit.suffix[0] == 'K')
    {
        parts.unitPart = unitKB;
    }
    else
    {
        parts.unitPart = unitB;
    }

    return parts;
}

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
/// Zero-allocation fast path for percentages in the 0-100 range (typical CPU%, MEM%)
[[nodiscard]] inline auto splitPercentForAlignment(double percent) -> AlignedPercentParts
{
    // Clamp to valid percentage range
    percent = std::clamp(percent, 0.0, 100.0);

    auto wholeValue = static_cast<int>(percent);

    // Extract fractional part and round to 1 decimal place
    const double fractional = percent - static_cast<double>(wholeValue);
    auto fractionalDigit = static_cast<int>(std::round(fractional * 10.0));

    // Handle rounding overflow (e.g., 99.95 -> fractional rounds to 10)
    if (fractionalDigit >= 10)
    {
        fractionalDigit = 0;
        wholeValue++;
    }

    // Defensive clamp: ensure wholeValue remains within valid percentage bounds [0, 100]
    wholeValue = std::clamp(wholeValue, 0, 100);

    AlignedPercentParts parts;

    // Format whole part directly into buffer (no locale, no allocation)
    // Max value is 100, so at most 3 digits + decimal point + null = 5 chars
    // Use index counter instead of pointer arithmetic for safety
    std::size_t pos = 0;

    if (wholeValue >= 100)
    {
        parts.buffer[pos++] = '1';
        parts.buffer[pos++] = '0';
        parts.buffer[pos++] = '0';
    }
    else if (wholeValue >= 10)
    {
        parts.buffer[pos++] = static_cast<char>('0' + (wholeValue / 10));
        parts.buffer[pos++] = static_cast<char>('0' + (wholeValue % 10));
    }
    else
    {
        parts.buffer[pos++] = static_cast<char>('0' + wholeValue);
    }
    parts.buffer[pos++] = '.'; // Decimal point
    parts.buffer[pos] = '\0';  // Null terminate

    parts.wholePart = std::string_view(parts.buffer.data(), pos);
    parts.decimalDigit = static_cast<char>('0' + fractionalDigit);

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
