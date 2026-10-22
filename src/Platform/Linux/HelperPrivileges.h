#pragma once

// What tasksmack-helper keeps of root (#2640). pkexec starts it as root with every capability; once its
// peer check passes it keeps only what the broker protocol's requests need and can never regain the rest:
//   CAP_KILL             signal another user's process (Signal)
//   CAP_SYS_NICE         renice, ioprio_set (the Realtime class too) and sched_setaffinity on another
//                        user's process (SetPriority, SetIoPriority, SetAffinity)
//   CAP_SYS_PTRACE       ptrace read access: another user's environ, maps, fd links, fdinfo and
//                        smaps_rollup (ReadEnvironment, ReadModules, ReadOpenFiles, ReadConnections,
//                        ReadMemoryRollup)
//   CAP_DAC_READ_SEARCH  read the journal and core files whatever their owner and mode (ReadCrashes)
// The helper stays UID 0, so the root-owned firmware files (ReadFirmwareFile) stay readable as their
// owner. Everything else -- CAP_SYS_ADMIN, CAP_SYS_MODULE, CAP_SETUID, CAP_NET_ADMIN, CAP_DAC_OVERRIDE
// and the rest -- leaves the bounding, permitted, effective, inheritable and ambient sets, and
// no_new_privs is set, so no exec can bring any of it back.

#include "Platform/Linux/ProcPrivileges.h"

#include <cstdint>

namespace Platform::HelperPrivileges
{

/// The capabilities the helper keeps, as a capability.h bit mask.
inline constexpr std::uint64_t KEPT_CAPABILITIES =
    (std::uint64_t{1} << ProcPrivileges::CAP_KILL_BIT) | (std::uint64_t{1} << ProcPrivileges::CAP_SYS_NICE_BIT) |
    (std::uint64_t{1} << ProcPrivileges::CAP_SYS_PTRACE_BIT) | (std::uint64_t{1} << ProcPrivileges::CAP_DAC_READ_SEARCH_BIT);

/// What dropToKeptCapabilities() did.
enum class DropResult : std::uint8_t
{
    Dropped,       ///< Only KEPT_CAPABILITIES are left (of those it had), in every set, and no_new_privs is on
    NotPrivileged, ///< It had no capabilities to drop (run directly as a user, as the tests do); no_new_privs is on
    Failed,        ///< A step failed: the helper must not serve
};

/// Drop to KEPT_CAPABILITIES and set no_new_privs (#2640). Call once, before serving any request and
/// before starting any thread (capabilities are per thread).
[[nodiscard]] DropResult dropToKeptCapabilities() noexcept;

} // namespace Platform::HelperPrivileges
