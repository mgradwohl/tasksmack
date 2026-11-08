#pragma once

// One process's UDP bytes sent and received (#3366), as the Linux helper's eBPF counter sums them
// (Linux/UdpByteCounter.h) and the broker protocol carries them (UdpBytesReply).

#include <cstdint>

namespace Platform
{

/// One process's UDP totals since the counter was loaded.
struct UdpByteCounts
{
    std::uint32_t tgid = 0; ///< The process (thread group) ID
    std::uint64_t txBytes = 0;
    std::uint64_t rxBytes = 0;

    friend bool operator==(const UdpByteCounts&, const UdpByteCounts&) = default;
};

} // namespace Platform
