// nl80211's station rates (#2591): the requests built, the replies parsed (bytes laid out as the kernel
// sends them), and the standard each kind of rate names.

#include "Platform/Linux/Nl80211.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <vector>

// NOLINTBEGIN(misc-include-cleaner) - kernel netlink headers: include-cleaner lacks mappings for their macros
#include <linux/genetlink.h>
#include <linux/netlink.h>
#include <linux/nl80211.h>

namespace Platform
{
namespace
{

using Bytes = std::vector<std::byte>;

template<typename T> void put(Bytes& out, T value)
{
    const std::size_t at = out.size();
    out.resize(at + sizeof(T));
    std::memcpy(out.data() + at, &value, sizeof(T));
}

/// One attribute of @p type holding @p payload, padded to 4 bytes.
[[nodiscard]] Bytes attribute(std::uint16_t type, const Bytes& payload)
{
    Bytes out;
    put<std::uint16_t>(out, static_cast<std::uint16_t>(NLA_HDRLEN + payload.size()));
    put<std::uint16_t>(out, type);
    out.insert(out.end(), payload.begin(), payload.end());
    out.resize((out.size() + 3U) & ~std::size_t{3});
    return out;
}

template<typename T> [[nodiscard]] Bytes value(T v)
{
    Bytes out;
    put(out, v);
    return out;
}

[[nodiscard]] Bytes concat(std::initializer_list<Bytes> parts)
{
    Bytes out;
    for (const Bytes& part : parts)
    {
        out.insert(out.end(), part.begin(), part.end());
    }
    return out;
}

/// A generic netlink message of @p type and @p command around @p attributes.
[[nodiscard]] Bytes message(std::uint16_t type, std::uint8_t command, const Bytes& attributes)
{
    Bytes out;
    put<std::uint32_t>(out, static_cast<std::uint32_t>(NLMSG_HDRLEN + GENL_HDRLEN + attributes.size()));
    put<std::uint16_t>(out, type);
    put<std::uint16_t>(out, NLM_F_MULTI);
    put<std::uint32_t>(out, 2);
    put<std::uint32_t>(out, 0);
    out.push_back(static_cast<std::byte>(command));
    out.push_back(std::byte{1});
    put<std::uint16_t>(out, 0);
    out.insert(out.end(), attributes.begin(), attributes.end());
    return out;
}

constexpr std::uint16_t FAMILY = 0x1C;

TEST(Nl80211Test, TheFamilyRequestAndReply)
{
    const Bytes request = Nl80211::familyRequest(7);
    ASSERT_GE(request.size(), std::size_t{NLMSG_HDRLEN + GENL_HDRLEN});
    std::uint32_t length = 0;
    std::memcpy(&length, request.data(), 4);
    EXPECT_EQ(length, request.size());
    EXPECT_EQ(static_cast<std::uint8_t>(request[NLMSG_HDRLEN]), CTRL_CMD_GETFAMILY);

    const Bytes reply = message(GENL_ID_CTRL,
                                CTRL_CMD_NEWFAMILY,
                                concat({attribute(CTRL_ATTR_FAMILY_NAME, Bytes{std::byte{'n'}, std::byte{0}}),
                                        attribute(CTRL_ATTR_FAMILY_ID, value<std::uint16_t>(FAMILY))}));
    EXPECT_EQ(Nl80211::parseFamilyId(reply), std::optional<std::uint16_t>(FAMILY));
    EXPECT_FALSE(Nl80211::parseFamilyId(message(GENL_ID_CTRL, CTRL_CMD_GETFAMILY, {})).has_value()) << "not a reply";
}

TEST(Nl80211Test, AStationsRatesAndTheirKind)
{
    const Bytes tx = concat({attribute(NL80211_RATE_INFO_BITRATE32, value<std::uint32_t>(8667)),
                             attribute(NL80211_RATE_INFO_HE_MCS, value<std::uint8_t>(11)),
                             attribute(NL80211_RATE_INFO_HE_NSS, value<std::uint8_t>(2))});
    const Bytes rx = concat(
        {attribute(NL80211_RATE_INFO_BITRATE, value<std::uint16_t>(7800)), attribute(NL80211_RATE_INFO_VHT_MCS, value<std::uint8_t>(9))});
    const Bytes info = concat({attribute(NL80211_STA_INFO_SIGNAL, value<std::int8_t>(-52)),
                               attribute(NLA_F_NESTED | NL80211_STA_INFO_TX_BITRATE, tx),
                               attribute(NLA_F_NESTED | NL80211_STA_INFO_RX_BITRATE, rx)});
    const Bytes reply =
        message(FAMILY,
                NL80211_CMD_NEW_STATION,
                concat({attribute(NL80211_ATTR_IFINDEX, value<std::uint32_t>(3)), attribute(NLA_F_NESTED | NL80211_ATTR_STA_INFO, info)}));
    const std::optional<Nl80211::Station> parsed = Nl80211::parseStation(reply, FAMILY);
    ASSERT_TRUE(parsed.has_value());
    const Nl80211::Station station = parsed.value_or(Nl80211::Station{});
    const Nl80211::Rate txRate = station.tx.value_or(Nl80211::Rate{});
    const Nl80211::Rate rxRate = station.rx.value_or(Nl80211::Rate{});
    EXPECT_TRUE(station.tx.has_value());
    EXPECT_EQ(txRate.kbps, 866'700U);
    EXPECT_EQ(txRate.kind, Nl80211::RateKind::He);
    EXPECT_TRUE(station.rx.has_value());
    EXPECT_EQ(rxRate.kbps, 780'000U) << "the 16-bit field when the 32-bit one is absent";
    EXPECT_EQ(rxRate.kind, Nl80211::RateKind::Vht);
    EXPECT_EQ(Nl80211::standardName(station, 5180), "802.11ax (Wi-Fi 6)");
    EXPECT_EQ(Nl80211::standardName(station, 5955), "802.11ax (Wi-Fi 6E)");

    EXPECT_FALSE(Nl80211::parseStation(reply, FAMILY + 1).has_value()) << "another family's message";
    Bytes cut = reply;
    cut.resize(cut.size() - 6); // a truncated reply: its declared length runs past the bytes
    EXPECT_FALSE(Nl80211::parseStation(cut, FAMILY).has_value());
}

TEST(Nl80211Test, EachRateKindNamesItsStandard)
{
    const auto named = [](Nl80211::RateKind kind, std::uint32_t kbps, std::uint32_t mhz)
    {
        return Nl80211::standardName({.tx = Nl80211::Rate{.kbps = kbps, .kind = kind}, .rx = std::nullopt}, mhz);
    };
    EXPECT_EQ(named(Nl80211::RateKind::Eht, 2'000'000, 6000), "802.11be (Wi-Fi 7)");
    EXPECT_EQ(named(Nl80211::RateKind::Vht, 433'300, 5180), "802.11ac (Wi-Fi 5)");
    EXPECT_EQ(named(Nl80211::RateKind::Ht, 144'400, 2437), "802.11n (Wi-Fi 4)");
    EXPECT_EQ(named(Nl80211::RateKind::Legacy, 54'000, 5180), "802.11a");
    EXPECT_EQ(named(Nl80211::RateKind::Legacy, 54'000, 2412), "802.11g");
    EXPECT_EQ(named(Nl80211::RateKind::Legacy, 11'000, 2412), "802.11b");
    EXPECT_EQ(Nl80211::standardName({}, 2412), "") << "no rates reported";
}

TEST(Nl80211Test, TheStationRequestNamesTheAdapter)
{
    const Bytes request = Nl80211::stationRequest(FAMILY, 2, 3);
    std::uint16_t type = 0;
    std::uint16_t flags = 0;
    std::memcpy(&type, request.data() + 4, 2);
    std::memcpy(&flags, request.data() + 6, 2);
    EXPECT_EQ(type, FAMILY);
    EXPECT_EQ(flags, NLM_F_REQUEST | NLM_F_DUMP);
    EXPECT_EQ(static_cast<std::uint8_t>(request[NLMSG_HDRLEN]), NL80211_CMD_GET_STATION);
    const Bytes ifindex = attribute(NL80211_ATTR_IFINDEX, value<std::uint32_t>(3));
    EXPECT_TRUE(std::equal(ifindex.begin(), ifindex.end(), request.end() - static_cast<std::ptrdiff_t>(ifindex.size())));
}

// The real socket: the loopback adapter (index 1) has no Wi-Fi station, with or without nl80211 here, and
// the read ends promptly rather than waiting out its receive timeout more than once.
TEST(Nl80211Test, AnAdapterWithoutWifiHasNoStation)
{
    const auto started = std::chrono::steady_clock::now();
    EXPECT_FALSE(Nl80211::readStation(1).has_value());
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(3));
}

} // namespace
} // namespace Platform

// NOLINTEND(misc-include-cleaner)
