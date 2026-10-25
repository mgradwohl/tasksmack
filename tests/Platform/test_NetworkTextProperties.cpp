/// @file test_NetworkTextProperties.cpp
/// @brief Property-based tests (#139, rapidcheck) for the network text the System Information page
/// builds on Linux: any IPv6 address's text round-trips through inet_pton() and is in RFC 5952 form;
/// any IPv4 address's text is its dotted quad; and the networkd description parser decodes any
/// \u-escaped string to the UTF-8 it encodes, and never fails on arbitrary input.

#include "Platform/Linux/LinuxNetworkAdapters.h"
#include "Platform/Linux/NetworkdLinkJson.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>
#include <sys/socket.h>

namespace Platform
{
namespace
{

/// 16 bytes, biased towards zero runs so the "::" compression is exercised often.
[[nodiscard]] rc::Gen<std::array<std::uint8_t, 16>> ipv6Bytes()
{
    return rc::gen::map(
        rc::gen::container<std::vector<std::uint8_t>>(
            16, rc::gen::weightedOneOf<std::uint8_t>({{3, rc::gen::just<std::uint8_t>(0)}, {2, rc::gen::arbitrary<std::uint8_t>()}})),
        [](const std::vector<std::uint8_t>& bytes)
        {
            std::array<std::uint8_t, 16> out{};
            std::copy_n(bytes.begin(), out.size(), out.begin());
            return out;
        });
}

// Any 16 bytes: the text parses back (inet_pton) to the same bytes.
RC_GTEST_PROP(NetworkTextPropertiesTest, Ipv6TextRoundTrips, ())
{
    const std::array<std::uint8_t, 16> bytes = *ipv6Bytes();
    const std::string text = LinuxNetworkAdapters::formatAddressBytes(10, bytes);
    RC_ASSERT(!text.empty());
    std::array<std::uint8_t, 16> parsed{};
    RC_ASSERT(::inet_pton(AF_INET6, text.c_str(), parsed.data()) == 1);
    RC_ASSERT(parsed == bytes);
}

// RFC 5952: lowercase, no leading zeros in a group, at most one "::", never for a single zero group.
// That is the form inet_ntop() writes, so the two agree.
RC_GTEST_PROP(NetworkTextPropertiesTest, Ipv6TextIsInRfc5952Form, ())
{
    const std::array<std::uint8_t, 16> bytes = *ipv6Bytes();
    const std::string text = LinuxNetworkAdapters::formatAddressBytes(10, bytes);
    std::array<char, INET6_ADDRSTRLEN> reference{};
    RC_ASSERT(::inet_ntop(AF_INET6, bytes.data(), reference.data(), reference.size()) != nullptr);
    const std::string expected(reference.data());
    // glibc writes an IPv4-mapped or -compatible address with a dotted tail; RFC 5952 allows either.
    RC_PRE(expected.find('.') == std::string::npos);
    RC_ASSERT(text == expected);
}

// Any 4 bytes: the dotted quad, each part its decimal value.
RC_GTEST_PROP(NetworkTextPropertiesTest, Ipv4TextIsTheDottedQuad, (std::uint8_t a, std::uint8_t b, std::uint8_t c, std::uint8_t d))
{
    const std::array<std::uint8_t, 4> bytes{a, b, c, d};
    RC_ASSERT(LinuxNetworkAdapters::formatAddressBytes(2, bytes) == std::format("{}.{}.{}.{}", a, b, c, d));
}

/// @p text as a JSON string literal with every character escaped as \uXXXX (surrogate pairs above
/// U+FFFF), the way a strict encoder may write it.
[[nodiscard]] std::string escapedJsonString(const std::u32string& text)
{
    std::string json = "\"";
    for (const char32_t c : text)
    {
        if (c >= 0x10000)
        {
            const char32_t v = c - 0x10000;
            json += std::format("\\u{:04x}\\u{:04x}", 0xD800 + (v >> 10U), 0xDC00 + (v & 0x3FFU));
        }
        else
        {
            json += std::format("\\u{:04x}", static_cast<std::uint32_t>(c));
        }
    }
    return json + "\"";
}

/// @p text as UTF-8.
[[nodiscard]] std::string utf8(const std::u32string& text)
{
    std::string out;
    for (const char32_t c : text)
    {
        if (c < 0x80)
        {
            out += static_cast<char>(c);
        }
        else if (c < 0x800)
        {
            out += static_cast<char>(0xC0 | (c >> 6U));
            out += static_cast<char>(0x80 | (c & 0x3FU));
        }
        else if (c < 0x10000)
        {
            out += static_cast<char>(0xE0 | (c >> 12U));
            out += static_cast<char>(0x80 | ((c >> 6U) & 0x3FU));
            out += static_cast<char>(0x80 | (c & 0x3FU));
        }
        else
        {
            out += static_cast<char>(0xF0 | (c >> 18U));
            out += static_cast<char>(0x80 | ((c >> 12U) & 0x3FU));
            out += static_cast<char>(0x80 | ((c >> 6U) & 0x3FU));
            out += static_cast<char>(0x80 | (c & 0x3FU));
        }
    }
    return out;
}

/// A code point that isn't a surrogate: from the BMP or the supplementary planes.
[[nodiscard]] rc::Gen<char32_t> scalarValues()
{
    return rc::gen::suchThat(rc::gen::inRange<char32_t>(1, 0x110000), [](char32_t c) { return c < 0xD800 || c > 0xDFFF; });
}

// Any string of Unicode scalar values, fully \u-escaped: networkd's parser decodes it to its UTF-8.
RC_GTEST_PROP(NetworkTextPropertiesTest, JsonEscapesDecodeToTheirUtf8, ())
{
    const std::u32string text = *rc::gen::container<std::u32string>(scalarValues());
    const std::optional<NetworkdLinkJson::Value> value = NetworkdLinkJson::parse(escapedJsonString(text));
    const bool decoded = value.has_value() && value->kind == NetworkdLinkJson::Value::Kind::String && value->string == utf8(text);
    RC_ASSERT(decoded);
}

// Arbitrary bytes: the parser and the lease extraction answer (nullopt or a value), never crash.
RC_GTEST_PROP(NetworkTextPropertiesTest, TheParserTakesAnyInput, (const std::string& text))
{
    static_cast<void>(NetworkdLinkJson::parse(text));
    static_cast<void>(NetworkdLinkJson::leaseFromDescription(text));
    RC_SUCCEED("answered");
}

} // namespace
} // namespace Platform
