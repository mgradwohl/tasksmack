#pragma once

// Text from the OS shown on one line (#4328, the maintainer's 2026-11-20 resolution). The models keep the raw bytes
// (an environment value, a Wi-Fi network's name, a search domain); the UI makes display text from them only as it
// draws, so the bytes are never lost and a copy can still give them exactly (hex()).
//
// - Malformed UTF-8 becomes U+FFFD, one per maximal ill-formed subpart (Domain::sanitizedUtf8(), #4299's D1).
// - A control character is shown as "\n", "\t", "\r", or else "\xNN", so it can't break a row.
// - A literal backslash is doubled ("\\") only where it would otherwise start an escape: when the next character shown
//   is 'n', 't', 'r', 'x' or '\' (which includes the next byte being a control character, whose escape starts with
//   '\'). Anywhere else it is shown as itself, so a Windows path reads "C:\Windows\system32" (#4838), not
//   "C:\\Windows\\system32" as when every backslash was doubled.
//
//   That decodes unambiguously, reading left to right: "\\" is one backslash; "\n", "\t", "\r" and "\xNN" are the
//   control characters; and a '\' followed by anything else, or by nothing, is a backslash on its own. A single
//   backslash is never written before 'n', 't', 'r', 'x' or '\', so text that reads "\n" or "\x41" can't be taken
//   for an escape, and the raw "\\" shows as "\\\" (three), never as one escaped backslash.
// - A Wi-Fi network's name follows nmcli: valid UTF-8 shows as text, anything else as the whole name in hex, so two
//   malformed names never look the same.
//
// Pure and header-only, so Platform and the UI can both use it.

#include "Domain/Utf8Sanitize.h"

#include <cstddef>
#include <string>
#include <string_view>

namespace Domain::DisplayText
{

namespace Detail
{

inline constexpr std::string_view HEX_DIGITS = "0123456789ABCDEF";

inline void appendHexByte(std::string& out, unsigned char byte)
{
    out += HEX_DIGITS[byte >> 4U];
    out += HEX_DIGITS[byte & 0x0FU];
}

/// Whether a backslash written just before @p raw[@p next] would read as the start of an escape: the next character
/// shown is 'n', 't', 'r', 'x' or '\', which a control byte's own escape starts with. A byte from 0x80 up shows as
/// itself in a multi-byte character or as U+FFFD, neither of which reads as an escape.
[[nodiscard]] constexpr bool backslashStartsEscape(std::string_view raw, std::size_t next) noexcept
{
    if (next >= raw.size())
    {
        return false; // a trailing backslash stands alone
    }
    const auto byte = static_cast<unsigned char>(raw[next]);
    return byte == 'n' || byte == 't' || byte == 'r' || byte == 'x' || byte == '\\' || byte < 0x20U || byte == 0x7FU;
}

} // namespace Detail

/// @p raw as one line of valid UTF-8: see the file comment.
[[nodiscard]] inline std::string escaped(std::string_view raw)
{
    std::string out;
    out.reserve(raw.size());
    std::size_t at = 0;
    while (at < raw.size())
    {
        std::size_t prefix = 0;
        const std::size_t length = Utf8Detail::sequenceAt(raw, at, prefix);
        if (length == 0)
        {
            out.append(UTF8_REPLACEMENT_CHARACTER);
            at += prefix;
            continue;
        }
        if (length > 1)
        {
            out.append(raw.substr(at, length));
            at += length;
            continue;
        }
        const auto byte = static_cast<unsigned char>(raw[at]);
        switch (byte)
        {
        case '\\':
            out += Detail::backslashStartsEscape(raw, at + 1) ? "\\\\" : "\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\t':
            out += "\\t";
            break;
        case '\r':
            out += "\\r";
            break;
        default:
            if (byte < 0x20U || byte == 0x7FU)
            {
                out += "\\x";
                Detail::appendHexByte(out, byte);
            }
            else
            {
                out += static_cast<char>(byte);
            }
            break;
        }
        ++at;
    }
    return out;
}

/// @p raw's bytes in upper-case hex, two digits each and nothing between: what "Copy raw (hex)" copies.
[[nodiscard]] inline std::string hex(std::string_view raw)
{
    std::string out;
    out.reserve(raw.size() * 2);
    for (const char c : raw)
    {
        Detail::appendHexByte(out, static_cast<unsigned char>(c));
    }
    return out;
}

/// A Wi-Fi network's name as nmcli shows it: escaped() when it is valid UTF-8, else hex() of the whole name.
[[nodiscard]] inline std::string ssid(std::string_view raw)
{
    return isValidUtf8(raw) ? escaped(raw) : hex(raw);
}

} // namespace Domain::DisplayText
