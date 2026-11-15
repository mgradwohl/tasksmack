#pragma once

// An Administrator session's capabilities (#3621): TaskSmack started as root keeps, in its effective and permitted
// sets, only the capabilities it uses, so a bug that doesn't exec can't load a module, write raw I/O, set the clock
// and so on. The bounding set and the root exec rule are left alone: a program a root TaskSmack execs ("Run as
// administrator", #3065) gets full root back, as the maintainer decided (D4, #3580).
//
// Capabilities are per thread: the drop runs first thing in runApp(), before any other thread starts, and every
// thread made later inherits the narrowed sets.

#include "Platform/Linux/CapabilityBits.h"

#include <array>
#include <cstdint>

namespace Platform::RootCapabilities
{

/// The capabilities(7) bit numbers, from CapabilityBits.h (#3770), with what each is kept for.
inline constexpr unsigned CHOWN = CapabilityBits::CHOWN;               ///< fchown of the invoking user's settings to that user (#3592)
inline constexpr unsigned DAC_OVERRIDE = CapabilityBits::DAC_OVERRIDE; ///< the user's own files, display and delegated cgroups
inline constexpr unsigned DAC_READ_SEARCH = CapabilityBits::DAC_READ_SEARCH; ///< other users' /proc files
inline constexpr unsigned FOWNER = CapabilityBits::FOWNER;             ///< the ACL and mode set on the user's settings after the chown
inline constexpr unsigned KILL = CapabilityBits::KILL;                 ///< End task and signals
inline constexpr unsigned SETGID = CapabilityBits::SETGID;             ///< links, folders and Run new task as the invoking user (#2461)
inline constexpr unsigned SETUID = CapabilityBits::SETUID;             ///< the same
inline constexpr unsigned SYS_PTRACE = CapabilityBits::SYS_PTRACE;     ///< other users' /proc, strace and gcore, Close connection
inline constexpr unsigned SYS_NICE = CapabilityBits::SYS_NICE;         ///< priority, affinity, real-time policies, the Realtime I/O class
inline constexpr unsigned SYS_RESOURCE = CapabilityBits::SYS_RESOURCE; ///< limits, and libbpf's memlock raise
inline constexpr unsigned SYSLOG = CapabilityBits::SYSLOG;             ///< kernel module load addresses
inline constexpr unsigned PERFMON = CapabilityBits::PERFMON;           ///< loading the eBPF UDP counter (#3366)
inline constexpr unsigned BPF = CapabilityBits::BPF;                   ///< the same

/// What an Administrator session keeps. CAP_SYS_ADMIN isn't among them (maintainer, 2026-11-11): kernels before 5.8,
/// which need it in place of CAP_BPF and CAP_PERFMON, don't count UDP per process as root.
inline constexpr std::array<unsigned, 13> KEPT{
    CHOWN,
    DAC_OVERRIDE,
    DAC_READ_SEARCH,
    FOWNER,
    KILL,
    SETGID,
    SETUID,
    SYS_PTRACE,
    SYS_NICE,
    SYS_RESOURCE,
    SYSLOG,
    PERFMON,
    BPF,
};

/// KEPT as a capability mask.
[[nodiscard]] constexpr std::uint64_t keptMask() noexcept
{
    std::uint64_t mask = 0;
    for (const unsigned bit : KEPT)
    {
        mask |= std::uint64_t{1} << bit;
    }
    return mask;
}

/// A thread's three capability sets, as masks.
struct Sets
{
    std::uint64_t effective = 0;
    std::uint64_t permitted = 0;
    std::uint64_t inheritable = 0;

    friend bool operator==(const Sets&, const Sets&) = default;
};

/// @p current with its effective and permitted sets narrowed to keptMask(); the inheritable set is left as it is.
[[nodiscard]] constexpr Sets narrowed(Sets current) noexcept
{
    current.effective &= keptMask();
    current.permitted &= keptMask();
    return current;
}

/// What dropUnused() did.
struct DropResult
{
    bool root = false;    ///< Whether the effective uid is 0; nothing is changed otherwise
    bool dropped = false; ///< Whether the narrowed sets were set
    int error = 0;        ///< errno from capget/capset when they failed
    int threads = 0;      ///< This process's threads at the time: more than 1 means some kept the full sets
    Sets before;
    Sets after;
};

/// Narrows the calling thread's sets to narrowed() when TaskSmack runs as root; does nothing otherwise. Call it
/// before any other thread starts. Never throws.
[[nodiscard]] DropResult dropUnused() noexcept;

} // namespace Platform::RootCapabilities
