#pragma once

// The public IP lookup on Windows (#1894), the Winsock twin of Platform/Linux/LinuxPublicIp.h (#1518):
// asks OpenDNS's resolvers for myip.opendns.com, which they answer with the address the query came
// from. An A query to the IPv4 resolver finds the IPv4 address, an AAAA query to the IPv6 one the IPv6
// address; both are sent at once over non-blocking UDP sockets and waited for together with WSAPoll, so
// the lookup takes at most one timeout. The query and the reply go through the shared codec
// (Platform/DnsMessage.h), and the options, what a reply means and the reasons shown for a missing
// address are shared with Linux (Platform/PublicIpLookup.h, #3117). Only run when the
// user asks (it sends a query off the machine), from the System tab's background read, never the UI
// thread. Each lookup holds its own WSAStartup reference and releases it (Winsock counts them), so it
// is safe alongside the app's other Winsock users. The resolvers are injectable, so the tests answer
// from a fake server on localhost.

#include "Platform/ISystemInfoProbe.h"
#include "Platform/PublicIpLookup.h"

#include <string>

namespace Platform::WindowsPublicIp
{

/// A resolver to ask, and the lookup's resolvers, name and timeout: shared with the other platform's
/// lookup (Platform/PublicIpLookup.h, #3117).
using Resolver = PublicIpLookup::Resolver;
using Options = PublicIpLookup::Options;

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
