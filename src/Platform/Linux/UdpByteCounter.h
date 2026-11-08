#pragma once

// The helper's per-process UDP byte counter (#3366): loads bpf/udp_bytes.bpf.c, which sums each process's UDP
// bytes sent and received into a map keyed by tgid, and reads that map. The Linux side of "Count UDP traffic per
// process" (Windows counts with ETW).
//
// Loading needs root (CAP_BPF and CAP_PERFMON, or CAP_SYS_ADMIN): the helper loads at startup, before it drops its
// capabilities and installs its sandbox, and afterwards only reads the map (BPF_MAP_GET_NEXT_KEY and
// BPF_MAP_LOOKUP_ELEM on the map's fd). The program stays attached for as long as this object lives.

#include "Platform/UdpByteCounts.h"

#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>
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

/// The attempts load() makes, in order: fexit before kretprobes, and with IPv6 before without it.
[[nodiscard]] std::span<const UdpAttachAttempt> udpAttachAttempts() noexcept;

/// Whether the program in ELF section `section` (such as "fexit/udpv6_recvmsg") is loaded for `attempt`.
[[nodiscard]] bool udpProgramWanted(std::string_view section, const UdpAttachAttempt& attempt) noexcept;

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

    /// The map's file descriptor, for the helper's seccomp filter to allow the map syscalls on.
    [[nodiscard]] int mapFd() const noexcept;

  private:
    struct State;
    explicit UdpByteCounter(std::unique_ptr<State> state) noexcept;

    std::unique_ptr<State> m_State;
};

} // namespace Platform
