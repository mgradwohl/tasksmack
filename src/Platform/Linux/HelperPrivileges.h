#pragma once

// What tasksmack-helper keeps of root (#2640, #2752). pkexec starts it as root with every capability; once
// its peer check passes it stops being root -- it runs as nobody, with no supplementary groups -- and keeps
// only the capabilities the broker protocol's requests need, never able to regain the rest:
//   CAP_KILL             signal another user's process (Signal)
//   CAP_SYS_NICE         renice, ioprio_set (the Realtime class too) and sched_setaffinity on another
//                        user's process (SetPriority, SetIoPriority, SetAffinity)
//   CAP_SYS_PTRACE       ptrace read access: another user's environ, maps, fd links, fdinfo and
//                        smaps_rollup (ReadEnvironment, ReadModules, ReadOpenFiles, ReadConnections,
//                        ReadMemoryRollup)
//   CAP_DAC_READ_SEARCH  read the journal and core files whatever their owner and mode (ReadCrashes)
// CAP_DAC_READ_SEARCH also reads the root-only firmware files (ReadFirmwareFile), so nothing needs UID 0.
// Not being UID 0 means owning no root-owned file: code run in the helper by a bug can't rewrite
// /etc/passwd, a unit file or anything in /etc/cron.d, which root may whatever its capabilities.
// Everything else -- CAP_SYS_ADMIN, CAP_SYS_MODULE, CAP_SETUID, CAP_NET_ADMIN, CAP_DAC_OVERRIDE and the
// rest -- leaves the bounding, permitted, effective, inheritable and ambient sets, the process is made
// non-dumpable, and no_new_privs is set, so no exec can bring any of it back. nobody is shared: another
// process running as nobody can signal or renice the helper, at worst ending TaskSmack's session (which
// starts a new one on the next action), but can't ptrace it or read its memory, being neither able to
// match its capabilities nor dump it.

#include "Platform/Linux/ProcPrivileges.h"

#include <cstdint>

#include <sys/types.h>

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

/// Who the helper runs as once it stops being root (#2752).
struct Identity
{
    uid_t uid = 0;
    gid_t gid = 0;
};

/// nobody and nogroup, by number: 65534 on every distribution TaskSmack ships for. Fixed rather than
/// looked up, so no NSS module (sss, ldap, systemd-userdb) runs inside the still fully privileged helper,
/// and no directory can map "nobody" to a real user. setresuid() needs no passwd entry.
inline constexpr Identity NOBODY{.uid = 65534, .gid = 65534};

/// Drop to KEPT_CAPABILITIES and set no_new_privs (#2640); if any of its real, effective or saved user or
/// group IDs is 0, also become @p runAs, with no supplementary groups and not dumpable (#2752) -- or fail,
/// so the helper never serves as root, even started as root without the capabilities to stop being it.
/// @p runAs must not be root's (UID or GID 0): that fails at once, before anything changes. Call once,
/// before serving any request and before starting any thread (capabilities and IDs are per thread).
[[nodiscard]] DropResult dropToKeptCapabilities(Identity runAs) noexcept;

} // namespace Platform::HelperPrivileges
