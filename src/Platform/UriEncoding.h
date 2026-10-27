#pragma once

// Percent-encoding for the URIs TaskSmack hands to other programs (#3064): the file:// URI the file
// manager is asked to show, and the web search for a process name. Platform is the lowest layer, so App
// code can include it, as it already includes Platform/AsciiTrim.h.

#include <array>
#include <string>
#include <string_view>

namespace Platform
{

/// Whether @p c is in RFC 3986's unreserved set: ALPHA, DIGIT, '-', '.', '_', '~'.
[[nodiscard]] constexpr bool isUriUnreserved(char c) noexcept
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~';
}

/// @p text with every byte outside the unreserved set and @p keep written as %XX (upper-case hex), so
/// spaces, commas, quotes, '%' and non-ASCII bytes (each byte of their UTF-8, or of a non-UTF-8 path)
/// survive as they are.
[[nodiscard]] inline std::string percentEncode(std::string_view text, std::string_view keep = {})
{
    static constexpr std::array<char, 16> HEX{'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};
    std::string encoded;
    encoded.reserve(text.size());
    for (const char c : text)
    {
        if (isUriUnreserved(c) || keep.contains(c))
        {
            encoded.push_back(c);
            continue;
        }
        const auto byte = static_cast<unsigned char>(c);
        encoded.push_back('%');
        encoded.push_back(HEX.at(byte >> 4U));
        encoded.push_back(HEX.at(byte & 0x0FU));
    }
    return encoded;
}

} // namespace Platform
