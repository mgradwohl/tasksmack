#pragma once

// The public IP lookup (#1518): asks OpenDNS's resolvers for myip.opendns.com, which they answer with
// the address the query came from. An A query to the IPv4 resolver finds the IPv4 address, an AAAA
// query to the IPv6 one the IPv6 address; both are sent at once over UDP and waited for together, so
// the lookup takes at most one timeout. Only run when the user asks (it sends a query off the machine).
// The resolvers are injectable, so the tests answer from a fake server on localhost.

#include "Platform/DnsMessage.h"
#include "Platform/ISystemInfoProbe.h"

#include <chrono>
#include <cstdint>
#include <string>

namespace Platform::LinuxPublicIp
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

/// Looks the public addresses up. Always sets `read`; an address it couldn't find is empty with its
/// *Unavailable reason set.
[[nodiscard]] PublicIpInfo lookUp(const Options& options = {});

} // namespace Platform::LinuxPublicIp
