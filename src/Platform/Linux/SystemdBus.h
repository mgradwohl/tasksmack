#pragma once

// A minimal client for system-bus services through sd-bus (libsystemd): systemd's boot timestamps
// (#1525) and udisks2's drive SMART data (#1631).
// Optional at build time: without libsystemd (TASKSMACK_HAS_SDBUS undefined) every read reports that
// the build can't, so callers say "unavailable" rather than guess. Reads are synchronous and quick;
// the System Information page makes them on its worker thread, never per frame.

#include "Platform/Linux/LinuxDiskSmart.h"

#include <cstdint>
#include <string>

namespace Platform::SystemdBus
{

/// The systemd Manager's boot timestamps (LinuxBootTimes::ManagerTimestamps), read in one connection.
struct BootTimestampsRead
{
    bool ok = false;
    std::string error; ///< Why not, when !ok: no sd-bus in this build, no system bus, or the call's error.
    std::uint64_t firmware = 0;
    std::uint64_t loader = 0;
    std::uint64_t initrd = 0;
    std::uint64_t userspace = 0;
    std::uint64_t finish = 0;
};

/// Whether this build was linked with libsystemd.
[[nodiscard]] bool built() noexcept;

/// Reads org.freedesktop.systemd1.Manager's Firmware/Loader/InitRD/Userspace/FinishTimestampMonotonic
/// from the system bus. Unprivileged.
[[nodiscard]] BootTimestampsRead readBootTimestamps();

/// A reader of each disk's SMART data from udisks2 (org.freedesktop.UDisks2), sharing one system-bus
/// connection, opened on its first call. Unprivileged: udisks2 serves its cached data to anyone.
[[nodiscard]] LinuxDiskSmart::SmartReader makeDriveSmartReader();

} // namespace Platform::SystemdBus
