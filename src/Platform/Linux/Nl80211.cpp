#include "Nl80211.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <vector>

// NOLINTBEGIN(misc-include-cleaner) - kernel netlink headers: include-cleaner lacks mappings for NLMSG_*,
// NLA_*, CTRL_*, GENL_* and NL80211_*
#include <linux/genetlink.h>
#include <linux/netlink.h>
#include <linux/nl80211.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace Platform::Nl80211
{

namespace
{

constexpr int RECEIVE_TIMEOUT_MS = 1000;
constexpr std::size_t RECEIVE_BUFFER_BYTES = std::size_t{32} * 1024U;

constexpr std::size_t HEADER_BYTES = NLMSG_HDRLEN + GENL_HDRLEN;

[[nodiscard]] constexpr std::size_t align4(std::size_t size) noexcept
{
    return (size + 3U) & ~std::size_t{3};
}

/// A little-endian (host order) value read from @p bytes at @p offset.
template<typename T> [[nodiscard]] T readValue(std::span<const std::byte> bytes, std::size_t offset)
{
    T value{};
    std::memcpy(&value, bytes.data() + offset, sizeof(T));
    return value;
}

template<typename T> void appendValue(std::vector<std::byte>& out, T value)
{
    const std::span<const std::byte, sizeof(T)> raw = std::as_bytes(std::span<const T, 1>(&value, 1));
    out.insert(out.end(), raw.begin(), raw.end());
}

/// Appends one attribute of @p type holding @p payload, padded to 4 bytes.
void appendAttribute(std::vector<std::byte>& out, std::uint16_t type, std::span<const std::byte> payload)
{
    appendValue<std::uint16_t>(out, static_cast<std::uint16_t>(NLA_HDRLEN + payload.size()));
    appendValue<std::uint16_t>(out, type);
    out.insert(out.end(), payload.begin(), payload.end());
    out.resize(align4(out.size()));
}

/// A netlink message of @p type with a generic netlink header for @p command, ready for attributes; the
/// length is fixed by finish().
[[nodiscard]] std::vector<std::byte> startMessage(std::uint16_t type, std::uint16_t flags, std::uint32_t seq, std::uint8_t command)
{
    std::vector<std::byte> out;
    appendValue<std::uint32_t>(out, 0); // nlmsg_len, set by finish()
    appendValue<std::uint16_t>(out, type);
    appendValue<std::uint16_t>(out, flags);
    appendValue<std::uint32_t>(out, seq);
    appendValue<std::uint32_t>(out, 0); // nlmsg_pid: the kernel fills it
    out.push_back(static_cast<std::byte>(command));
    out.push_back(std::byte{1}); // version
    appendValue<std::uint16_t>(out, 0);
    return out;
}

void finish(std::vector<std::byte>& message)
{
    const auto length = static_cast<std::uint32_t>(message.size());
    std::memcpy(message.data(), &length, sizeof(length));
}

/// Each attribute in @p bytes: calls @p visit(type without its flags, payload) until it returns true.
template<typename Visit> void forEachAttribute(std::span<const std::byte> bytes, const Visit& visit)
{
    std::size_t offset = 0;
    while (offset + NLA_HDRLEN <= bytes.size())
    {
        const auto length = readValue<std::uint16_t>(bytes, offset);
        const auto type = static_cast<std::uint16_t>(readValue<std::uint16_t>(bytes, offset + 2) & NLA_TYPE_MASK);
        if (length < NLA_HDRLEN || offset + length > bytes.size())
        {
            return; // malformed: stop rather than read past it
        }
        if (visit(type, bytes.subspan(offset + NLA_HDRLEN, length - NLA_HDRLEN)))
        {
            return;
        }
        offset += align4(length);
    }
}

/// The attributes of one generic netlink message of @p type and @p command; empty when it isn't one.
[[nodiscard]] std::span<const std::byte> attributesOf(std::span<const std::byte> message, std::uint16_t type, std::uint8_t command)
{
    if (message.size() < HEADER_BYTES)
    {
        return {};
    }
    const auto length = readValue<std::uint32_t>(message, 0);
    if (length < HEADER_BYTES || length > message.size() || readValue<std::uint16_t>(message, 4) != type ||
        static_cast<std::uint8_t>(message[NLMSG_HDRLEN]) != command)
    {
        return {};
    }
    return message.subspan(HEADER_BYTES, length - HEADER_BYTES);
}

/// A NL80211_STA_INFO_*_BITRATE nest's rate.
[[nodiscard]] Rate parseRate(std::span<const std::byte> nest)
{
    Rate rate;
    std::uint32_t bitrate16 = 0;
    forEachAttribute(nest,
                     [&rate, &bitrate16](std::uint16_t type, std::span<const std::byte> payload)
                     {
                         if (type == NL80211_RATE_INFO_BITRATE32 && payload.size() >= 4)
                         {
                             rate.kbps = readValue<std::uint32_t>(payload, 0) * 100U; // in 100 kbit/s
                         }
                         else if (type == NL80211_RATE_INFO_BITRATE && payload.size() >= 2)
                         {
                             bitrate16 = std::uint32_t{readValue<std::uint16_t>(payload, 0)} * 100U;
                         }
                         else if (type == NL80211_RATE_INFO_EHT_MCS)
                         {
                             rate.kind = RateKind::Eht;
                         }
                         else if (type == NL80211_RATE_INFO_HE_MCS && rate.kind < RateKind::He)
                         {
                             rate.kind = RateKind::He;
                         }
                         else if (type == NL80211_RATE_INFO_VHT_MCS && rate.kind < RateKind::Vht)
                         {
                             rate.kind = RateKind::Vht;
                         }
                         else if (type == NL80211_RATE_INFO_MCS && rate.kind < RateKind::Ht)
                         {
                             rate.kind = RateKind::Ht;
                         }
                         return false;
                     });
    if (rate.kbps == 0)
    {
        rate.kbps = bitrate16; // the 16-bit field, from a kernel or rate too old for the 32-bit one
    }
    return rate;
}

/// A file descriptor closed on scope exit.
class Socket
{
  public:
    explicit Socket(int fd) noexcept : m_Fd(fd)
    {}
    ~Socket()
    {
        if (m_Fd >= 0)
        {
            ::close(m_Fd);
        }
    }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    Socket(Socket&&) = delete;
    Socket& operator=(Socket&&) = delete;

    [[nodiscard]] int get() const noexcept
    {
        return m_Fd;
    }

  private:
    int m_Fd;
};

/// Sends @p request and calls @p onMessage(message) for each reply message until NLMSG_DONE, an error, a
/// non-multipart reply or a timeout. @return false on a failure (an error reply counts).
template<typename OnMessage>
[[nodiscard]] bool exchange(int fd, const std::vector<std::byte>& request, std::uint32_t seq, const OnMessage& onMessage)
{
    if (::send(fd, request.data(), request.size(), 0) != static_cast<ssize_t>(request.size()))
    {
        return false;
    }
    std::vector<std::byte> buffer(RECEIVE_BUFFER_BYTES);
    for (int datagrams = 0; datagrams < 64; ++datagrams)
    {
        const ssize_t received = ::recv(fd, buffer.data(), buffer.size(), 0);
        if (received <= 0)
        {
            return false;
        }
        std::span<const std::byte> rest(buffer.data(), static_cast<std::size_t>(received));
        while (rest.size() >= NLMSG_HDRLEN)
        {
            const auto length = readValue<std::uint32_t>(rest, 0);
            const auto type = readValue<std::uint16_t>(rest, 4);
            const auto flags = readValue<std::uint16_t>(rest, 6);
            if (length < NLMSG_HDRLEN || length > rest.size())
            {
                return false;
            }
            if (readValue<std::uint32_t>(rest, 8) == seq)
            {
                if (type == NLMSG_DONE)
                {
                    return true;
                }
                if (type == NLMSG_ERROR)
                {
                    return rest.size() >= NLMSG_HDRLEN + sizeof(std::int32_t) && readValue<std::int32_t>(rest, NLMSG_HDRLEN) == 0;
                }
                onMessage(rest.first(length));
                if ((flags & NLM_F_MULTI) == 0)
                {
                    return true;
                }
            }
            rest = rest.subspan(std::min<std::size_t>(NLMSG_ALIGN(length), rest.size()));
        }
    }
    return false;
}

} // namespace

std::vector<std::byte> familyRequest(std::uint32_t seq)
{
    std::vector<std::byte> message = startMessage(GENL_ID_CTRL, NLM_F_REQUEST, seq, CTRL_CMD_GETFAMILY);
    constexpr std::array<char, 8> NAME{'n', 'l', '8', '0', '2', '1', '1', '\0'};
    appendAttribute(message, CTRL_ATTR_FAMILY_NAME, std::as_bytes(std::span(NAME)));
    finish(message);
    return message;
}

std::optional<std::uint16_t> parseFamilyId(std::span<const std::byte> message)
{
    std::optional<std::uint16_t> id;
    forEachAttribute(attributesOf(message, GENL_ID_CTRL, CTRL_CMD_NEWFAMILY),
                     [&id](std::uint16_t type, std::span<const std::byte> payload)
                     {
                         if (type == CTRL_ATTR_FAMILY_ID && payload.size() >= 2)
                         {
                             id = readValue<std::uint16_t>(payload, 0);
                             return true;
                         }
                         return false;
                     });
    return id;
}

std::vector<std::byte> stationRequest(std::uint16_t familyId, std::uint32_t seq, std::uint32_t ifindex)
{
    std::vector<std::byte> message = startMessage(familyId, NLM_F_REQUEST | NLM_F_DUMP, seq, NL80211_CMD_GET_STATION);
    appendAttribute(message, NL80211_ATTR_IFINDEX, std::as_bytes(std::span(&ifindex, 1)));
    finish(message);
    return message;
}

std::optional<Station> parseStation(std::span<const std::byte> message, std::uint16_t familyId)
{
    std::optional<Station> station;
    forEachAttribute(attributesOf(message, familyId, NL80211_CMD_NEW_STATION),
                     [&station](std::uint16_t type, std::span<const std::byte> info)
                     {
                         if (type != NL80211_ATTR_STA_INFO)
                         {
                             return false;
                         }
                         Station parsed;
                         forEachAttribute(info,
                                          [&parsed](std::uint16_t field, std::span<const std::byte> nest)
                                          {
                                              if (field == NL80211_STA_INFO_TX_BITRATE)
                                              {
                                                  parsed.tx = parseRate(nest);
                                              }
                                              else if (field == NL80211_STA_INFO_RX_BITRATE)
                                              {
                                                  parsed.rx = parseRate(nest);
                                              }
                                              return false;
                                          });
                         station = parsed;
                         return true;
                     });
    return station;
}

std::optional<Station> readStation(std::uint32_t ifindex)
{
    const Socket socket(::socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_GENERIC));
    if (socket.get() < 0)
    {
        return std::nullopt;
    }
    timeval timeout{.tv_sec = RECEIVE_TIMEOUT_MS / 1000, .tv_usec = static_cast<suseconds_t>(RECEIVE_TIMEOUT_MS % 1000) * 1000};
    if (::setsockopt(socket.get(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0)
    {
        return std::nullopt; // fails closed: a receive that could block forever isn't tried
    }
    std::optional<std::uint16_t> familyId;
    if (!exchange(socket.get(), familyRequest(1), 1, [&familyId](std::span<const std::byte> m) { familyId = parseFamilyId(m); }) ||
        !familyId)
    {
        return std::nullopt; // no nl80211 (no Wi-Fi driver loaded, or a kernel without cfg80211)
    }
    std::optional<Station> station;
    const bool done = exchange(socket.get(),
                               stationRequest(*familyId, 2, ifindex),
                               2,
                               [&station, id = *familyId](std::span<const std::byte> m)
                               {
                                   if (!station)
                                   {
                                       station = parseStation(m, id); // a station interface has one: its access point
                                   }
                               });
    return done ? station : std::nullopt;
}

} // namespace Platform::Nl80211

// NOLINTEND(misc-include-cleaner)
