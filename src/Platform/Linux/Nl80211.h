#pragma once

// A Wi-Fi adapter's link as the kernel's nl80211 sees it (#2591): the Wi-Fi standard of the rates in use and
// the receive rate, which NetworkManager doesn't give. One NL80211_CMD_GET_STATION dump over a generic
// netlink socket per read, as `iw dev wlan0 station dump` asks it; no privilege needed. The message building
// and parsing are pure, so tests feed them bytes; readStation() does the socket.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Platform::Nl80211
{

/// The kinds of rate nl80211 reports a station's rates in, newest first.
enum class RateKind : std::uint8_t
{
    Legacy, ///< No MCS: 802.11a/b/g
    Ht,     ///< 802.11n (Wi-Fi 4)
    Vht,    ///< 802.11ac (Wi-Fi 5)
    He,     ///< 802.11ax (Wi-Fi 6, 6E at 6 GHz)
    Eht,    ///< 802.11be (Wi-Fi 7)
};

/// One of a station's rates (NL80211_STA_INFO_TX_BITRATE or _RX_BITRATE).
struct Rate
{
    std::uint32_t kbps = 0; ///< 0 when not reported
    RateKind kind = RateKind::Legacy;
};

/// The access point the adapter is associated with, as one GET_STATION reply describes it.
struct Station
{
    std::optional<Rate> tx;
    std::optional<Rate> rx;
};

/// The standard a link with @p tx and @p rx rates uses, at @p frequencyMhz (0 when unknown), as Windows
/// names it: "802.11ax (Wi-Fi 6)", "802.11ax (Wi-Fi 6E)" at 6 GHz, "802.11g". The newer of the two rates'
/// kinds counts; legacy rates are 802.11a at 5 GHz, else 802.11b up to 11 Mbit/s, else 802.11g. Empty when
/// neither rate was reported.
/// Inline, with no socket code, since LinuxNetworkAdapters.h calls it and that header is also built on
/// Windows, where its parsers are unit-tested and this file's .cpp isn't compiled.
[[nodiscard]] inline std::string_view standardName(const Station& station, std::uint32_t frequencyMhz)
{
    if (!station.tx && !station.rx)
    {
        return {};
    }
    RateKind kind = RateKind::Legacy;
    std::uint32_t fastestKbps = 0;
    for (const std::optional<Rate>& rate : {station.tx, station.rx})
    {
        if (rate)
        {
            kind = rate->kind > kind ? rate->kind : kind;
            fastestKbps = rate->kbps > fastestKbps ? rate->kbps : fastestKbps;
        }
    }
    constexpr std::uint32_t SIX_GHZ_LOW_MHZ = 5925;
    constexpr std::uint32_t FIVE_GHZ_LOW_MHZ = 5000;
    constexpr std::uint32_t ELEVEN_MBITS_KBPS = 11'000;
    switch (kind)
    {
    case RateKind::Eht:
        return "802.11be (Wi-Fi 7)";
    case RateKind::He:
        return frequencyMhz >= SIX_GHZ_LOW_MHZ ? "802.11ax (Wi-Fi 6E)" : "802.11ax (Wi-Fi 6)";
    case RateKind::Vht:
        return "802.11ac (Wi-Fi 5)";
    case RateKind::Ht:
        return "802.11n (Wi-Fi 4)";
    case RateKind::Legacy:
        break;
    }
    if (frequencyMhz >= FIVE_GHZ_LOW_MHZ)
    {
        return "802.11a";
    }
    return fastestKbps != 0 && fastestKbps <= ELEVEN_MBITS_KBPS ? "802.11b" : "802.11g";
}

/// A CTRL_CMD_GETFAMILY request for "nl80211" (generic netlink's controller), with sequence @p seq.
[[nodiscard]] std::vector<std::byte> familyRequest(std::uint32_t seq);

/// nl80211's generic netlink family id from the controller's reply @p message; nullopt when it isn't one.
[[nodiscard]] std::optional<std::uint16_t> parseFamilyId(std::span<const std::byte> message);

/// An NL80211_CMD_GET_STATION dump request for the adapter with index @p ifindex.
[[nodiscard]] std::vector<std::byte> stationRequest(std::uint16_t familyId, std::uint32_t seq, std::uint32_t ifindex);

/// The station a GET_STATION reply @p message (one netlink message, header included) describes; nullopt
/// when it isn't one or has no station info.
[[nodiscard]] std::optional<Station> parseStation(std::span<const std::byte> message, std::uint16_t familyId);

/// The adapter's station, read now over a generic netlink socket; nullopt when nl80211 isn't there, the
/// adapter isn't associated, or the read failed or timed out. Blocking, bounded by a receive timeout.
[[nodiscard]] std::optional<Station> readStation(std::uint32_t ifindex);

} // namespace Platform::Nl80211
