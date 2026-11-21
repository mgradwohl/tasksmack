#pragma once

// One sysfs or /proc attribute's text as a number (#4390): std::from_chars, so "-5000" is minus 5000 and not a
// wrapped 18446744073709546616 (std::stoull accepted the sign and wrapped it), "+1", " 1" and "1x" are no number,
// and nothing throws for control flow. The text is the attribute with its trailing newline (and spaces) already
// trimmed; anything else around the digits is refused. nullopt when it isn't a whole number of the type.

#include <charconv>
#include <concepts>
#include <optional>
#include <string_view>
#include <system_error>

namespace Platform::SysfsValue
{

/// @p text without trailing whitespace and newlines (the kernel ends each attribute with '\n').
[[nodiscard]] constexpr std::string_view trimmedEnd(std::string_view text) noexcept
{
    while (!text.empty() && (text.back() == '\n' || text.back() == ' ' || text.back() == '\t' || text.back() == '\r'))
    {
        text.remove_suffix(1);
    }
    return text;
}

/// @p text as a whole number of type @p T (signed or unsigned): digits only, a leading '-' for a signed type.
template<std::integral T> [[nodiscard]] constexpr std::optional<T> parse(std::string_view text) noexcept
{
    text = trimmedEnd(text);
    if (text.empty())
    {
        return std::nullopt;
    }
    T value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size())
    {
        return std::nullopt;
    }
    return value;
}

} // namespace Platform::SysfsValue
