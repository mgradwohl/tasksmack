#pragma once

// The ASCII case fold, for identifiers, keys and protocol words (#2566): a column id, a TOML key, a
// D-Bus or WMI name, an HTTP header, a file extension. There were about twenty, one in nearly every
// file: some folded A-Z by hand, others called std::tolower(), which follows the C locale main() sets
// from the user's environment. These fold ASCII letters only: every byte of 0x80 and up is left as it
// is, so a UTF-8 sequence never changes and "É" never matches "é".
//
// That is wrong for anything the user types or reads (#4299, D2): filters, search boxes, sorted
// columns. Those use Unicode simple case folding (Platform/Utf8CaseFold.h: equalsFoldedCase,
// containsFoldedCase, compareFoldedCase). An identity Windows compares ignoring case (a path, an
// account or service name) is ordinal: Platform::Windows::equalsOrdinalIgnoreCase (#4309).
//
// All constexpr and allocation-free. Platform is the lowest layer, so Domain, Core, UI and App code can
// all include it, as they already include Platform/AsciiTrim.h.

#include <compare>
#include <cstddef>
#include <string_view>

namespace Platform
{

/// @p ch with A-Z folded to a-z. Every other byte is unchanged, so a UTF-8 sequence never folds.
[[nodiscard]] constexpr char foldAscii(char ch) noexcept
{
    return (ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch - 'A' + 'a') : ch;
}

/// @p ch with a-z raised to A-Z. Every other byte is unchanged.
[[nodiscard]] constexpr char upperAscii(char ch) noexcept
{
    return (ch >= 'a' && ch <= 'z') ? static_cast<char>(ch - 'a' + 'A') : ch;
}

/// Whether @p a and @p b are the same text, ASCII letters compared case-insensitively.
[[nodiscard]] constexpr bool equalsIgnoreCase(std::string_view a, std::string_view b) noexcept
{
    if (a.size() != b.size())
    {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i)
    {
        if (foldAscii(a[i]) != foldAscii(b[i]))
        {
            return false;
        }
    }
    return true;
}

/// Whether @p text contains @p term, ASCII letters compared case-insensitively. An empty term is in
/// every text. Allocates nothing: candidate starts are found by the term's first character, so a long
/// command line costs about one pass (the Processes filter runs this on every row).
[[nodiscard]] constexpr bool containsIgnoreCase(std::string_view text, std::string_view term) noexcept
{
    if (term.empty())
    {
        return true;
    }
    if (text.size() < term.size())
    {
        return false;
    }
    const char first = foldAscii(term.front());
    const std::size_t lastStart = text.size() - term.size();
    for (std::size_t start = 0; start <= lastStart; ++start)
    {
        if (foldAscii(text[start]) != first)
        {
            continue;
        }
        std::size_t k = 1;
        while (k < term.size() && foldAscii(text[start + k]) == foldAscii(term[k]))
        {
            ++k;
        }
        if (k == term.size())
        {
            return true;
        }
    }
    return false;
}

/// Three-way comparison of @p a and @p b, ASCII letters compared case-insensitively and every other
/// byte as unsigned, the order std::string gives ("a" < "B" < "c" < "é"). A prefix orders before the
/// longer text, so "svc" < "svchost". For sorting: equal means equalsIgnoreCase().
[[nodiscard]] constexpr std::strong_ordering compareIgnoreCase(std::string_view a, std::string_view b) noexcept
{
    const std::size_t common = a.size() < b.size() ? a.size() : b.size();
    for (std::size_t i = 0; i < common; ++i)
    {
        const auto x = static_cast<unsigned char>(foldAscii(a[i]));
        const auto y = static_cast<unsigned char>(foldAscii(b[i]));
        if (x != y)
        {
            return x <=> y;
        }
    }
    return a.size() <=> b.size();
}

} // namespace Platform
