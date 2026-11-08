#pragma once

// A UDP collector's per-process counters as socket-traffic samples (#3366): each process's UDP bytes become one
// synthetic, cumulative "connection", which Domain::SocketTrafficAccumulator folds in like a TCP socket's. Linux's
// process probe adds them from the helper's eBPF counter. Windows' does the same from ETW with its own copy in
// Windows/WindowsEtwNetMath.h (EtwNet::appendUdpSamples), whose key this one matches.

#include "Platform/IUdpTrafficCollector.h"
#include "Platform/ProcessTypes.h"

#include <cstdint>
#include <span>
#include <vector>

namespace Platform::UdpTraffic
{

/// The socket key of a process's UDP counters: bit 63 set (a Linux socket inode or a Windows TCP key never has it, so
/// the two never collide), the counter's generation in bits 32-62, the PID below. Generation 0 is Windows' key.
[[nodiscard]] constexpr std::uint64_t socketKey(std::uint32_t pid, std::uint32_t generation = 0) noexcept
{
    constexpr std::uint64_t GENERATION_MASK = 0x7FFF'FFFFU;
    return (std::uint64_t{1} << 63U) | ((generation & GENERATION_MASK) << 32U) | pid;
}

/// Adds each process's UDP counters to @p sockets as one connection keyed socketKey(pid, generation), owned by that PID
/// alone (start time 0): the counters are keyed by PID, not by a process's identity.
inline void appendSamples(std::vector<SocketTrafficSample>& sockets, std::span<const UdpProcessTraffic> udp)
{
    sockets.reserve(sockets.size() + udp.size());
    for (const UdpProcessTraffic& process : udp)
    {
        sockets.push_back(SocketTrafficSample{
            .key = socketKey(process.pid, process.generation),
            .pid = static_cast<std::int32_t>(process.pid),
            .ownerStartTimeTicks = 0,
            .bytesReceived = process.bytesReceived,
            .bytesSent = process.bytesSent,
            .readable = process.readable,
        });
    }
}

} // namespace Platform::UdpTraffic
