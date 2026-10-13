#pragma once

// The public IP lookup on Windows (#1894), the Winsock twin of Platform/Linux/LinuxPublicIp.h (#1518):
// asks OpenDNS's resolvers for myip.opendns.com, which they answer with the address the query came
// from. An A query to the IPv4 resolver finds the IPv4 address, an AAAA query to the IPv6 one the IPv6
// address; both are sent at once over non-blocking UDP sockets and waited for together with WSAPoll, so
// the lookup takes at most one timeout. The query and the reply go through the shared codec
// (Platform/DnsMessage.h); the reasons shown for a missing address are the Linux ones. Only run when the
// user asks (it sends a query off the machine), from the System tab's background read, never the UI
// thread. Each lookup holds its own WSAStartup reference and releases it (Winsock counts them), so it
// is safe alongside the app's other Winsock users. The resolvers are injectable, so the tests answer
// from a fake server on localhost.

#include "Platform/ISystemInfoProbe.h"

#include <chrono>
#include <cstdint>
#include <string>

namespace Platform::WindowsPublicIp
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

namespace Detail
{
/// Why sending to, or hearing from, the resolver failed with the Winsock error @p wsaError
/// (WSAGetLastError()), for the UI.
[[nodiscard]] std::string networkError(int wsaError, bool v6);
} // namespace Detail

/// Looks the public addresses up. Always sets `read`; an address it couldn't find is empty with its
/// *Unavailable reason set.
[[nodiscard]] PublicIpInfo lookUp(const Options& options = {});

} // namespace Platform::WindowsPublicIp
