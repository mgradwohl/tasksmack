#pragma once

// Case-insensitive matching for user-visible text (#4302, the UTF-8 audit's D2): Unicode simple case folding,
// so "émile" finds "Émile.exe", "élodie" matches the account "PC\Élodie" and "яндекс" matches "Яндекс". The
// ASCII helpers (AsciiCase.h) stay for ASCII keywords and protocol words only. Simple folding maps one code
// point to one (CaseFolding.txt statuses C and S), so a match never changes length in code points and the scan
// allocates nothing. Text is decoded on the fly; an ill-formed byte decodes to a value no character has, so it
// never matches one, and nothing past the input is read. ASCII-only text, the usual case, takes AsciiCase.h's
// fast path. Platform is the lowest layer, so every layer can include it.

#include "Domain/Utf8Sanitize.h"
#include "Platform/AsciiCase.h"
#include "Platform/UnicodeCaseFoldTable.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <string>
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
    // Eight bytes at a time (#4567): the filter calls this on every row's name and command line, and a byte at a
    // time made process filtering 27-38% slower than the ASCII-only scan it replaced (#4492). The sanitizer's scan.
    return Domain::Utf8Detail::isAscii(text);
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

/// Appends @p text's Unicode simple case fold to @p out, as UTF-8: a key that two spellings of one name share
/// ("PC\\Élodie" and "pc\\élodie"). An ill-formed byte is copied as it is, so it stays distinct from any character.
inline void appendFolded(std::string& out, std::string_view text)
{
    std::size_t at = 0;
    while (at < text.size())
    {
        std::size_t length = 0;
        const char32_t decoded = Utf8CaseFoldDetail::decodeAt(text, at, length);
        if (decoded > 0x10FFFFU) // ill-formed: keep the byte
        {
            out += text[at];
            at += length;
            continue;
        }
        const char32_t cp = simpleCaseFold(decoded);
        if (cp < 0x80U)
        {
            out += static_cast<char>(cp);
        }
        else if (cp < 0x800U)
        {
            out += static_cast<char>(0xC0U | (cp >> 6U));
            out += static_cast<char>(0x80U | (cp & 0x3FU));
        }
        else if (cp < 0x10000U)
        {
            out += static_cast<char>(0xE0U | (cp >> 12U));
            out += static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU));
            out += static_cast<char>(0x80U | (cp & 0x3FU));
        }
        else
        {
            out += static_cast<char>(0xF0U | (cp >> 18U));
            out += static_cast<char>(0x80U | ((cp >> 12U) & 0x3FU));
            out += static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU));
            out += static_cast<char>(0x80U | (cp & 0x3FU));
        }
        at += length;
    }
}

/// The order of @p a and @p b by Unicode simple case folding alone: case variants ("PC\\alice", "pc\\ALICE") compare
/// equal, as compareIgnoreCase()'s do (a strong_ordering for the same reason: callers order by it and then by their
/// own tie-breakers). For keys that identify one thing however it is spelled, and for sorts that break ties by
/// another key; a sort with no tie-breaker of its own wants compareFoldedCase().
[[nodiscard]] constexpr std::strong_ordering compareFolded(std::string_view a, std::string_view b) noexcept
{
    if (Utf8CaseFoldDetail::isAscii(a) && Utf8CaseFoldDetail::isAscii(b))
    {
        return compareIgnoreCase(a, b);
    }
    std::size_t i = 0;
    std::size_t j = 0;
    while (i < a.size() && j < b.size())
    {
        std::size_t aLength = 0;
        std::size_t bLength = 0;
        const char32_t x = simpleCaseFold(Utf8CaseFoldDetail::decodeAt(a, i, aLength));
        const char32_t y = simpleCaseFold(Utf8CaseFoldDetail::decodeAt(b, j, bLength));
        if (x != y)
        {
            return x <=> y;
        }
        i += aLength;
        j += bLength;
    }
    return (a.size() - i) <=> (b.size() - j); // one side is used up: the one with text left is after
}

/// The order of @p a and @p b for a user-visible sort: by Unicode simple case folding ("Émile" beside "émile"; folding
/// joins cases, it strips no accents, so both still sort after "Zoe" by code point, #4509), then by their bytes, so case
/// variants still sort in one stable order (the maintainer, #4324: the same fold as matching, with a byte-wise
/// tie-break; not locale collation).
[[nodiscard]] constexpr std::strong_ordering compareFoldedCase(std::string_view a, std::string_view b) noexcept
{
    const std::strong_ordering folded = compareFolded(a, b);
    if (folded < 0)
    {
        return std::strong_ordering::less;
    }
    if (folded > 0)
    {
        return std::strong_ordering::greater;
    }
    return a.compare(b) <=> 0;
}

} // namespace Platform
