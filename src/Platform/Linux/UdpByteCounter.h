#pragma once

// The per-process UDP byte counter (#3366): loads bpf/udp_bytes.bpf.c, which sums each process's UDP bytes sent and
// received into a map keyed by tgid, and reads that map. The Linux side of "Count UDP traffic per process" (Windows
// counts with ETW), loaded by TaskSmack started as root (LinuxUdpTrafficCollector, #3580).
//
// Loading needs root (CAP_BPF and CAP_PERFMON, or CAP_SYS_ADMIN); afterwards it only reads the map
// (BPF_MAP_GET_NEXT_KEY and BPF_MAP_LOOKUP_ELEM on the map's fd). The program stays attached for as long as this
// object lives.

#include "Platform/UdpByteCounts.h"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace Platform
{

/// How the program is attached: fexit where the kernel has BTF trampolines and bpf_get_func_ret (5.17+),
/// kretprobes otherwise.
enum class UdpAttachMode : std::uint8_t
{
    Fexit,
    Kretprobe,
};

/// One way of loading the program: an attach mode, and whether the IPv6 functions are hooked too. Where IPv6
/// is off or its module isn't loaded, udpv6_sendmsg and udpv6_recvmsg can't be attached, and IPv4 is still
/// counted.
struct UdpAttachAttempt
{
    UdpAttachMode mode = UdpAttachMode::Fexit;
    bool ipv6 = true;

    friend bool operator==(const UdpAttachAttempt&, const UdpAttachAttempt&) = default;
};

/// UdpByteCounter::read()'s error for a walk that ran out of steps (#4265): the map changed too fast once, not a
/// counter that stopped working, so the collector starts over from a baseline without reporting a problem (#4631).
inline constexpr std::string_view UDP_READ_INCOMPLETE = "the UDP byte counter's map changed too fast to read in full";

/// The walk of the tgid-keyed LRU map that UdpByteCounter::read() makes (#4265), over callables so the tests can fake
/// a map whose cursor ages out. A key that aged out between two calls makes the next start over from the first, so
/// the walk is bounded (at most @p entries processes, twice that in steps) and skips a tgid it already has.
namespace UdpMapWalk
{

/// Why a walk gave no reading.
struct Failure
{
    int kernelError = 0; ///< bpf_map_get_next_key()'s -errno; 0 when the walk ran out of steps before the last key
};

/// @p getNextKey(previous, next) is bpf_map_get_next_key() (previous null for the first key: 0, -ENOENT after the
/// last, or -errno); @p lookup(tgid, counts) is bpf_map_lookup_elem() into a UdpByteCounts (true when found). Only a
/// walk that reached the last key, or holds a full map's worth of processes, is a reading: one that ran out of steps
/// (the cursor aged out again and again) would miss processes, whose lifetime bytes the next reading would then
/// credit to one interval (#4265).
template<typename GetNextKey, typename Lookup>
[[nodiscard]] std::expected<std::vector<UdpByteCounts>, Failure>
walk(std::size_t entries, const GetNextKey& getNextKey, const Lookup& lookup)
{
    std::vector<UdpByteCounts> counts;
    std::unordered_set<std::uint32_t> seen;
    std::uint32_t key = 0;
    std::uint32_t next = 0;
    const std::uint32_t* previous = nullptr; // nullptr asks for the first key
    for (std::size_t step = 0; step < 2 * entries; ++step)
    {
        if (counts.size() >= entries)
        {
            return counts; // every entry the map can hold
        }
        if (const int error = getNextKey(previous, next); error != 0)
        {
            if (error == -ENOENT)
            {
                return counts; // the last key
            }
            return std::unexpected(Failure{.kernelError = error});
        }
        UdpByteCounts process{.tgid = next, .txBytes = 0, .rxBytes = 0};
        // An entry can age out of the LRU map between the two calls; skip it.
        if (seen.insert(next).second && lookup(next, process))
        {
            counts.push_back(process);
        }
        key = next;
        previous = &key;
    }
    if (counts.size() >= entries)
    {
        return counts;
    }
    return std::unexpected(Failure{.kernelError = 0});
}

} // namespace UdpMapWalk

/// The attempts load() makes, in order: fexit before kretprobes, and with IPv6 before without it.
[[nodiscard]] std::span<const UdpAttachAttempt> udpAttachAttempts() noexcept;

/// Whether the program in ELF section `section` (such as "fexit/udpv6_recvmsg") is loaded for `attempt`.
[[nodiscard]] bool udpProgramWanted(std::string_view section, const UdpAttachAttempt& attempt) noexcept;

/// How a failed attempt's reason quotes libbpf's warnings (the verifier's log among them), for the tests (#3374).
namespace UdpLibbpfLog
{

/// The most of libbpf's warnings one attempt keeps, and the most of their end a failure's reason quotes.
inline constexpr std::size_t CAPTURE_BYTES = 16384;
inline constexpr std::size_t QUOTED_BYTES = 800;

/// Adds @p line, one of libbpf's warnings, to @p capture, keeping only its last CAPTURE_BYTES: the end says why.
void keep(std::string& capture, std::string_view line);

/// @p reason, with the last QUOTED_BYTES of @p captured (trailing newlines dropped), or @p reason alone when libbpf
/// said nothing.
[[nodiscard]] std::string withLog(std::string reason, std::string_view captured);

/// One load attempt's capture of libbpf's warnings (#3634). The text lives in this thread's own buffer, never in the
/// caller's, so no address of the caller's storage is kept anywhere. While one is open, note() keeps what libbpf says;
/// opening one empties the buffer, and closing it stops note() from adding to it.
class Capture
{
  public:
    Capture() noexcept;
    ~Capture();
    Capture(const Capture&) = delete;
    Capture& operator=(const Capture&) = delete;
    Capture(Capture&&) = delete;
    Capture& operator=(Capture&&) = delete;

    /// What libbpf has said on this thread since this capture opened, its last CAPTURE_BYTES.
    [[nodiscard]] const std::string& text() const noexcept;
};

/// Adds @p line, one of libbpf's warnings, to this thread's open Capture (keep()); nothing when none is open.
void note(std::string_view line);

} // namespace UdpLibbpfLog

class UdpByteCounter
{
  public:
    /// Loads and attaches the program, trying each of udpAttachAttempts() until one works. Needs root. The error
    /// names what the last attempt couldn't do.
    [[nodiscard]] static std::expected<UdpByteCounter, std::string> load();
    /// The same with only @p attempts, in order: for the tests, which load each mode on its own.
    [[nodiscard]] static std::expected<UdpByteCounter, std::string> load(std::span<const UdpAttachAttempt> attempts);

    ~UdpByteCounter();
    UdpByteCounter(const UdpByteCounter&) = delete;
    UdpByteCounter& operator=(const UdpByteCounter&) = delete;
    UdpByteCounter(UdpByteCounter&& other) noexcept;
    UdpByteCounter& operator=(UdpByteCounter&& other) noexcept;

    /// Every process's totals in the map, in no particular order, or why the kernel refused reading it.
    [[nodiscard]] std::expected<std::vector<UdpByteCounts>, std::string> read() const;

    /// How the program attached.
    [[nodiscard]] UdpAttachAttempt attempt() const noexcept;

    /// The map's file descriptor: the one BPF descriptor left open once the program is attached.
    [[nodiscard]] int mapFd() const noexcept;

  private:
    struct State;
    explicit UdpByteCounter(std::unique_ptr<State> state) noexcept;

    std::unique_ptr<State> m_State;
};

} // namespace Platform
