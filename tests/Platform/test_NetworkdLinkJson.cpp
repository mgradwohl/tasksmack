/// @file test_NetworkdLinkJson.cpp
/// @brief systemd-networkd's link description (#1518): the bounded JSON parser (values, escapes,
/// surrogate pairs, numbers, and the malformed or over-deep text it refuses), the DHCPv4 lease read from
/// a description as networkd writes one, and its CLOCK_BOOTTIME times moved to Unix time.

#include "Platform/ISystemInfoProbe.h"
#include "Platform/Linux/NetworkdLinkJson.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string>

namespace Platform
{
namespace
{

using NetworkdLinkJson::parse;
using NetworkdLinkJson::Value;

TEST(NetworkdLinkJsonTest, ParsesTheValueKinds)
{
    const std::optional<Value> value =
        parse(R"( {"a": 1, "b": [true, false, null], "c": "text", "d": {"e": 18446744073709551615}, "f": -2.5e3} )");
    ASSERT_TRUE(value.has_value());
    const Value& root = value.value_or(Value{});
    ASSERT_EQ(root.kind, Value::Kind::Object);
    ASSERT_NE(root.find("a"), nullptr);
    EXPECT_EQ(root.find("a")->unsignedInteger, std::optional<std::uint64_t>(1));
    ASSERT_NE(root.find("b"), nullptr);
    ASSERT_EQ(root.find("b")->items.size(), 3U);
    EXPECT_TRUE(root.find("b")->items[0].boolean);
    EXPECT_EQ(root.find("b")->items[2].kind, Value::Kind::Null);
    EXPECT_EQ(root.find("c")->string, "text");
    EXPECT_EQ(root.find("d")->find("e")->unsignedInteger, std::optional<std::uint64_t>(UINT64_MAX));
    // A negative or fractional number is a number, but not an unsigned integer.
    EXPECT_EQ(root.find("f")->kind, Value::Kind::Number);
    EXPECT_TRUE(root.find("f")->negative);
    EXPECT_FALSE(root.find("f")->unsignedInteger.has_value());
    EXPECT_EQ(root.find("missing"), nullptr);
}

TEST(NetworkdLinkJsonTest, DecodesStringEscapes)
{
    const std::optional<Value> value = parse(R"("a\"b\\c\/d\n\té😀")");
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(value.value_or(Value{}).string, "a\"b\\c/d\n\t\xC3\xA9\xF0\x9F\x98\x80"); // é and 😀 as UTF-8
}

// One \uXXXX escape per UTF-8 length (1, 2, 3 and 4 bytes, the last a surrogate pair), so every branch of the
// decoder runs in every run: NetworkTextProperties' random escapes only sometimes pick an ASCII one (#3165).
TEST(NetworkdLinkJsonTest, DecodesUnicodeEscapesOfEveryLength)
{
    const std::optional<Value> value = parse(R"("\u0041\u00e9\u20ac\ud83d\ude00")");
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(value.value_or(Value{}).string, "A\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80"); // A, é, € and 😀 as UTF-8
}

TEST(NetworkdLinkJsonTest, RefusesWhatIsNotOneJsonDocument)
{
    for (const char* text : {"",
                             "{",
                             "[1,",
                             "{\"a\" 1}",
                             "{\"a\":1,}",
                             "[1 2]",
                             "tru",
                             "01x",
                             "-",
                             "1.",
                             "1e",
                             "\"unterminated",
                             R"("bad \q escape")",
                             R"("\ud800 unpaired")",
                             "{} trailing",
                             "\"raw\ncontrol\""})
    {
        EXPECT_FALSE(parse(text).has_value()) << text;
    }
    // Nesting is bounded, so a hostile description can't exhaust the stack.
    std::string deep(NetworkdLinkJson::MAX_DEPTH + 2, '[');
    deep += std::string(NetworkdLinkJson::MAX_DEPTH + 2, ']');
    EXPECT_FALSE(parse(deep).has_value());
    std::string fine(NetworkdLinkJson::MAX_DEPTH, '[');
    fine += std::string(NetworkdLinkJson::MAX_DEPTH, ']');
    EXPECT_TRUE(parse(fine).has_value());
}

TEST(NetworkdLinkJsonTest, DecodesTheShortControlEscapes)
{
    const std::optional<Value> value = parse(R"("\b\f\r\t\n")");
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(value.value_or(Value{}).string, "\b\f\r\t\n");
}

TEST(NetworkdLinkJsonTest, RefusesBrokenEscapesAndSurrogates)
{
    for (const char* text : {R"("ends in a backslash\)",
                             R"("\u12")",         // too few hex digits
                             R"("\u12g4")",       // not hex
                             R"("\udc00")",       // a low surrogate alone
                             R"("\ud800A")",      // a high surrogate followed by a non-surrogate
                             R"("\ud800\udbff")", // a high surrogate followed by another high one
                             R"("\ud800\u12")"})  // the low half cut short
    {
        EXPECT_FALSE(parse(text).has_value()) << text;
    }
}

TEST(NetworkdLinkJsonTest, RefusesBrokenObjects)
{
    for (const char* text : {R"({"a": 1 "b": 2})", // no comma between members
                             R"({"a": [1,})",      // a member that doesn't parse
                             R"({1: 2})",          // a key that isn't a string
                             R"({"a" "b"})"})      // no colon
    {
        EXPECT_FALSE(parse(text).has_value()) << text;
    }
}

TEST(NetworkdLinkJsonTest, ReadsExponentsWithASign)
{
    for (const char* text : {"1e+3", "1E-3", "2.5e10"})
    {
        const std::optional<Value> value = parse(text);
        ASSERT_TRUE(value.has_value()) << text;
        EXPECT_EQ(value.value_or(Value{}).kind, Value::Kind::Number) << text;
        // Not an integer, so no unsigned value even where it would be whole.
        EXPECT_FALSE(value.value_or(Value{}).unsignedInteger.has_value()) << text;
    }
    EXPECT_FALSE(parse("1e+").has_value());
    // Too big for 64 bits: still a number, just not an unsigned integer.
    const std::optional<Value> huge = parse("18446744073709551616");
    ASSERT_TRUE(huge.has_value());
    EXPECT_FALSE(huge.value_or(Value{}).unsignedInteger.has_value());
}

TEST(NetworkdLinkJsonTest, FindAnswersNothingOutsideAnObject)
{
    const std::optional<Value> array = parse(R"([{"a": 1}])");
    ASSERT_TRUE(array.has_value());
    EXPECT_EQ(array.value_or(Value{}).find("a"), nullptr);
    EXPECT_EQ(Value{}.find("a"), nullptr);
}

TEST(NetworkdLinkJsonTest, RefusesTextOverTheSizeCap)
{
    std::string text = "\"";
    text.append(NetworkdLinkJson::MAX_TEXT_BYTES, 'a');
    text += '"';
    EXPECT_FALSE(parse(text).has_value());
}

TEST(NetworkdLinkJsonTest, Ipv4NeedsFourByteValues)
{
    const auto ipv4 = [](const char* json)
    {
        return NetworkdLinkJson::ipv4FromBytes(parse(json).value_or(Value{}));
    };
    EXPECT_EQ(ipv4("[10,0,0,1]"), "10.0.0.1");
    EXPECT_EQ(ipv4("[10,0,0,256]"), "");
    EXPECT_EQ(ipv4(R"([10,0,0,"1"])"), "");
    EXPECT_EQ(ipv4("[10,0,0,-1]"), "");
    EXPECT_EQ(ipv4("[10,0,0,1.5]"), "");
    EXPECT_EQ(ipv4(R"({"a": 1})"), "");
}

/// A link description as systemd-networkd 259 writes one (trimmed): a DHCPv4 address from 192.168.1.1,
/// a link-local IPv6 address, and the DHCPv4 client's lease.
constexpr const char* DESCRIPTION = R"({
  "Index": 2, "Name": "enp3s0", "Type": "ether", "OperationalState": "routable",
  "Addresses": [
    {"Family": 10, "Address": [254,128,0,0,0,0,0,0,26,42,59,255,254,77,94,111], "PrefixLength": 64,
     "ConfigSource": "foreign", "ConfigState": "configured"},
    {"Family": 2, "Address": [192,168,1,20], "PrefixLength": 24, "ConfigSource": "DHCPv4",
     "ConfigProvider": [192,168,1,1], "PreferredLifetimeUSec": 86412000000, "ValidLifetimeUSec": 86412000000,
     "ConfigState": "configured"}
  ],
  "DHCPv4Client": {"Lease": {"LeaseTimestampUSec": 12000000, "Timeout1USec": 43212000000,
                             "Timeout2USec": 75612000000, "Hostname": "box"}}
})";

TEST(NetworkdLinkJsonTest, ReadsTheDhcpLeaseFromADescription)
{
    const std::optional<NetworkdLinkJson::Lease> lease = NetworkdLinkJson::leaseFromDescription(DESCRIPTION);
    ASSERT_TRUE(lease.has_value());
    const NetworkdLinkJson::Lease got = lease.value_or(NetworkdLinkJson::Lease{});
    EXPECT_EQ(got.server, "192.168.1.1");
    EXPECT_EQ(got.obtainedBootUsec, std::optional<std::uint64_t>(12'000'000));
    EXPECT_EQ(got.expiresBootUsec, std::optional<std::uint64_t>(86'412'000'000));
}

TEST(NetworkdLinkJsonTest, NoLeaseWhereTheLinkHasNoDhcpv4)
{
    // Static and link-local addresses only, and no DHCPv4 client.
    EXPECT_FALSE(
        NetworkdLinkJson::leaseFromDescription(R"({"Addresses": [{"Family": 2, "Address": [10,0,0,5], "ConfigSource": "static"}]})")
            .has_value());
    EXPECT_FALSE(NetworkdLinkJson::leaseFromDescription("{}").has_value());
    EXPECT_FALSE(NetworkdLinkJson::leaseFromDescription("not json").has_value());
    EXPECT_FALSE(NetworkdLinkJson::leaseFromDescription("[]").has_value());
    // A provider that isn't four bytes is no server, but the lease still counts.
    const std::optional<NetworkdLinkJson::Lease> odd = NetworkdLinkJson::leaseFromDescription(
        R"({"Addresses": [{"Family": 2, "ConfigSource": "DHCPv4", "ConfigProvider": [1,2,3], "ValidLifetimeUSec": 5}]})");
    ASSERT_TRUE(odd.has_value());
    EXPECT_TRUE(odd.value_or(NetworkdLinkJson::Lease{}).server.empty());
}

TEST(NetworkdLinkJsonTest, BootTimesBecomeUnixTimesAndALength)
{
    constexpr std::uint64_t NOW_UNIX = 1'760'000'000;
    constexpr std::uint64_t NOW_BOOT_USEC = 3'600'000'000; // an hour since boot
    // Obtained 10 minutes after boot, valid for a day: it runs out a day minus 50 minutes from now.
    const NetworkdLinkJson::Lease lease{
        .server = "192.168.1.1", .obtainedBootUsec = 600'000'000, .expiresBootUsec = 600'000'000 + 86'400'000'000};
    const std::optional<DhcpLease> shown = NetworkdLinkJson::toDhcpLease(lease, NOW_UNIX, NOW_BOOT_USEC);
    ASSERT_TRUE(shown.has_value());
    const DhcpLease got = shown.value_or(DhcpLease{});
    EXPECT_EQ(got.server, "192.168.1.1");
    EXPECT_EQ(got.leaseSeconds, 86'400U);
    EXPECT_EQ(got.expiresUnix, NOW_UNIX + 86'400 - 3'000);

    // Already run out: in the past, not wrapped around.
    const NetworkdLinkJson::Lease expired{.server = {}, .obtainedBootUsec = std::nullopt, .expiresBootUsec = 1'000'000'000};
    EXPECT_EQ(NetworkdLinkJson::toDhcpLease(expired, NOW_UNIX, NOW_BOOT_USEC).value_or(DhcpLease{}).expiresUnix, NOW_UNIX - 2'600);

    // Nothing to show.
    EXPECT_FALSE(NetworkdLinkJson::toDhcpLease(NetworkdLinkJson::Lease{}, NOW_UNIX, NOW_BOOT_USEC).has_value());
}

} // namespace
} // namespace Platform
