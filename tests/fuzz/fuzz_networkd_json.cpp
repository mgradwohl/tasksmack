#include "Platform/Linux/NetworkdLinkJson.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

// systemd-networkd's link description (#1518) on arbitrary text: the JSON parser and the DHCPv4 lease
// read from it. Nothing may read past the input, recurse without bound, or throw; a lease read from the
// text converts to Unix time without overflow.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) // NOLINT(readability-identifier-naming)
{
    const std::string_view text(reinterpret_cast<const char*>(data), size);
    namespace Json = Platform::NetworkdLinkJson;

    static_cast<void>(Json::parse(text));
    if (const std::optional<Json::Lease> lease = Json::leaseFromDescription(text); lease.has_value())
    {
        static_cast<void>(Json::toDhcpLease(*lease, 1'760'000'000, 3'600'000'000));
        static_cast<void>(Json::toDhcpLease(*lease, 0, UINT64_MAX));
    }
    return 0;
}
