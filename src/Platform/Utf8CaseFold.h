#pragma once

// Case-insensitive matching for user-visible text (#4302, the UTF-8 audit's D2): Unicode simple case folding,
// so "émile" finds "Émile.exe", "élodie" matches the account "PC\Élodie" and "яндекс" matches "Яндекс". The
// ASCII helpers (AsciiCase.h) stay for ASCII keywords and protocol words only. Simple folding maps one code
// point to one (CaseFolding.txt statuses C and S), so a match never changes length in code points and the scan
// allocates nothing. Text is decoded on the fly; an ill-formed byte decodes to a value no character has, so it
// never matches one, and nothing past the input is read. ASCII-only text, the usual case, takes AsciiCase.h's
// fast path. Platform is the lowest layer, so every layer can include it.

#include "Platform/AsciiCase.h"
#include "Platform/UnicodeCaseFoldTable.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace Platform
{

/// @p cp's simple case fold (Unicode simple case folding: no full or Turkish mappings). ASCII folds A-Z.
[[nodiscard]] constexpr char32_t simpleCaseFold(char32_t cp) noexcept
{
    if (cp < 0x80U)
    {
        return static_cast<char32_t>(static_cast<unsigned char>(foldAscii(static_cast<char>(cp))));
    }
    // A plain binary search over indices: an iterator variable can't satisfy both toolchains' clang-tidy (libc++'s
    // std::array iterator is a pointer, so readability-qualified-auto wants auto*; MSVC's is a class, which auto*
    // can't hold, and a named type trips modernize-use-auto).
    const auto& table = UnicodeCaseFold::SIMPLE_FOLDS;
    const auto key = static_cast<std::uint32_t>(cp);
    std::size_t low = 0;
    std::size_t high = table.size();
    while (low < high)
    {
        const std::size_t middle = low + ((high - low) / 2);
        if (table[middle].first < key)
        {
            low = middle + 1;
        }
        else
        {
            high = middle;
        }
    }
    return (low < table.size() && table[low].first == key) ? static_cast<char32_t>(table[low].second) : cp;
}

namespace Utf8CaseFoldDetail
{

/// The code point at @p text[at] and its length in bytes. An ill-formed byte (a lead with no valid
/// continuation, a stray continuation, an overlong or surrogate form) is one byte long and decodes to
/// 0x110000 + that byte: no character's value, so it matches only the same ill-formed byte.
[[nodiscard]] constexpr char32_t decodeAt(std::string_view text, std::size_t at, std::size_t& length) noexcept
{
    const auto lead = static_cast<unsigned char>(text[at]);
    const char32_t invalid = 0x110000U + lead;
    length = 1;
    if (lead < 0x80U)
    {
        return lead;
    }
    std::size_t need = 0;
    char32_t cp = 0;
    char32_t minimum = 0;
    if (lead >= 0xC2U && lead <= 0xDFU)
    {
        need = 1;
        cp = lead & 0x1FU;
        minimum = 0x80U;
    }
    else if (lead >= 0xE0U && lead <= 0xEFU)
    {
        need = 2;
        cp = lead & 0x0FU;
        minimum = 0x800U;
    }
    else if (lead >= 0xF0U && lead <= 0xF4U)
    {
        need = 3;
        cp = lead & 0x07U;
        minimum = 0x10000U;
    }
    else
    {
        return invalid;
    }
    if (at + need >= text.size()) // the continuation bytes would run past the end
    {
        return invalid;
    }
    for (std::size_t i = 1; i <= need; ++i)
    {
        const auto byte = static_cast<unsigned char>(text[at + i]);
        if ((byte & 0xC0U) != 0x80U)
        {
            return invalid;
        }
        cp = (cp << 6U) | (byte & 0x3FU);
    }
    if (cp < minimum || cp > 0x10FFFFU || (cp >= 0xD800U && cp <= 0xDFFFU))
    {
        return invalid;
    }
    length = need + 1;
    return cp;
}

[[nodiscard]] constexpr bool isAscii(std::string_view text) noexcept
{
    unsigned high = 0;
    for (const char c : text)
    {
        high |= static_cast<unsigned char>(c);
    }
    return (high & 0x80U) == 0U;
}

/// Whether @p text, from byte @p at, starts with @p term, both folded; on a match @p end is the byte after it.
[[nodiscard]] constexpr bool foldedPrefixAt(std::string_view text, std::size_t at, std::string_view term, std::size_t& end) noexcept
{
    std::size_t t = 0;
    while (t < term.size())
    {
        if (at >= text.size())
        {
            return false;
        }
        std::size_t textLength = 0;
        std::size_t termLength = 0;
        const char32_t a = simpleCaseFold(decodeAt(text, at, textLength));
        const char32_t b = simpleCaseFold(decodeAt(term, t, termLength));
        if (a != b)
        {
            return false;
        }
        at += textLength;
        t += termLength;
    }
    end = at;
    return true;
}

} // namespace Utf8CaseFoldDetail

/// Whether @p text contains @p term, compared by Unicode simple case folding. An empty term is in every text.
/// Allocates nothing. ASCII text and term take containsIgnoreCase()'s fast path (an ASCII term can still match
/// non-ASCII text: "k" is in "K", U+212A KELVIN SIGN, whose fold is "k").
[[nodiscard]] constexpr bool containsFoldedCase(std::string_view text, std::string_view term) noexcept
{
    if (term.empty())
    {
        return true;
    }
    if (Utf8CaseFoldDetail::isAscii(text) && Utf8CaseFoldDetail::isAscii(term))
    {
        return containsIgnoreCase(text, term);
    }
    std::size_t at = 0;
    while (at < text.size())
    {
        std::size_t end = 0;
        if (Utf8CaseFoldDetail::foldedPrefixAt(text, at, term, end))
        {
            return true;
        }
        std::size_t length = 0;
        static_cast<void>(Utf8CaseFoldDetail::decodeAt(text, at, length));
        at += length;
    }
    return false;
}

/// Whether @p a and @p b are the same text by Unicode simple case folding ("Élodie" and "élodie").
[[nodiscard]] constexpr bool equalsFoldedCase(std::string_view a, std::string_view b) noexcept
{
    if (Utf8CaseFoldDetail::isAscii(a) && Utf8CaseFoldDetail::isAscii(b))
    {
        return equalsIgnoreCase(a, b);
    }
    std::size_t i = 0;
    std::size_t j = 0;
    while (i < a.size() && j < b.size())
    {
        std::size_t aLength = 0;
        std::size_t bLength = 0;
        if (simpleCaseFold(Utf8CaseFoldDetail::decodeAt(a, i, aLength)) != simpleCaseFold(Utf8CaseFoldDetail::decodeAt(b, j, bLength)))
        {
            return false;
        }
        i += aLength;
        j += bLength;
    }
    return i == a.size() && j == b.size();
}

/// Whether @p text starts with @p prefix by Unicode simple case folding.
[[nodiscard]] constexpr bool startsWithFoldedCase(std::string_view text, std::string_view prefix) noexcept
{
    std::size_t end = 0;
    return Utf8CaseFoldDetail::foldedPrefixAt(text, 0, prefix, end);
}

/// Whether @p text ends with @p suffix by Unicode simple case folding. Byte lengths can differ (U+212A is three
/// bytes, its fold "k" one), so each code point boundary is tried as the start.
[[nodiscard]] constexpr bool endsWithFoldedCase(std::string_view text, std::string_view suffix) noexcept
{
    if (suffix.empty())
    {
        return true;
    }
    std::size_t at = 0;
    while (at < text.size())
    {
        std::size_t end = 0;
        if (Utf8CaseFoldDetail::foldedPrefixAt(text, at, suffix, end) && end == text.size())
        {
            return true;
        }
        std::size_t length = 0;
        static_cast<void>(Utf8CaseFoldDetail::decodeAt(text, at, length));
        at += length;
    }
    return false;
}

} // namespace Platform
