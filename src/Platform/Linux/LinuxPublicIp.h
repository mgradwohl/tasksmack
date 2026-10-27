#pragma once

// The public IP lookup (#1518): asks OpenDNS's resolvers for myip.opendns.com, which they answer with
// the address the query came from. An A query to the IPv4 resolver finds the IPv4 address, an AAAA
// query to the IPv6 one the IPv6 address; both are sent at once over UDP and waited for together, so
// the lookup takes at most one timeout. Only run when the user asks (it sends a query off the machine).
// The resolvers are injectable, so the tests answer from a fake server on localhost. What isn't about
// sockets (the options, what a reply means, the reasons shown) is shared with Windows in
// Platform/PublicIpLookup.h (#3117).

#include "Platform/ISystemInfoProbe.h"
#include "Platform/PublicIpLookup.h"

#include <string>

namespace Platform::LinuxPublicIp
{

/// A resolver to ask, and the lookup's resolvers, name and timeout: shared with the other platform's
/// lookup (Platform/PublicIpLookup.h, #3117).
using Resolver = PublicIpLookup::Resolver;
using Options = PublicIpLookup::Options;

namespace Detail
{
/// Why sending to, or hearing from, the resolver failed with @p errorNumber, for the UI.
[[nodiscard]] std::string networkError(int errorNumber, bool v6);
} // namespace Detail

/// Looks the public addresses up. Always sets `read`; an address it couldn't find is empty with its
/// *Unavailable reason set.
[[nodiscard]] PublicIpInfo lookUp(const Options& options = {});

} // namespace Platform::LinuxPublicIp
