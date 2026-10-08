#pragma once

// Linux's view of a TCP or UDP socket, as both of its sources report it (#799): an INET_DIAG dump
// (NetlinkSocketStats.h, dumpInetSockets()) and the /proc/[pid]/net/{tcp,tcp6,udp,udp6} tables, the
// fallback when netlink can't be used. Pure, so the table parser and the state mapping are tested on
// fixtures (tests/Platform/test_InetSocketTable.cpp).

#include "Platform/IProcessConnections.h"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <system_error>
#include <vector>

namespace Platform::InetSockets
{

/// One socket, raw: the kernel's state number and the endpoints in network byte order.
struct RawInetSocket
{
    std::uint64_t inode = 0; ///< 0 for a socket no file refers to any more (TIME_WAIT, an orphan)
    ConnectionProtocol protocol = ConnectionProtocol::Tcp;
    ConnectionFamily family = ConnectionFamily::IPv4;
    std::uint8_t state = 0; ///< include/net/tcp_states.h (TCP_ESTABLISHED = 1 ... TCP_NEW_SYN_RECV = 12)
    ConnectionEndpoint local;
    ConnectionEndpoint remote;
};

/// The platform-neutral state for a kernel TCP state number (include/net/tcp_states.h). UDP uses the
/// same numbers: TCP_ESTABLISHED once connect()ed, TCP_CLOSE when only bound.
[[nodiscard]] constexpr ConnectionState connectionStateFromLinux(std::uint8_t state) noexcept
{
    switch (state)
    {
    case 1:
        return ConnectionState::Established;
    case 2:
        return ConnectionState::SynSent;
    case 3:
    case 12: // TCP_NEW_SYN_RECV: a request socket, shown as the SYN_RECV it stands for
        return ConnectionState::SynReceived;
    case 4:
        return ConnectionState::FinWait1;
    case 5:
        return ConnectionState::FinWait2;
    case 6:
        return ConnectionState::TimeWait;
    case 7:
        return ConnectionState::Closed;
    case 8:
        return ConnectionState::CloseWait;
    case 9:
        return ConnectionState::LastAck;
    case 10:
        return ConnectionState::Listen;
    case 11:
        return ConnectionState::Closing;
    default:
        return ConnectionState::Unknown;
    }
}

/// The interface row for @p socket.
[[nodiscard]] constexpr ProcessConnection toConnection(const RawInetSocket& socket) noexcept
{
    return {.protocol = socket.protocol,
            .family = socket.family,
            .local = socket.local,
            .remote = socket.remote,
            .state = connectionStateFromLinux(socket.state)};
}

namespace Detail
{

/// @p text as one hexadecimal number of exactly its length (at most 8 digits), or nullopt.
[[nodiscard]] constexpr std::optional<std::uint32_t> parseHex32(std::string_view text) noexcept
{
    if (text.empty() || text.size() > 8)
    {
        return std::nullopt;
    }
    std::uint32_t value = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value, 16);
    if (ec != std::errc{} || end != text.data() + text.size())
    {
        return std::nullopt;
    }
    return value;
}

/// Splits off the next run of non-blank characters of @p text (advancing it past them).
[[nodiscard]] constexpr std::string_view nextField(std::string_view& text) noexcept
{
    const std::size_t start = text.find_first_not_of(" \t");
    if (start == std::string_view::npos)
    {
        text = {};
        return {};
    }
    text.remove_prefix(start);
    // Views cut with remove_prefix/remove_suffix rather than substr(), which may throw: this is noexcept.
    std::string_view field = text;
    if (const std::size_t end = text.find_first_of(" \t"); end != std::string_view::npos)
    {
        field.remove_suffix(text.size() - end);
    }
    text.remove_prefix(field.size());
    return field;
}

/// Parses a /proc/net endpoint ("0100007F:0035" or 32 hex digits ":" port) for @p family.
/// The kernel prints each 32-bit word of the address (stored in network order) as a host-order
/// number with %08X, so the word's in-memory bytes, recovered with bit_cast, are the network-order
/// address bytes; the port is printed as a plain number.
[[nodiscard]] constexpr std::optional<ConnectionEndpoint> parseEndpoint(std::string_view text, ConnectionFamily family) noexcept
{
    const std::size_t colon = text.find(':');
    if (colon == std::string_view::npos)
    {
        return std::nullopt;
    }
    std::string_view address = text;
    address.remove_suffix(text.size() - colon);
    std::string_view port = text;
    port.remove_prefix(colon + 1);
    const std::size_t words = (family == ConnectionFamily::IPv4) ? 1 : 4;
    if (address.size() != words * 8)
    {
        return std::nullopt;
    }
    ConnectionEndpoint endpoint;
    for (std::size_t word = 0; word < words; ++word)
    {
        std::string_view digits = address;
        digits.remove_prefix(word * 8);
        digits.remove_suffix(digits.size() - 8);
        const std::optional<std::uint32_t> value = parseHex32(digits);
        if (!value.has_value())
        {
            return std::nullopt;
        }
        const auto bytes = std::bit_cast<std::array<std::uint8_t, 4>>(*value);
        std::ranges::copy(bytes, endpoint.address.begin() + static_cast<std::ptrdiff_t>(word * 4));
    }
    const std::optional<std::uint32_t> portValue = (port.size() == 4) ? parseHex32(port) : std::nullopt;
    if (!portValue.has_value())
    {
        return std::nullopt;
    }
    endpoint.port = static_cast<std::uint16_t>(*portValue);
    return endpoint;
}

} // namespace Detail

/// One data line of /proc/net/{tcp,tcp6,udp,udp6} ("  sl  local_address rem_address   st ... inode
/// ..."), or nullopt for the header line or a malformed one. @p protocol and @p family say which
/// table the line came from.
[[nodiscard]] constexpr std::optional<RawInetSocket>
parseProcNetLine(std::string_view line, ConnectionProtocol protocol, ConnectionFamily family) noexcept
{
    // sl local_address rem_address st tx_queue:rx_queue tr:tm->when retrnsmt uid timeout inode ...
    const std::string_view slot = Detail::nextField(line);
    if (slot.empty() || slot.back() != ':')
    {
        return std::nullopt; // the header line ("sl"), or not a data line
    }
    const std::optional<ConnectionEndpoint> local = Detail::parseEndpoint(Detail::nextField(line), family);
    const std::optional<ConnectionEndpoint> remote = Detail::parseEndpoint(Detail::nextField(line), family);
    const std::string_view stateField = Detail::nextField(line);
    const std::optional<std::uint32_t> state = (stateField.size() == 2) ? Detail::parseHex32(stateField) : std::nullopt;
    if (!local.has_value() || !remote.has_value() || !state.has_value())
    {
        return std::nullopt;
    }
    for (int skipped = 0; skipped < 5; ++skipped) // tx:rx, tr:when, retrnsmt, uid, timeout
    {
        if (Detail::nextField(line).empty())
        {
            return std::nullopt;
        }
    }
    const std::string_view inodeField = Detail::nextField(line);
    std::uint64_t inode = 0;
    const auto [end, ec] = std::from_chars(inodeField.data(), inodeField.data() + inodeField.size(), inode);
    if (inodeField.empty() || ec != std::errc{} || end != inodeField.data() + inodeField.size())
    {
        return std::nullopt;
    }
    return RawInetSocket{.inode = inode,
                         .protocol = protocol,
                         .family = family,
                         .state = static_cast<std::uint8_t>(*state),
                         .local = *local,
                         .remote = *remote};
}

/// Every socket in the whole text of one /proc/net table, appended to @p out. Lines that do not parse
/// (the header, a truncated last line) are skipped.
inline void
parseProcNetTable(std::string_view contents, ConnectionProtocol protocol, ConnectionFamily family, std::vector<RawInetSocket>& out)
{
    while (!contents.empty())
    {
        const std::size_t newline = contents.find('\n');
        const std::string_view line = contents.substr(0, newline);
        contents.remove_prefix(newline == std::string_view::npos ? contents.size() : newline + 1);
        if (const std::optional<RawInetSocket> socket = parseProcNetLine(line, protocol, family))
        {
            out.push_back(*socket);
        }
    }
}

} // namespace Platform::InetSockets
