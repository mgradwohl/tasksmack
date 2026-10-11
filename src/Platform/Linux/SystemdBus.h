#pragma once

// A minimal client for system-bus services through sd-bus (libsystemd): systemd's boot timestamps
// (#1525), udisks2's drive SMART data (#1631), NetworkManager's Wi-Fi links and systemd-resolved's
// per-adapter DNS settings (#1518).
// Optional at build time: without libsystemd (TASKSMACK_HAS_SDBUS undefined) every read reports that
// the build can't, so callers say "unavailable" rather than guess. Reads are synchronous and quick;
// the System Information page makes them on its worker thread, never per frame.

#include "Platform/Linux/LinuxDiskSmart.h"
#include "Platform/Linux/LinuxNetworkAdapters.h"

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

/// Longest any one call on the system bus may take before it is abandoned (#1689): sd-bus's own default
/// is 25 s, and a read makes several calls -- a few per disk -- so a hung service would hold the page's
/// worker read for minutes.
inline constexpr std::uint64_t CALL_TIMEOUT_USEC = 2'000'000;

/// Whether this build was linked with libsystemd.
[[nodiscard]] bool built() noexcept;

/// Reads org.freedesktop.systemd1.Manager's Firmware/Loader/InitRD/Userspace/FinishTimestampMonotonic
/// from the system bus. Unprivileged.
[[nodiscard]] BootTimestampsRead readBootTimestamps();

/// A reader of each disk's SMART data from udisks2 (org.freedesktop.UDisks2), sharing one system-bus
/// connection, opened on its first call. Unprivileged: udisks2 serves its cached data to anyone.
[[nodiscard]] LinuxDiskSmart::SmartReader makeDriveSmartReader();

/// A reader of each Wi-Fi adapter's link from NetworkManager (org.freedesktop.NetworkManager): the
/// active access point's SSID and frequency and the adapter's bitrate. Shares one system-bus
/// connection, opened on its first call; once NetworkManager is found not running, or doesn't answer
/// in time, the rest of the read doesn't ask again. Unprivileged.
[[nodiscard]] LinuxNetworkAdapters::WifiLinkReader makeWifiLinkReader();

/// A reader of each adapter's own DNS servers and search domains from systemd-resolved
/// (org.freedesktop.resolve1), sharing one system-bus connection, opened on its first call; once
/// resolved is found not running, or doesn't answer in time, the rest of the read doesn't ask again.
/// Unprivileged.
[[nodiscard]] LinuxNetworkAdapters::LinkDnsReader makeLinkDnsReader();

} // namespace Platform::SystemdBus
