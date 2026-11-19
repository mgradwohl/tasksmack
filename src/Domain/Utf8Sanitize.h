#pragma once

// The one place display text is made valid UTF-8 (#4301, the UTF-8 audit #4299's D1). Text from the OS
// (a 15-byte Linux comm cut mid-character, a Latin-1 file name in a command line, a Windows name with a lone
// surrogate after conversion) stays as its raw bytes for every action and lookup; only the copy that is shown
// or copied is passed through here. Each maximal ill-formed subpart becomes one U+FFFD, as Unicode 3.9's
// "U+FFFD Substitution of Maximal Subparts" says, so a cut character costs one replacement and the bytes after
// it are kept, unlike ImGui's decoder, which swallows them ("Caf\xE9.txt" drew as "Caf<?>xt").
//
// Pure and header-only, so Platform (which may include Domain's pure headers) can use it too.

#include <cstddef>
#include <string>
#include <string_view>

namespace Domain
{

/// U+FFFD REPLACEMENT CHARACTER in UTF-8.
inline constexpr std::string_view UTF8_REPLACEMENT_CHARACTER = "\xEF\xBF\xBD";

namespace Utf8Detail
{

[[nodiscard]] constexpr bool isContinuation(unsigned char byte) noexcept
{
    return (byte & 0xC0U) == 0x80U;
}

/// The well-formed sequence starting at @p text[at]: its length, or 0 with @p prefix set to the length of its
/// maximal ill-formed subpart (at least 1). Table 3-7 of the Unicode Standard: no overlong forms, no surrogates
/// (U+D800..U+DFFF), nothing past U+10FFFF.
[[nodiscard]] constexpr std::size_t sequenceAt(std::string_view text, std::size_t at, std::size_t& prefix) noexcept
{
    const auto lead = static_cast<unsigned char>(text[at]);
    if (lead < 0x80U)
    {
        return 1;
    }
    std::size_t length = 0;
    unsigned char secondLow = 0x80U;
    unsigned char secondHigh = 0xBFU;
    if (lead >= 0xC2U && lead <= 0xDFU)
    {
        length = 2;
    }
    else if (lead >= 0xE0U && lead <= 0xEFU)
    {
        length = 3;
        secondLow = lead == 0xE0U ? 0xA0U : 0x80U;  // E0 80..9F would be overlong
        secondHigh = lead == 0xEDU ? 0x9FU : 0xBFU; // ED A0..BF would be a surrogate
    }
    else if (lead >= 0xF0U && lead <= 0xF4U)
    {
        length = 4;
        secondLow = lead == 0xF0U ? 0x90U : 0x80U;  // F0 80..8F would be overlong
        secondHigh = lead == 0xF4U ? 0x8FU : 0xBFU; // F4 90.. would be past U+10FFFF
    }
    else
    {
        prefix = 1; // a continuation byte, C0, C1 or F5..FF: never a lead
        return 0;
    }
    for (std::size_t i = 1; i < length; ++i)
    {
        if (at + i >= text.size())
        {
            prefix = i;
            return 0;
        }
        const auto byte = static_cast<unsigned char>(text[at + i]);
        const bool ok = i == 1 ? (byte >= secondLow && byte <= secondHigh) : isContinuation(byte);
        if (!ok)
        {
            prefix = i;
            return 0;
        }
    }
    return length;
}

} // namespace Utf8Detail

/// True when @p text is well-formed UTF-8. ASCII-only text, the usual case, is one read-only pass with no
/// early exit, which the compiler vectorizes; only text with a byte >= 0x80 is decoded.
[[nodiscard]] constexpr bool isValidUtf8(std::string_view text) noexcept
{
    unsigned high = 0;
    for (const char c : text)
    {
        high |= static_cast<unsigned char>(c);
    }
    if ((high & 0x80U) == 0U)
    {
        return true;
    }
    std::size_t at = 0;
    while (at < text.size())
    {
        std::size_t prefix = 0;
        const std::size_t length = Utf8Detail::sequenceAt(text, at, prefix);
        if (length == 0)
        {
            return false;
        }
        at += length;
    }
    return true;
}

/// Appends @p text to @p out with each maximal ill-formed subpart replaced by U+FFFD. Well-formed text is
/// appended unchanged. Returns whether anything was replaced.
inline bool appendSanitizedUtf8(std::string& out, std::string_view text)
{
    if (isValidUtf8(text))
    {
        out.append(text);
        return false;
    }
    std::size_t at = 0;
    while (at < text.size())
    {
        std::size_t prefix = 0;
        const std::size_t length = Utf8Detail::sequenceAt(text, at, prefix);
        if (length != 0)
        {
            out.append(text.substr(at, length));
            at += length;
        }
        else
        {
            out.append(UTF8_REPLACEMENT_CHARACTER);
            at += prefix;
        }
    }
    return true;
}

/// @p text as well-formed UTF-8, for display or copying: see appendSanitizedUtf8(). Never for an action or a
/// lookup, which keep the raw bytes.
[[nodiscard]] inline std::string sanitizedUtf8(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    appendSanitizedUtf8(out, text);
    return out;
}

} // namespace Domain
