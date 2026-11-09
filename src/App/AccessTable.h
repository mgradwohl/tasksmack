#pragma once

// What Administrator adds (#2728): the Standard-versus-Administrator table the startup notice's
// "What does Administrator add?" expands, and the status-bar chip shows, one table per platform in
// code, from the 2026-10-21 inventory on the issue. Unit-tested (tests/App/test_AccessChoice.cpp),
// including that every Linux MissingPrivilege bit has a row, so a new privilege can't go unexplained.
//
// It describes Administrator as the design makes it: nothing limited. On Windows that is TaskSmack
// restarted elevated. On Linux it is TaskSmack started as root, "TaskSmack (Administrator)" or sudo, for
// the whole session (#3580): a Standard session is never elevated after it starts, so its column says what
// it gets, never a prompt to finish it.

#include "App/AccessChoice.h"
#include "Platform/ProcessTypes.h"

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace App::AccessTable
{

struct Row
{
    std::string_view item;          ///< What is limited
    std::string_view standard;      ///< What a Standard session gets
    std::string_view administrator; ///< What an Administrator session gets
    /// Linux: the ProcessCapabilities::missingPrivileges bit (Platform::MissingPrivilege) this row
    /// explains; 0 for a row no bit stands for.
    std::uint8_t missingPrivilege = 0;
};

inline constexpr std::array LINUX_ROWS{
    Row{
        .item = "Other users' processes: open files count, I/O and network",
        .standard = "Not readable",
        .administrator = "Read",
        .missingPrivilege = Platform::MissingPrivilege::READ_OTHERS,
    },
    Row{.item = "Other users' processes: PSS and USS memory", .standard = "Not readable", .administrator = "Read"},
    Row{
        .item = "Ending, suspending or resuming other users' processes",
        .standard = "Refused",
        .administrator = "Done",
        .missingPrivilege = Platform::MissingPrivilege::SIGNAL_OTHERS,
    },
    Row{
        .item = "Raising priority, real-time I/O and policies, leaving the Idle policy, other users' priority, affinity and policy",
        .standard = "Refused",
        .administrator = "Done",
        .missingPrivilege = Platform::MissingPrivilege::RAISE_PRIORITY,
    },
    Row{
        .item = "Processes hidden by /proc's hidepid",
        .standard = "Not listed",
        .administrator = "Listed",
        .missingPrivilege = Platform::MissingPrivilege::PROCESSES_HIDDEN,
    },
    Row{.item = "System Information: serial numbers, UUIDs, SMBIOS and memory modules", .standard = "Hidden", .administrator = "Shown"},
    Row{.item = "Other users' crash reports", .standard = "Hidden", .administrator = "Shown"},
    Row{.item = "Service actions", .standard = "Refused unless a polkit rule allows them", .administrator = "Done"},
    Row{.item = "UDP traffic per process (opt-in, Settings)", .standard = "Not counted", .administrator = "Counted when turned on"},
};

inline constexpr std::array WINDOWS_ROWS{
    Row{.item = "Network traffic per process (TCP)", .standard = "Not counted", .administrator = "Counted"},
    Row{.item = "UDP traffic per process (opt-in, Settings)", .standard = "Can't start", .administrator = "Counted when turned on"},
    Row{
        .item = "Elevated and other users' processes: user, command line, publisher, priority and architecture",
        .standard = "Partly unreadable",
        .administrator = "Read",
    },
    Row{.item = "Elevated processes: modules, open files and connections", .standard = "Access denied", .administrator = "Listed"},
    Row{.item = "Elevated processes: the Security tab", .standard = "Not readable", .administrator = "Read"},
    Row{
        .item = "Ending, suspending, resuming, priority and affinity of elevated or other users' processes",
        .standard = "Refused",
        .administrator = "Done",
    },
    Row{.item = "Realtime priority", .standard = "Windows sets High instead", .administrator = "Set"},
    Row{.item = "Memory dumps of elevated processes", .standard = "Refused", .administrator = "Written"},
    Row{.item = "Services: starting, stopping and changing them", .standard = "Refused for most services", .administrator = "Done"},
    Row{.item = "Startup entries for all users", .standard = "Can't be changed", .administrator = "Can be changed"},
    Row{.item = "Disks: the NVMe health log and partition layout", .standard = "Not read", .administrator = "Read"},
};

/// The table's last row: what is refused even as Administrator, and says so.
inline constexpr std::string_view LINUX_NEVER = "Never, even as Administrator: PID 1 (init) and kernel threads";
inline constexpr std::string_view WINDOWS_NEVER =
    "Never, even as Administrator: protected and critical processes, and the System process (PID 4)";

[[nodiscard]] constexpr std::span<const Row> rowsFor(AccessChoice::AccessPlatform platform) noexcept
{
    return platform == AccessChoice::AccessPlatform::Windows ? std::span<const Row>(WINDOWS_ROWS) : std::span<const Row>(LINUX_ROWS);
}

[[nodiscard]] constexpr std::string_view neverRowFor(AccessChoice::AccessPlatform platform) noexcept
{
    return platform == AccessChoice::AccessPlatform::Windows ? WINDOWS_NEVER : LINUX_NEVER;
}

} // namespace App::AccessTable
