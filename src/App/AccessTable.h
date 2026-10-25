#pragma once

// What Administrator adds (#2728): the Standard-versus-Administrator table the startup notice's
// "What does Administrator add?" expands, and the status-bar chip shows, one table per platform in
// code, from the 2026-10-21 inventory on the issue. Unit-tested (tests/App/test_AccessChoice.cpp),
// including that every Linux MissingPrivilege bit has a row, so a new privilege can't go unexplained.
//
// It describes Administrator as the design makes it: nothing limited. On Windows that is TaskSmack
// restarted elevated. On Linux it is the session-held helper (#2872), and the rows marked
// needsHelperCoverage are reads the helper doesn't do yet; the notice mustn't promise them until it
// does.

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
    /// Linux: Administrator gives this only once the session-held helper does the read (#2728's
    /// "Coverage" list); false where it already does.
    bool needsHelperCoverage = false;
};

inline constexpr std::array LINUX_ROWS{
    Row{.item = "Other users' processes: open files count, I/O and network",
        .standard = "Not readable",
        .administrator = "Read through the helper",
        .missingPrivilege = Platform::MissingPrivilege::READ_OTHERS,
        .needsHelperCoverage = true},
    Row{.item = "Other users' processes: PSS and USS memory", .standard = "Not readable", .administrator = "Read through the helper"},
    Row{.item = "Ending, suspending or resuming other users' processes",
        .standard = "Refused; Authenticate and retry asks each time",
        .administrator = "Done",
        .missingPrivilege = Platform::MissingPrivilege::SIGNAL_OTHERS},
    Row{.item = "Raising priority, the Realtime I/O class, other users' priority and affinity",
        .standard = "Refused; Authenticate and retry asks each time",
        .administrator = "Done",
        .missingPrivilege = Platform::MissingPrivilege::RAISE_PRIORITY},
    Row{.item = "Processes hidden by /proc's hidepid",
        .standard = "Not listed",
        .administrator = "Listed through the helper",
        .missingPrivilege = Platform::MissingPrivilege::PROCESSES_HIDDEN,
        .needsHelperCoverage = true},
    Row{.item = "System Information: serial numbers, UUIDs, SMBIOS and memory modules",
        .standard = "Hidden; Authenticate to read",
        .administrator = "Shown"},
    Row{.item = "Other users' crash reports", .standard = "Hidden; Authenticate to read", .administrator = "Shown"},
    Row{.item = "Service actions",
        .standard = "systemd asks for a password each time",
        .administrator = "Done without asking again",
        .needsHelperCoverage = true},
};

inline constexpr std::array WINDOWS_ROWS{
    Row{.item = "Network traffic per process (TCP)", .standard = "Not counted", .administrator = "Counted"},
    Row{.item = "UDP traffic per process (opt-in, Settings)", .standard = "Can't start", .administrator = "Counted when turned on"},
    Row{.item = "Elevated and other users' processes: user, command line, publisher, priority and architecture",
        .standard = "Partly unreadable",
        .administrator = "Read"},
    Row{.item = "Elevated processes: modules, open files and connections", .standard = "Access denied", .administrator = "Listed"},
    Row{.item = "Elevated processes: the Security tab", .standard = "Not readable", .administrator = "Read"},
    Row{.item = "Ending, suspending, resuming, priority and affinity of elevated or other users' processes",
        .standard = "Refused",
        .administrator = "Done"},
    Row{.item = "Memory dumps of elevated processes", .standard = "Refused", .administrator = "Written"},
    Row{.item = "Services: starting, stopping and changing them", .standard = "Refused for most services", .administrator = "Done"},
    Row{.item = "Startup entries for all users", .standard = "Can't be changed", .administrator = "Can be changed"},
    Row{.item = "Disks: the NVMe health log and partition layout", .standard = "Not read", .administrator = "Read"},
};

/// The table's last row: what is refused even as Administrator, and says so.
inline constexpr std::string_view LINUX_NEVER = "Never, even as Administrator: PID 1 (init) and kernel threads";
inline constexpr std::string_view WINDOWS_NEVER = "Never, even as Administrator: protected and critical processes, the System process "
                                                  "(PID 4), and Realtime priority";

[[nodiscard]] constexpr std::span<const Row> rowsFor(AccessChoice::AccessPlatform platform) noexcept
{
    return platform == AccessChoice::AccessPlatform::Windows ? std::span<const Row>(WINDOWS_ROWS) : std::span<const Row>(LINUX_ROWS);
}

[[nodiscard]] constexpr std::string_view neverRowFor(AccessChoice::AccessPlatform platform) noexcept
{
    return platform == AccessChoice::AccessPlatform::Windows ? WINDOWS_NEVER : LINUX_NEVER;
}

} // namespace App::AccessTable
