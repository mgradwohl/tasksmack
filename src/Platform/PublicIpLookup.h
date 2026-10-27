#pragma once

// What the public IP lookups share (#3117): Platform/Linux/LinuxPublicIp.h (#1518) and its Winsock twin
// Platform/Windows/WindowsPublicIp.h (#1894) ask the same resolvers the same question, read the reply with
// the same codec (Platform/DnsMessage.h) and give the same reasons for a missing address. This holds the
// platform-independent half: the resolvers and their defaults, what a reply means for its query, and the
// PublicIpInfo built from the two queries. Sockets, the waiting, the OS's errors and turning an address
// into text stay with each platform. Pure -- no sockets -- so it is unit-tested on any host.

#include "Platform/DnsMessage.h"
#include "Platform/ISystemInfoProbe.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <variant>

namespace Platform::PublicIpLookup
{

/// A resolver to ask: a numeric address ("208.67.222.222", "::1") and its port.
struct Resolver
{
    std::string address;
    std::uint16_t port = 53;
};

struct Options
{
    Resolver v4Resolver{.address = "208.67.222.222", .port = 53};  ///< resolver1.opendns.com
    Resolver v6Resolver{.address = "2620:119:35::35", .port = 53}; ///< resolver1.opendns.com over IPv6
    std::string name = "myip.opendns.com";
    std::chrono::milliseconds timeout{2000}; ///< For both queries together
    std::string source = "OpenDNS (myip.opendns.com)";
};

/// Why an address is missing when the resolver's reply, or the datagram it came in, couldn't be read.
inline constexpr std::string_view UNREADABLE_REPLY = "The resolver's reply couldn't be read";

/// A reply as DnsMessage::parseAnswer() reads it.
using Answer = std::variant<DnsMessage::Ipv4, DnsMessage::Ipv6, DnsMessage::AnswerError>;

/// The platform's text for the address in @p answer ("203.0.113.7", "2001:db8::7"), from its
/// inet_ntop(); empty when it can't make one. Only called for an Ipv4 or Ipv6 answer.
using AddressText = std::string (*)(const Answer& answer);

/// What one datagram from the resolver means for its query.
struct ReplyDisposition
{
    enum class Kind : std::uint8_t
    {
        Ignore,  ///< Not the reply (a stray datagram): keep waiting for it
        Fail,    ///< The reply, but no address in it: `text` says why, for the UI
        Address, ///< The reply and its address: `text` is the address
    };

    Kind kind = Kind::Ignore;
    std::string text;
};

/// What @p answer, the reply parsed for an A query (or an AAAA one when @p v6), means for its query:
/// a datagram that isn't the reply is ignored, an error is a reason, and an address is its text from
/// @p addressText (a reason when there is no text).
[[nodiscard]] ReplyDisposition replyDisposition(const Answer& answer, bool v6, AddressText addressText);

/// How one query ended, once it has (`done`): the address found, or why there is none. Each platform's
/// query in flight adds its socket to this.
struct Outcome
{
    bool done = false;
    std::string address;
    std::string unavailable;

    /// Ends the query without an address, for @p reason.
    void fail(std::string reason);

    /// Ends the query as @p reply says; an ignored reply leaves it waiting.
    void settle(ReplyDisposition reply);
};

/// Why an address is missing when its query wasn't answered within @p timeout, rounded up to seconds.
[[nodiscard]] std::string timedOutReason(std::chrono::milliseconds timeout);

/// The lookup's result from the IPv4 query @p v4 and the IPv6 query @p v6: a query not done by then
/// timed out. Always sets `read`.
[[nodiscard]] PublicIpInfo assembleInfo(const Options& options, const Outcome& v4, const Outcome& v6);

} // namespace Platform::PublicIpLookup
