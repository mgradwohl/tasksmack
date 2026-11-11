#pragma once

// An Administrator session's capabilities (#3621): TaskSmack started as root keeps, in its effective and permitted
// sets, only the capabilities it uses, so a bug that doesn't exec can't load a module, write raw I/O, set the clock
// and so on. The bounding set and the root exec rule are left alone: a program a root TaskSmack execs ("Run as
// administrator", #3065) gets full root back, as the maintainer decided (D4, #3580).
//
// Capabilities are per thread: the drop runs first thing in runApp(), before any other thread starts, and every
// thread made later inherits the narrowed sets.

#include <array>
#include <cstdint>

namespace Platform::RootCapabilities
{

/// capabilities(7) bit numbers, spelled out so this builds against kernel headers older than CAP_PERFMON/CAP_BPF.
inline constexpr unsigned CHOWN = 0;           ///< fchown of the invoking user's settings to that user (#3592)
inline constexpr unsigned DAC_OVERRIDE = 1;    ///< the user's own files, display and delegated cgroups
inline constexpr unsigned DAC_READ_SEARCH = 2; ///< other users' /proc files
inline constexpr unsigned FOWNER = 3;          ///< the ACL and mode set on the user's settings after the chown
inline constexpr unsigned KILL = 5;            ///< End task and signals
inline constexpr unsigned SETGID = 6;          ///< links, folders and Run new task as the invoking user (#2461)
inline constexpr unsigned SETUID = 7;          ///< the same
inline constexpr unsigned SYS_PTRACE = 19;     ///< other users' /proc, strace and gcore, Close connection
inline constexpr unsigned SYS_NICE = 23;       ///< priority, affinity, real-time policies, the Realtime I/O class
inline constexpr unsigned SYS_RESOURCE = 24;   ///< limits, and libbpf's memlock raise
inline constexpr unsigned SYSLOG = 34;         ///< kernel module load addresses
inline constexpr unsigned PERFMON = 38;        ///< loading the eBPF UDP counter (#3366)
inline constexpr unsigned BPF = 39;            ///< the same

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
