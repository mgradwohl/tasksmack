#pragma once

#include "Domain/Numeric.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <charconv>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <format>
#include <functional>
#include <iterator>
#include <limits>
#include <locale>
#include <span>
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
///
/// @note The separator is cached on first access per thread and will NOT update if the
///       global locale changes at runtime. This is acceptable since TaskSmack sets the
///       locale once at startup and does not change it afterwards.
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

/// The global locale's decimal point, the one std::format's "L" specs print, so the table's aligned
/// cells read "1,5 MB" beside a tooltip's "1,5 MB" in a comma-decimal locale (#1202); '.' in the
/// "C" locale. Not cached: read once per call from std::locale() (a reference-count bump, no
/// allocation), so it always agrees with the "L" formatters even if the global locale changes.
[[nodiscard]] inline auto getLocaleDecimalPoint() noexcept -> char
{
    try
    {
        return std::use_facet<std::numpunct<char>>(std::locale()).decimal_point();
    }
    catch (...)
    {
        return '.';
    }
}

// ============================================================================
// Allocation-free localized fixed-point formatting (#1334)
// ============================================================================

/// The global locale's numeric punctuation, as std::format's "L" specs apply it to a number.
struct NumericPunctuation
{
    char decimalPoint = '.';
    char thousandsSep = ',';
    std::string grouping; // numpunct::grouping(): empty for no separators ("C" locale)
};

/// The global locale's punctuation, cached per thread and re-read whenever the global locale is
/// replaced (the test suites switch it; TaskSmack sets it once at startup). Checking costs one
/// std::locale() copy and a pointer compare, against the locale lookup, facet calls and grouping
/// string std::format("{:L}") pays on every call.
[[nodiscard]] inline auto numericPunctuation() noexcept -> const NumericPunctuation&
{
    // NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables,misc-const-correctness)
    thread_local std::locale cachedLocale = std::locale::classic();
    thread_local NumericPunctuation cached{.decimalPoint = '.', .thousandsSep = ',', .grouping = {}};
    // NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables,misc-const-correctness)
    try
    {
        const std::locale current;
        if (current != cachedLocale)
        {
            // Build the new value first and commit the key last, so a throw leaves the old pair intact.
            const auto& facet = std::use_facet<std::numpunct<char>>(current);
            NumericPunctuation fresh{
                .decimalPoint = facet.decimal_point(), .thousandsSep = facet.thousands_sep(), .grouping = facet.grouping()};
            cached = std::move(fresh);
            cachedLocale = current;
        }
        return cached;
    }
    catch (...)
    {
        return cached; // The last punctuation read (the "C" locale's until one is read successfully)
    }
}

/// Writes `value` with `decimals` fraction digits into [out, out + capacity), exactly as
/// std::format("{:.{}Lf}", value, decimals) prints it with the global locale: its decimal point,
/// and its thousands separator placed by its grouping. No allocation and no locale lookup per call,
/// which is what made every chart axis tick about twice as slow once the axes went localized (#1334).
/// Returns the length written, or 0 if it doesn't fit, `value` isn't finite, or `decimals` is outside
/// 0-9; the caller then falls back to std::format, so the output never differs from it.
[[nodiscard]] inline auto formatFixedLocalizedTo(char* out, std::size_t capacity, double value, int decimals) noexcept -> std::size_t
{
    constexpr int MAX_DECIMALS = 9;
    if (!std::isfinite(value) || decimals < 0 || decimals > MAX_DECIMALS)
    {
        return 0;
    }
    // std::format's "f" is to_chars' fixed format: up to 309 integer digits, a sign and a point.
    std::array<char, 330> digits{};
    const auto [digitsEnd, error] = std::to_chars(digits.data(), digits.data() + digits.size(), value, std::chars_format::fixed, decimals);
    if (error != std::errc{})
    {
        return 0;
    }

    const NumericPunctuation& punct = numericPunctuation();
    const char* first = digits.data();
    std::size_t length = 0;
    const auto put = [out, capacity, &length](char c) noexcept
    {
        if (length < capacity)
        {
            out[length] = c;
        }
        ++length;
    };

    if (*first == '-')
    {
        put('-');
        ++first;
    }
    const char* const textEnd = digitsEnd;
    const char* integerEnd = std::find(first, textEnd, '.');
    const auto integerDigits = static_cast<std::size_t>(integerEnd - first);

    // Separator positions, as the count of integer digits to their right: numpunct grouping gives
    // the group sizes from the right, its last entry repeating; a size <= 0 or CHAR_MAX ends grouping.
    std::array<std::size_t, 320> cuts{};
    std::size_t cutCount = 0;
    if (!punct.grouping.empty())
    {
        std::size_t position = 0;
        for (std::size_t group = 0; cutCount < cuts.size(); ++group)
        {
            const char size = punct.grouping[std::min(group, punct.grouping.size() - 1)];
            if (size <= 0 || size == std::numeric_limits<char>::max())
            {
                break;
            }
            position += static_cast<std::size_t>(size);
            if (position >= integerDigits)
            {
                break;
            }
            cuts[cutCount++] = position;
        }
    }

    for (std::size_t i = 0; i < integerDigits; ++i)
    {
        put(first[i]);
        const std::size_t toTheRight = integerDigits - i - 1;
        if (cutCount > 0 && toTheRight == cuts[cutCount - 1])
        {
            put(punct.thousandsSep);
            --cutCount;
        }
    }
    if (integerEnd != textEnd)
    {
        put(punct.decimalPoint);
        for (const char* p = integerEnd + 1; p != textEnd; ++p)
        {
            put(*p);
        }
    }
    return length <= capacity ? length : 0;
}

/// Appends `text` at out[length], tracking the length past `capacity` so the caller can tell it
/// didn't fit (formatFixedLocalizedTo()'s convention).
inline void appendText(char* out, std::size_t capacity, std::size_t& length, std::string_view text) noexcept
{
    for (const char c : text)
    {
        if (length < capacity)
        {
            out[length] = c;
        }
        ++length;
    }
}

/// std::format("{:.{}Lf}{}", value, decimals, suffix) through formatFixedLocalizedTo(): the same
/// text, built in a stack buffer instead of through std::format's locale-aware path (#1334).
[[nodiscard]] inline auto formatFixedLocalized(double value, int decimals, std::string_view suffix) -> std::string
{
    std::array<char, 64> buffer{};
    std::size_t length = formatFixedLocalizedTo(buffer.data(), buffer.size(), value, decimals);
    if (length > 0)
    {
        appendText(buffer.data(), buffer.size(), length, suffix);
        if (length <= buffer.size())
        {
            return {buffer.data(), length};
        }
    }
    return std::format("{:.{}Lf}{}", value, decimals, suffix);
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
    // NaN (a sample with no reading) would survive std::max and reach std::lround, whose result
    // is unspecified for it -- "-2,147,483,648%" in practice (#1148). Infinity likewise.
    if (!(percent > static_cast<T>(0))) // Also catches NaN
    {
        return 0;
    }
    // Saturate before rounding: std::lround is unspecified outside long's range, which is only
    // 32 bits on Windows, so a huge finite value (or infinity) must not reach it (#1227 review).
    constexpr auto INT_LIMIT = static_cast<double>(std::numeric_limits<int>::max());
    if (static_cast<double>(percent) >= INT_LIMIT)
    {
        return std::numeric_limits<int>::max();
    }
    return toIntSaturated(std::lround(static_cast<double>(percent)));
}

/// "42%", or "N/A" for NaN, which marks a sample with no reading.
template<std::floating_point T> [[nodiscard]] inline auto percentCompact(T percent) -> std::string
{
    if (std::isnan(percent))
    {
        return "N/A";
    }
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
    return std::format("{:.{}Lf}", value, decimals);
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

// ============================================================================
// Durations (#1202): CPU Time, uptime and the history charts' time axis
// ============================================================================

/// How formatDuration() treats a zero second part.
enum class DurationStyle : std::uint8_t
{
    /// Always two parts once a minute has passed ("5m 00s"), so a live value such as a process's
    /// CPU Time keeps its width as it ticks over.
    Fixed,
    /// A zero second part is left off ("5m", "1h"), for the round values of an axis tick.
    Compact,
};

/// A duration in the app's one grammar (#1202): "45s", "2m 05s", "1h 02m", "3d 04h" -- the two
/// largest units, the second padded to two digits, rounded to the nearest second first. Negative
/// durations are shown by their size and NaN as "N/A". Used for CPU Time, uptime and the history
/// charts' time axis, which used to print "1:02:05", "Up: 3d 4h 5m" and "-300" respectively.
[[nodiscard]] inline auto formatDuration(double seconds, DurationStyle style = DurationStyle::Fixed) -> std::string
{
    if (std::isnan(seconds))
    {
        return "N/A";
    }
    constexpr double MAX_SECONDS = 1e15; // Well inside long long; keeps std::llround defined
    const long long total = std::llround(std::min(std::abs(seconds), MAX_SECONDS));
    constexpr long long MINUTE = 60;
    constexpr long long HOUR = 60 * MINUTE;
    constexpr long long DAY = 24 * HOUR;

    const auto twoParts = [style](long long major, char majorUnit, long long minor, char minorUnit)
    {
        if (style == DurationStyle::Compact && minor == 0)
        {
            return std::format("{}{}", major, majorUnit);
        }
        return std::format("{}{} {:02}{}", major, majorUnit, minor, minorUnit);
    };
    if (total < MINUTE)
    {
        return std::format("{}s", total);
    }
    if (total < HOUR)
    {
        return twoParts(total / MINUTE, 'm', total % MINUTE, 's');
    }
    if (total < DAY)
    {
        return twoParts(total / HOUR, 'h', (total % HOUR) / MINUTE, 'm');
    }
    return twoParts(total / DAY, 'd', (total % DAY) / HOUR, 'h');
}

/// "Up: 3d 04h", the system uptime in formatDuration()'s grammar; empty for 0 (not known).
[[nodiscard]] inline auto formatUptimeShort(std::uint64_t seconds) -> std::string
{
    if (seconds == 0)
    {
        return {};
    }
    return "Up: " + formatDuration(static_cast<double>(seconds));
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
    const auto epochTime = static_cast<std::time_t>(epochSeconds);
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
    const auto epochTime = static_cast<std::time_t>(epochSeconds);
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
    std::string_view suffix = "B";
    double scale = 1.0;
    int decimals = 0;
};

/// The byte units, binary multiples labelled with their IEC names (KiB, MiB, GiB, TiB) app-wide
/// (#1202). Every size and rate in TaskSmack was already counted in powers of 1024 -- memory, disk
/// and network alike -- but labelled "KB/MB/GB", which are decimal names: a "1.0 GB" that is
/// 1,073,741,824 bytes. Relabelling keeps every number the user sees and makes the unit honest;
/// switching to decimal units instead would have changed every number in the app.
///
/// Named constants so a chart can hand ImPlot a stable pointer to the one unit its whole axis is
/// labelled in (see byteUnitFor()).
inline constexpr ByteUnit BYTE_UNIT_TB{.suffix = "TiB", .scale = 1024.0 * 1024.0 * 1024.0 * 1024.0, .decimals = 1};
inline constexpr ByteUnit BYTE_UNIT_GB{.suffix = "GiB", .scale = 1024.0 * 1024.0 * 1024.0, .decimals = 1};
inline constexpr ByteUnit BYTE_UNIT_MB{.suffix = "MiB", .scale = 1024.0 * 1024.0, .decimals = 1};
inline constexpr ByteUnit BYTE_UNIT_KB{.suffix = "KiB", .scale = 1024.0, .decimals = 1};
inline constexpr ByteUnit BYTE_UNIT_B{.suffix = "B", .scale = 1.0, .decimals = 1};

/// Every byte unit, largest first.
inline constexpr std::array<const ByteUnit*, 5> BYTE_UNITS = {&BYTE_UNIT_TB, &BYTE_UNIT_GB, &BYTE_UNIT_MB, &BYTE_UNIT_KB, &BYTE_UNIT_B};

/// The largest unit `bytes` is at least one of, as a reference to one of the BYTE_UNIT_* constants.
/// Terabytes have their own unit, so a 2 TiB disk or a byte axis above 1 TiB reads "2.0 TiB" and
/// steps in TiB rather than "2,048.0 GB" (#1202).
[[nodiscard]] inline auto byteUnitFor(double bytes) -> const ByteUnit&
{
    const double absBytes = std::abs(bytes);
    if (absBytes >= BYTE_UNIT_TB.scale)
    {
        return BYTE_UNIT_TB;
    }
    if (absBytes >= BYTE_UNIT_GB.scale)
    {
        return BYTE_UNIT_GB;
    }
    if (absBytes >= BYTE_UNIT_MB.scale)
    {
        return BYTE_UNIT_MB;
    }
    if (absBytes >= BYTE_UNIT_KB.scale)
    {
        return BYTE_UNIT_KB;
    }
    return BYTE_UNIT_B;
}

[[nodiscard]] inline auto chooseByteUnit(double bytes) -> ByteUnit
{
    return byteUnitFor(bytes);
}

[[nodiscard]] inline auto unitForTotalBytes(std::uint64_t bytes) -> ByteUnit
{
    return chooseByteUnit(static_cast<double>(bytes));
}

[[nodiscard]] inline auto unitForBytesPerSecond(double bytesPerSec) -> ByteUnit
{
    return chooseByteUnit(bytesPerSec);
}

/// Value rounded to decimals places, halves away from zero, as the table's aligned cells round
/// (splitBytesForAlignment() and friends). std::format alone rounds an exact half to even, so a
/// binary-exact 3.25 MB read "3.2 MB" in a tooltip beside "3.3 MB" in the table (#1202).
[[nodiscard]] inline auto roundHalfAwayFromZero(double value, int decimals) -> double
{
    const double factor = std::pow(10.0, decimals);
    const double rounded = std::round(value * factor) / factor;
    return std::isfinite(rounded) ? rounded : value;
}

/// formatBytesWithUnit() / formatBytesPerSecWithUnit() written into [out, out + capacity) ("1.5 GB",
/// "1.5 GB/s"), without allocating: chart axis ticks call this for every label every frame (#1334).
/// Returns the length, or 0 if it doesn't fit or the value isn't finite.
[[nodiscard]] inline auto formatBytesWithUnitTo(char* out, std::size_t capacity, double bytes, ByteUnit unit, bool perSecond) noexcept
    -> std::size_t
{
    const double value = roundHalfAwayFromZero(bytes / unit.scale, unit.decimals);
    std::size_t length = formatFixedLocalizedTo(out, capacity, value, unit.decimals);
    if (length == 0)
    {
        return 0;
    }
    appendText(out, capacity, length, " ");
    appendText(out, capacity, length, unit.suffix);
    if (perSecond)
    {
        appendText(out, capacity, length, "/s");
    }
    return length <= capacity ? length : 0;
}

/// The longest byte value a stack buffer holds before formatBytesWithUnit() falls back to std::format.
inline constexpr std::size_t BYTE_TEXT_CAPACITY = 64;

[[nodiscard]] inline auto formatBytesWithUnit(double bytes, ByteUnit unit) -> std::string
{
    std::array<char, BYTE_TEXT_CAPACITY> buffer{};
    if (const std::size_t length = formatBytesWithUnitTo(buffer.data(), buffer.size(), bytes, unit, false); length > 0)
    {
        return {buffer.data(), length};
    }
    const double value = roundHalfAwayFromZero(bytes / unit.scale, unit.decimals);
    return std::format("{:.{}Lf} {}", value, unit.decimals, unit.suffix);
}

[[nodiscard]] inline auto formatBytes(double bytes) -> std::string
{
    const auto unit = chooseByteUnit(bytes);
    return formatBytesWithUnit(bytes, unit);
}

[[nodiscard]] inline auto formatBytesPerSecWithUnit(double bytesPerSec, ByteUnit unit) -> std::string
{
    std::array<char, BYTE_TEXT_CAPACITY> buffer{};
    if (const std::size_t length = formatBytesWithUnitTo(buffer.data(), buffer.size(), bytesPerSec, unit, true); length > 0)
    {
        return {buffer.data(), length};
    }
    return formatBytesWithUnit(bytesPerSec, unit) + "/s";
}

[[nodiscard]] inline auto formatBytesPerSec(double bytesPerSec) -> std::string
{
    const auto unit = chooseByteUnit(bytesPerSec);
    return formatBytesPerSecWithUnit(bytesPerSec, unit);
}

/// formatBytesPerSec for a history sample: "N/A" for NaN, which marks a sample where nothing was
/// measured (the interface or disk was absent), so a tooltip never shows "nan B/s" (#1015).
[[nodiscard]] inline auto formatBytesPerSecOrNA(double bytesPerSec) -> std::string
{
    return std::isfinite(bytesPerSec) ? formatBytesPerSec(bytesPerSec) : std::string{"N/A"};
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

/// A table cell's unit, with the space before it: " MiB" or, for a rate, " MiB/s". Static strings,
/// so AlignedBytesParts can point at them; a unit that is not one of BYTE_UNITS gets " B".
[[nodiscard]] inline auto cellUnitSuffix(const ByteUnit& unit, bool perSecond) noexcept -> std::string_view
{
    struct CellSuffix
    {
        std::string_view suffix;
        std::string_view size;
        std::string_view rate;
    };
    static constexpr auto CELL_SUFFIXES = std::to_array<CellSuffix>({
        {.suffix = "TiB", .size = " TiB", .rate = " TiB/s"},
        {.suffix = "GiB", .size = " GiB", .rate = " GiB/s"},
        {.suffix = "MiB", .size = " MiB", .rate = " MiB/s"},
        {.suffix = "KiB", .size = " KiB", .rate = " KiB/s"},
        {.suffix = "B", .size = " B", .rate = " B/s"},
    });
    // NOLINTNEXTLINE(readability-qualified-auto) - iterator type varies by platform
    const auto it = std::ranges::find(CELL_SUFFIXES, unit.suffix, &CellSuffix::suffix);
    const CellSuffix& found = (it != CELL_SUFFIXES.end()) ? *it : CELL_SUFFIXES.back();
    return perSecond ? found.rate : found.size;
}

/// Zero-allocation fast path for splitting byte values for decimal-aligned rendering.
/// This function produces equivalent output to splitBytesForAlignment but avoids
/// std::format overhead and heap allocations. Use for high-frequency rendering.
[[nodiscard]] inline auto splitBytesForAlignmentFast(double bytes, ByteUnit unit) -> AlignedBytesParts
{
    // Clamp negative values to 0 (bytes should never be negative in practice)
    bytes = std::max(0.0, bytes);

    const double value = bytes / unit.scale;
    auto wholeValue = static_cast<std::int64_t>(value);

    // Extract fractional part and round to 1 decimal place
    const double fractional = value - static_cast<double>(wholeValue);
    auto fractionalDigit = static_cast<int>(std::round(fractional * 10.0));

    // Handle rounding overflow (e.g., 0.95 -> 10 -> carry to whole part)
    if (fractionalDigit >= 10)
    {
        fractionalDigit = 0;
        wholeValue += 1;
    }

    AlignedBytesParts parts;
    std::size_t pos = 0;

    // Convert integer to string using std::to_chars (fast, no allocation)
    // Then insert thousand separators
    std::array<char, 24> digitBuf{}; // Enough for int64_t
    auto [endPtr, ec] = std::to_chars(digitBuf.data(), digitBuf.data() + digitBuf.size(), wholeValue);
    if (ec != std::errc{})
    {
        // Fallback: return a safe default if conversion fails (should never happen)
        digitBuf[0] = '0';
        endPtr = digitBuf.data() + 1;
    }
    assert(ec == std::errc{} && "std::to_chars failed unexpectedly");
    const auto numDigits = static_cast<std::size_t>(endPtr - digitBuf.data());

    // Get locale separator ('\0' means no separators, e.g., C locale)
    const char sep = getLocaleThousandSep();

    // Max buffer usage (by design): at most 20 digits + 6 separators + 1 decimal + 1 null = 28 < BUFFER_SIZE(32)
    // Invariant: numDigits must never exceed 20; if it does, that indicates a logic bug upstream.
    // The assert enforces this invariant in debug builds, while the runtime clamp provides
    // defense-in-depth in release builds (where asserts are disabled) to prevent buffer overflow.
    assert(numDigits <= 20 && "Unexpected number of digits in byte value");
    const std::size_t safeNumDigits = std::min(numDigits, std::size_t{20});

    // Insert digits with thousand separators (if separator is enabled)
    // For numDigits=6 (e.g., 123456), a separator is inserted before digit index 3, yielding "123,456"
    // Pattern: effectively a separator after every 3 digits when scanning from the left
    // firstGroupSize: number of digits before the first separator (1-3)
    // Use safeNumDigits to ensure consistency with the loop constraint
    const std::size_t firstGroupSize = ((safeNumDigits - 1) % 3) + 1;

    for (std::size_t i = 0; i < safeNumDigits; ++i)
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
    // Verify we have room for decimal point + null terminator (2 chars)
    assert(pos + 2 <= AlignedBytesParts::BUFFER_SIZE && "Buffer overflow in splitBytesForAlignmentFast");
    if (unit.decimals > 0)
    {
        parts.buffer[pos++] = getLocaleDecimalPoint(); // As formatBytes()'s "L" spec prints it (#1202)
    }
    parts.buffer[pos] = '\0';

    parts.wholePartLen = pos;
    parts.decimalDigit = static_cast<char>('0' + fractionalDigit);

    parts.unitPart = cellUnitSuffix(unit, false);
    return parts;
}

/// Zero-allocation fast path for splitting bytes-per-second values for decimal-aligned rendering.
/// Identical to splitBytesForAlignmentFast but uses "/s" unit suffixes (" B/s", " KB/s", etc.).
[[nodiscard]] inline auto splitBytesPerSecForAlignmentFast(double bytesPerSec, ByteUnit unit) -> AlignedBytesParts
{
    auto parts = splitBytesForAlignmentFast(bytesPerSec, unit);
    parts.unitPart = cellUnitSuffix(unit, true);
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

        // Whole part includes the locale's decimal point, as formatBytes() prints it (#1202)
        parts.wholePart = std::format("{:L}{}", wholeValue, getLocaleDecimalPoint());
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
    parts.buffer[pos++] = getLocaleDecimalPoint(); // As formatPercent()'s "L" spec prints it (#1202)
    parts.buffer[pos] = '\0';                      // Null terminate

    parts.wholePart = std::string_view(parts.buffer.data(), pos);
    parts.decimalDigit = static_cast<char>('0' + fractionalDigit);

    return parts;
}

/// "0.6%", one decimal, exactly as the Processes table shows a process's CPU and memory percents
/// (the same splitPercentForAlignment() rounding and locale decimal point), or "N/A" for NaN. A
/// process's share of the machine is usually under a few percent, where percentCompact() rounded it
/// to "0%" or "1%" (#1195).
[[nodiscard]] inline auto percentOneDecimal(double percent) -> std::string
{
    if (std::isnan(percent))
    {
        return "N/A";
    }
    const auto parts = splitPercentForAlignment(percent);
    std::string out(parts.wholePart);
    out.push_back(parts.decimalDigit);
    out.append(AlignedPercentParts::unitPart);
    return out;
}

/// Split a power value (watts) into parts for decimal-aligned rendering
[[nodiscard]] inline auto splitPowerForAlignment(double watts) -> AlignedNumericParts
{
    if (watts <= 0.0)
    {
        return {.wholePart = std::format("0{}", getLocaleDecimalPoint()), .decimalPart = "0", .unitPart = " W"};
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
    // Whole part includes the locale's decimal point, as formatWatts() prints it (#1202)
    parts.wholePart = std::format("{:L}{}", wholeValue, getLocaleDecimalPoint());
    // Single digit for fractional part
    parts.decimalPart = std::format("{}", fractionalDigit);
    parts.unitPart = std::format(" {}", unitSuffix);
    return parts;
}

[[nodiscard]] inline auto formatCountPerSecond(double value) -> std::string
{
    if (value >= 1'000'000.0)
    {
        return std::format("{:.1Lf}M/s", value / 1'000'000.0);
    }
    if (value >= 1'000.0)
    {
        return std::format("{:.1Lf}K/s", value / 1'000.0);
    }
    return std::format("{:.1Lf}/s", value);
}

/// " (16 logical processors @ 3.70 GHz)", or " (16 logical processors)" without a clock, for the
/// suffix after the CPU model. The count is of logical processors (hardware threads), which is
/// what the OS reports per CPU slot; "cores" overstated it on SMT machines (#1203).
[[nodiscard]] inline auto formatLogicalProcessorSummary(int logicalProcessors, double freqMHz) -> std::string
{
    const char* noun = (logicalProcessors == 1) ? "logical processor" : "logical processors";
    if (freqMHz > 0.0)
    {
        return std::format(" ({} {} @ {:.2f} GHz)", logicalProcessors, noun, freqMHz / 1000.0);
    }
    return std::format(" ({} {})", logicalProcessors, noun);
}

/// The processors in a CPU affinity bitset (`words`: 64-bit words, processors 0-63 first), listed
/// compactly in ascending order: runs of three or more as a range ("4-7"), pairs and singles by
/// number ("0,1", "9"), e.g. "0-3,64-127,200". "-" when no processor is set (affinity unread).
/// Any width: an affinity can include processors at 64 and above (#1247).
[[nodiscard]] inline auto formatCpuAffinity(std::span<const std::uint64_t> words) -> std::string
{
    constexpr std::size_t BITS_PER_WORD = 64;
    const std::size_t bitCount = words.size() * BITS_PER_WORD;
    const auto isSet = [words](std::size_t cpu) -> bool
    {
        return ((words[cpu / BITS_PER_WORD] >> (cpu % BITS_PER_WORD)) & 1U) != 0;
    };

    std::string result;
    std::size_t cpu = 0;
    while (cpu < bitCount)
    {
        if (cpu % BITS_PER_WORD == 0 && words[cpu / BITS_PER_WORD] == 0)
        {
            cpu += BITS_PER_WORD; // A whole word with no processor in it
            continue;
        }
        if (!isSet(cpu))
        {
            ++cpu;
            continue;
        }
        const std::size_t first = cpu;
        while (cpu < bitCount && isSet(cpu))
        {
            ++cpu;
        }
        const std::size_t last = cpu - 1;
        if (!result.empty())
        {
            result += ',';
        }
        if (first == last)
        {
            std::format_to(std::back_inserter(result), "{}", first);
        }
        else if (first + 1 == last)
        {
            std::format_to(std::back_inserter(result), "{},{}", first, last);
        }
        else
        {
            std::format_to(std::back_inserter(result), "{}-{}", first, last);
        }
    }
    return result.empty() ? std::string("-") : result;
}

/// formatCpuAffinity() of a 64-bit mask, bit N = processor N.
[[nodiscard]] inline auto formatCpuAffinityMask(std::uint64_t mask) -> std::string
{
    return formatCpuAffinity(std::span<const std::uint64_t>(&mask, 1));
}

// ============================================================================
// One number-and-unit grammar for values and chart axes (#1202)
//
// A space before every unit except %, one decimal for bytes and watts, localized. The chart axis
// formatters in ChartWidgets.h are thin adapters over these, so an axis tick, a tooltip and a
// table cell show the same quantity the same way.
// ============================================================================

/// "45.0 W", "15.0 mW", "500.0 µW", one decimal in the unit its magnitude calls for; "0.0 W" for
/// zero. Signed: a negative value keeps its sign. The same rounding and units as the Processes
/// table's Power column (splitPowerForAlignment()).
[[nodiscard]] inline auto formatWatts(double watts) -> std::string
{
    if (watts == 0.0)
    {
        return formatFixedLocalized(0.0, 1, " W"); // Also -0.0, which would print as "-0.0 W"
    }
    const double absWatts = std::abs(watts);
    if (absWatts >= 1.0)
    {
        return formatFixedLocalized(roundHalfAwayFromZero(watts, 1), 1, " W");
    }
    if (absWatts >= 0.001)
    {
        return formatFixedLocalized(roundHalfAwayFromZero(watts * 1000.0, 1), 1, " mW");
    }
    return formatFixedLocalized(roundHalfAwayFromZero(watts * 1'000'000.0, 1), 1, " µW");
}

/// "42%" from 10 % up, "4.2%" below it (where a whole number would read 0 % or 1 % for most
/// processes), "0%" for anything that rounds to zero, "N/A" for NaN (no reading). Localized.
[[nodiscard]] inline auto formatPercent(double percent) -> std::string
{
    if (std::isnan(percent))
    {
        return "N/A";
    }
    // Under 0.05 % rounds to zero: one canonical "0%", never "0.0%" or "-0.0%", so a value and the
    // chart axis tick beside it (formatAxisPercent()) read alike (#1202).
    if (std::abs(percent) < 0.05)
    {
        return "0%";
    }
    // Decide on the rounded value, so 9.96 becomes "10%" rather than "10.0%".
    const bool wholeNumber = std::abs(percent) >= 9.95;
    return wholeNumber ? formatFixedLocalized(roundHalfAwayFromZero(percent, 0), 0, "%")
                       : formatFixedLocalized(roundHalfAwayFromZero(percent, 1), 1, "%");
}

/// formatPercent() for a float history sample, so the panels' float series need no cast.
[[nodiscard]] inline auto formatPercent(float percent) -> std::string
{
    return formatPercent(static_cast<double>(percent));
}

/// A whole-number percent (battery health), "87%".
template<std::integral T> [[nodiscard]] inline auto formatPercent(T percent) -> std::string
{
    return std::format("{:L}%", percent);
}

/// "3.2 GiB / 16.0 GiB (20%)": used and total in the one unit the larger of them calls for.
[[nodiscard]] inline auto bytesUsedTotalPercentCompact(std::uint64_t usedBytes, std::uint64_t totalBytes, double percent) -> std::string
{
    const auto unit = unitForTotalBytes(std::max(usedBytes, totalBytes));
    const std::string usedStr = formatBytesWithUnit(static_cast<double>(usedBytes), unit);
    const std::string totalStr = formatBytesWithUnit(static_cast<double>(totalBytes), unit);
    return std::format("{} / {} ({})", usedStr, totalStr, formatPercent(percent));
}

/// Format power value with appropriate unit (W/mW/µW) based on magnitude, one decimal (#1202),
/// or "-" for zero or below.
[[nodiscard]] inline auto formatPowerCompact(double watts) -> std::string
{
    if (watts <= 0.0)
    {
        return "-";
    }
    return formatWatts(watts);
}

/// Format power value for per-process consumption contexts.
/// Returns "0.0 W" for zero; also clamps negative values to "0.0 W" because
/// per-process energy counters use 0.0 as the sentinel for "not yet measured"
/// and per-process power is never negative. Do NOT use this formatter for
/// system/battery power (Domain::PowerStatus::powerWatts), which is signed and
/// may be negative while the battery is charging. formatPowerCompact() also
/// does not preserve negative values (it returns "-" for watts <= 0.0), so a
/// dedicated signed-power formatter is required if that use case is needed.
[[nodiscard]] inline auto formatPowerOrZero(double watts) -> std::string
{
    if (watts <= 0.0)
    {
        return formatWatts(0.0);
    }
    return formatPowerCompact(watts);
}

/// "65°C", whole degrees rounded half away from zero, or "N/A" for NaN (no reading). The one rule
/// for a temperature (#1202): the GPU value strip truncated (65.9 -> "65°C") while its tooltip
/// rounded to even.
[[nodiscard]] inline auto formatCelsius(double celsius) -> std::string
{
    if (std::isnan(celsius))
    {
        return "N/A";
    }
    const double rounded = roundHalfAwayFromZero(celsius, 0);
    return std::format("{:.0f}°C", rounded == 0.0 ? 0.0 : rounded); // No "-0°C"
}

/// "1850 MHz", whole megahertz rounded half away from zero, or "N/A" for NaN (no reading): a GPU
/// clock, in its value strip, its tooltip and its series label alike (#1205).
[[nodiscard]] inline auto formatMegahertz(double megahertz) -> std::string
{
    if (std::isnan(megahertz))
    {
        return "N/A";
    }
    const double rounded = roundHalfAwayFromZero(megahertz, 0);
    return std::format("{:.0f} MHz", rounded == 0.0 ? 0.0 : rounded); // No "-0 MHz"
}

/// A link speed as NICs, switches and the OS describe it: a decimal bit rate, "100 Mbit/s",
/// "1 Gbit/s", "2.5 Gbit/s", "10 Gbit/s" (#1373). Whole megabits below 1 Gbit/s; from there,
/// gigabits to one decimal, dropped when it is zero. "-" for 0, the probes' "unknown". Link speeds
/// are reported in megabits per second (10^6 bits/s). Rates stay in bytes (formatBytesPerSec());
/// formatLinkSpeedAsByteRate() gives the link's byte-rate equivalent to compare with them.
[[nodiscard]] inline auto formatLinkSpeed(std::uint64_t megabitsPerSecond) -> std::string
{
    constexpr std::uint64_t MBIT_PER_GBIT = 1000;
    constexpr std::uint64_t MBIT_PER_TENTH_GBIT = MBIT_PER_GBIT / 10;
    if (megabitsPerSecond == 0)
    {
        return "-";
    }
    if (megabitsPerSecond < MBIT_PER_GBIT)
    {
        return formatFixedLocalized(static_cast<double>(megabitsPerSecond), 0, " Mbit/s");
    }
    // Tenths of a gigabit, rounded half up in integers, so 1999 Mbit/s reads "2 Gbit/s", not "2.0".
    const std::uint64_t tenths =
        (megabitsPerSecond / MBIT_PER_TENTH_GBIT) + ((megabitsPerSecond % MBIT_PER_TENTH_GBIT) >= MBIT_PER_TENTH_GBIT / 2 ? 1 : 0);
    const int decimals = (tenths % 10 == 0) ? 0 : 1;
    return formatFixedLocalized(static_cast<double>(tenths) / 10.0, decimals, " Gbit/s");
}

/// A link speed as the byte rate it carries at most, in the unit family of the Sent/Received rates
/// beside it: "119.2 MiB/s" for a 1 Gbit/s link (125,000,000 bytes/s). Shown on hover beside
/// formatLinkSpeed()'s bit rate, not instead of it (#1373).
[[nodiscard]] inline auto formatLinkSpeedAsByteRate(std::uint64_t megabitsPerSecond) -> std::string
{
    constexpr double BYTES_PER_SECOND_PER_MBPS = 1'000'000.0 / 8.0;
    return formatBytesPerSec(static_cast<double>(megabitsPerSecond) * BYTES_PER_SECOND_PER_MBPS);
}

// ============================================================================
// UI Numeric Utilities (for ImGui/ImPlot interop)
// ============================================================================

/// Clamp a percentage value to [0, 100]; NaN (no reading) becomes 0, which std::clamp alone
/// would pass through into bar geometry (#1148).
[[nodiscard]] constexpr auto clampPercent(double percent) noexcept -> double
{
    if (!(percent > 0.0))
    {
        return 0.0;
    }
    return std::min(percent, 100.0);
}

/// Clamp a percentage to [0, 100] and convert to [0, 1] range for ImGui
[[nodiscard]] constexpr auto percent01(double percent) noexcept -> double
{
    return clampPercent(percent) / 100.0;
}

/// Safe narrowing conversion for ImPlot series counts (size_t → int)
[[nodiscard]] constexpr auto checkedCount(std::size_t value) noexcept -> int
{
    return Domain::Numeric::narrowOr<int>(value, std::numeric_limits<int>::max());
}

/// Narrowing conversion to float for ImGui/ImPlot APIs that require float
[[nodiscard]] constexpr auto toFloatNarrow(double value) noexcept -> float
{
    return static_cast<float>(value);
}

/// Narrowing conversion to float for integral types
template<std::integral T> [[nodiscard]] constexpr auto toFloatNarrow(T value) noexcept -> float
{
    return static_cast<float>(value);
}

} // namespace UI::Format
