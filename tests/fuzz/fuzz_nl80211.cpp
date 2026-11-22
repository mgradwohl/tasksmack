#include "Platform/Linux/Nl80211.h"

#include <cstddef>
#include <cstdint>
#include <span>

// One nl80211 netlink message, as the Wi-Fi details read the kernel's replies (#4616): the generic-netlink family
// lookup and a GET_STATION reply, its attributes stepped by hand (#4581). The first two bytes pick the family id the
// station reply is checked against. Nothing may read past the message.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) // NOLINT(readability-identifier-naming)
{
    const std::span<const std::byte> bytes(reinterpret_cast<const std::byte*>(data), size);
    namespace Nl = Platform::Nl80211;

    static_cast<void>(Nl::parseFamilyId(bytes));
    if (size >= 2)
    {
        const auto familyId = static_cast<std::uint16_t>((static_cast<unsigned>(data[0]) << 8U) | data[1]);
        static_cast<void>(Nl::parseStation(bytes.subspan(2), familyId));
    }
    return 0;
}
