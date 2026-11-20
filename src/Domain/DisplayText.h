#pragma once

// Text from the OS shown on one line (#4328, the maintainer's 2026-11-20 resolution). The models keep the raw bytes
// (an environment value, a Wi-Fi network's name, a search domain); the UI makes display text from them only as it
// draws, so the bytes are never lost and a copy can still give them exactly (hex()).
//
// - Malformed UTF-8 becomes U+FFFD, one per maximal ill-formed subpart (Domain::sanitizedUtf8(), #4299's D1).
// - A control character is shown as "\n", "\t", "\r", or else "\xNN", so it can't break a row; a literal backslash is
//   shown as "\\", so text that reads "\n" or "\x41" can't be taken for an escape.
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
            out += "\\\\";
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
