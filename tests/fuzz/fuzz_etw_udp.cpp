#include "Platform/IUdpTrafficCollector.h"
#include "Platform/ProcessTypes.h"
#include "Platform/UdpTrafficSamples.h"
#include "Platform/Windows/WindowsEtwNetMath.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

// The UDP event trace's decoding (#1492) on arbitrary bytes (#4358): the input is a run of Microsoft-Windows-Kernel-Network
// events, each a little-endian event ID and payload length, then the payload (EVENT_RECORD::UserData), cut short where
// the input ends. Each is decoded by EtwNet::parseUdpEvent() and summed by UdpByteCounters, sampled every few events as
// the collector does, and the sums become socket-traffic samples. A decoded event must be a UDP one with its whole
// template and a PID; every sample must be exactly what was added since that process was last forgotten. The input is
// also read as a session name, which any process can give a trace session: one TaskSmack's own must name its owner back.
namespace
{

[[nodiscard]] std::uint32_t le32(std::span<const std::byte> bytes)
{
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < sizeof(value); ++i)
    {
        value |= static_cast<std::uint32_t>(std::to_integer<std::uint32_t>(bytes[i]) << (8U * i));
    }
    return value;
}

struct Totals
{
    std::uint64_t sent = 0;
    std::uint64_t received = 0;
};

/// A sample of @p counters checked against the sums @p expected holds; the processes it forgot are forgotten there too.
void checkSample(Platform::EtwNet::UdpByteCounters& counters, std::map<std::uint32_t, Totals>& expected, std::uint32_t idleSamples)
{
    const std::vector<Platform::UdpProcessTraffic> sample = counters.sample(idleSamples);
    if (sample.size() != counters.size())
    {
        __builtin_trap();
    }
    std::map<std::uint32_t, Totals> kept;
    for (const Platform::UdpProcessTraffic& process : sample)
    {
        const auto it = expected.find(process.pid);
        if (it == expected.end() || it->second.sent != process.bytesSent || it->second.received != process.bytesReceived ||
            !kept.emplace(process.pid, it->second).second)
        {
            __builtin_trap();
        }
    }
    expected = std::move(kept);
    std::vector<Platform::SocketTrafficSample> sockets;
    Platform::UdpTraffic::appendSamples(sockets, sample);
    if (sockets.size() != sample.size())
    {
        __builtin_trap();
    }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) // NOLINT(readability-identifier-naming)
{
    namespace EtwNet = Platform::EtwNet;
    constexpr std::size_t EVENT_HEADER = 4; // event ID and payload length, two little-endian words
    constexpr std::size_t EVENTS_PER_SAMPLE = 8;
    constexpr std::uint32_t IDLE_SAMPLES = 3; // forgets within one input, unlike the collector's 120

    std::span<const std::byte> rest = std::as_bytes(std::span<const std::uint8_t>(data, size));
    EtwNet::UdpByteCounters counters;
    std::map<std::uint32_t, Totals> expected;
    std::size_t events = 0;
    while (rest.size() >= EVENT_HEADER)
    {
        const auto eventId = static_cast<std::uint16_t>(std::to_integer<unsigned>(rest[0]) | (std::to_integer<unsigned>(rest[1]) << 8U));
        const std::size_t length = std::to_integer<std::size_t>(rest[2]) | (std::to_integer<std::size_t>(rest[3]) << 8U);
        rest = rest.subspan(EVENT_HEADER);
        const std::span<const std::byte> payload = rest.first(length < rest.size() ? length : rest.size());
        rest = rest.subspan(payload.size());

        if (const std::optional<EtwNet::UdpPacket> packet = EtwNet::parseUdpEvent(eventId, payload))
        {
            const std::optional<EtwNet::UdpDirection> direction = EtwNet::udpEventDirection(eventId);
            if (!direction.has_value() || *direction != packet->direction || payload.size() < EtwNet::udpPayloadSize(eventId) ||
                packet->pid == 0 || packet->pid != le32(payload.subspan(EtwNet::PID_OFFSET)) ||
                packet->bytes != le32(payload.subspan(EtwNet::SIZE_OFFSET)))
            {
                __builtin_trap();
            }
            counters.add(*packet);
            Totals& totals = expected[packet->pid];
            (packet->direction == EtwNet::UdpDirection::Sent ? totals.sent : totals.received) += packet->bytes;
        }
        if (++events % EVENTS_PER_SAMPLE == 0)
        {
            checkSample(counters, expected, IDLE_SAMPLES);
        }
    }
    checkSample(counters, expected, IDLE_SAMPLES);

    const std::string_view name(reinterpret_cast<const char*>(data), size);
    if (const std::optional<EtwNet::SessionOwner> owner = EtwNet::parseSessionName(name))
    {
        if (EtwNet::parseSessionName(EtwNet::sessionName(*owner)) != owner)
        {
            __builtin_trap();
        }
        static_cast<void>(EtwNet::shouldStopSession(*owner, EtwNet::SessionOwner{}, EtwNet::OwnerLiveness::Unknown));
    }
    return 0;
}
