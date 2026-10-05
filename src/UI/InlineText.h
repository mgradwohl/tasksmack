#pragma once

// Short text held in place rather than on the heap, for strings rebuilt every frame (#1171). Pure and
// ImGui-free, so it is unit-testable on its own.

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <string_view>
#include <utility>

namespace UI
{

/// The longest prefix of @p text, at most @p maxBytes long, that does not end part-way through a
/// UTF-8 sequence: text cut at a byte limit must not end in half a character.
[[nodiscard]] constexpr std::size_t utf8PrefixLength(std::string_view text, std::size_t maxBytes) noexcept
{
    if (text.size() <= maxBytes)
    {
        return text.size();
    }
    std::size_t length = maxBytes;
    // Back up over continuation bytes (10xxxxxx) to the lead byte of the character the cut splits,
    // and cut before it.
    while (length > 0 && (static_cast<unsigned char>(text[length]) & 0xC0U) == 0x80U)
    {
        --length;
    }
    return length;
}

/// Up to CAPACITY bytes of text, stored inline: building one never allocates. A NowBar's tooltip is
/// rebuilt every frame, since the value strip shows it, and as a std::string any tooltip longer than
/// the small-string buffer cost a heap allocation per bar per frame (#1171). Text longer than
/// CAPACITY is cut there, on a UTF-8 character boundary.
///
/// Implicitly constructible from a string literal, so `.tooltipText = "Fan: unavailable"` reads as it
/// did; from anything else only explicitly, or through format(), so code cannot build a std::string
/// and copy it in without saying so.
class InlineText
{
  public:
    static constexpr std::size_t CAPACITY = 127;

    constexpr InlineText() noexcept = default;

    // NOLINTNEXTLINE(google-explicit-constructor,hicpp-explicit-conversions) - stands in for the string literals it replaced
    constexpr InlineText(const char* text) noexcept : InlineText(std::string_view(text))
    {}

    constexpr explicit InlineText(std::string_view text) noexcept : m_Size(utf8PrefixLength(text, CAPACITY))
    {
        std::copy(text.begin(), text.begin() + static_cast<std::ptrdiff_t>(m_Size), m_Data.begin());
    }

    /// std::format(), into the inline buffer: no allocation, the result cut at CAPACITY if it is longer.
    template<typename... Args> [[nodiscard]] static InlineText format(std::format_string<Args...> fmt, Args&&... args)
    {
        // A few bytes of slack past CAPACITY, so a cut that lands inside a multi-byte character can be
        // seen (and backed off) rather than silently kept.
        std::array<char, CAPACITY + 4> scratch{};
        const auto result = std::format_to_n(scratch.data(), static_cast<std::ptrdiff_t>(scratch.size()), fmt, std::forward<Args>(args)...);
        const auto written = static_cast<std::size_t>(std::min<std::ptrdiff_t>(result.size, static_cast<std::ptrdiff_t>(scratch.size())));
        return InlineText(std::string_view(scratch.data(), written));
    }

    [[nodiscard]] constexpr std::string_view view() const noexcept
    {
        return {m_Data.data(), m_Size};
    }

    // NOLINTNEXTLINE(google-explicit-constructor,hicpp-explicit-conversions) - read wherever a std::string_view is
    constexpr operator std::string_view() const noexcept
    {
        return view();
    }

    [[nodiscard]] constexpr bool empty() const noexcept
    {
        return m_Size == 0;
    }

    [[nodiscard]] constexpr std::size_t size() const noexcept
    {
        return m_Size;
    }

    [[nodiscard]] friend constexpr bool operator==(const InlineText& lhs, std::string_view rhs) noexcept
    {
        return lhs.view() == rhs;
    }

  private:
    std::array<char, CAPACITY> m_Data{};
    std::size_t m_Size = 0;
};

} // namespace UI
