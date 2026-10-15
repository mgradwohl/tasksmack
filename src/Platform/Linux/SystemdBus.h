#pragma once

// A minimal client for system-bus services through sd-bus (libsystemd): systemd's boot timestamps
// (#1525), udisks2's drive SMART data (#1631), NetworkManager's Wi-Fi links and DHCP leases,
// systemd-networkd's DHCP leases and systemd-resolved's per-adapter DNS settings (#1518).
// Optional at build time: without libsystemd (TASKSMACK_HAS_SDBUS undefined) every read reports that
// the build can't, so callers say "unavailable" rather than guess. Reads are synchronous and quick;
// the System Information page makes them on its worker thread, never per frame.

#include "Platform/IServiceActions.h"
#include "Platform/IServiceProbe.h"
#include "Platform/Linux/LinuxDiskSmart.h"
#include "Platform/Linux/LinuxNetworkAdapters.h"
#include "Platform/Linux/SystemdServices.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

/// systemd's services, as ServicesRead holds them (#2006).
struct ServicesRead
{
    bool ok = false;
    std::string error; ///< Why not, when !ok: no sd-bus in this build, no system bus, systemd not answering.
    SystemdServices::Listing listing;
};

/// How long readServices() reuses a ListUnitFiles answer (#2125): systemd walks every unit directory on
/// disk to give it, and installed files and their enablement rarely change. A start type TaskSmack sets
/// itself shows at once (LinuxServiceProbe forgets the cache after an action); one changed elsewhere
/// (systemctl enable) shows within this long.
inline constexpr std::chrono::seconds UNIT_FILES_TTL{30};

/// The last ListUnitFiles answer readServices() was given, and when.
struct UnitFileCache
{
    std::vector<std::pair<std::string, std::string>> unitFiles;
    std::optional<std::chrono::steady_clock::time_point> readAt; ///< Empty: nothing cached (or forgotten)

    void forget() noexcept
    {
        readAt.reset();
    }
};

/// Reads org.freedesktop.systemd1.Manager's ListUnits (the loaded services and their state) and
/// ListUnitFiles (every installed service and whether it is enabled), then each running service's
/// MainPID, in one connection. Unprivileged. With @p cache, a ListUnitFiles answer younger than
/// UNIT_FILES_TTL at @p now is reused rather than asked again. A MainPID read that times out ends the
/// MainPID reads for this call (#2125): a stalled systemd costs one call timeout, not one per service.
[[nodiscard]] ServicesRead readServices(UnitFileCache* cache = nullptr,
                                        std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());

/// How long a start or stop waits for the unit to get there, and how often it looks (#2006).
struct UnitWait
{
    std::chrono::milliseconds timeout{10'000}; ///< Windows' bounded wait, per state change
    std::chrono::milliseconds poll{100};
};

/// Starts the unit @p name (Manager.StartUnit, mode "replace") and waits until it is running, it
/// fails, @p wait runs out or @p stopToken is stopped (#2006). Never asks polkit to prompt: refused
/// unprivileged, it says "Requires administrator".
[[nodiscard]] ServiceActionResult startUnit(std::string_view name, const std::stop_token& stopToken, UnitWait wait = {});

/// Stops the unit @p name and waits until it has stopped, unless a running unit would go down with it
/// (its RequiredBy, BoundBy, ConsistsOf or RequisiteOf, #2124): then nothing is sent and the refusal
/// names them, as on Windows (#2006).
[[nodiscard]] ServiceActionResult stopUnit(std::string_view name, const std::stop_token& stopToken, UnitWait wait = {});

/// Sets the start type of @p name's unit file: Automatic enables it, Manual disables it, Disabled masks
/// it, unmasking first when moving off Disabled; then reloads systemd, as systemctl does (#2006).
/// Automatic (delayed) and the read-only types are refused.
[[nodiscard]] ServiceActionResult setUnitStartType(std::string_view name, ServiceStartType startType);

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

/// A reader of each adapter's IPv4 DHCP lease from NetworkManager (its Device's Dhcp4Config Options),
/// sharing one system-bus connection, opened on its first call; once NetworkManager is found not
/// running, or doesn't answer in time, the rest of the read doesn't ask again. Unprivileged.
[[nodiscard]] LinuxNetworkAdapters::DhcpLeaseReader makeDhcpLeaseReader();

/// A reader of each adapter's IPv4 DHCP lease from systemd-networkd (org.freedesktop.network1): the
/// link's Describe() JSON (NetworkdLinkJson). The calls don't start networkd if it isn't running
/// (no D-Bus auto-start). One connection per read; once networkd is found absent or unresponsive, the
/// rest of the read doesn't ask again. Unprivileged.
[[nodiscard]] LinuxNetworkAdapters::IndexedDhcpLeaseReader makeNetworkdLeaseReader();

} // namespace Platform::SystemdBus
