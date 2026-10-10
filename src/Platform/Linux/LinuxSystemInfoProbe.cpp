#include "LinuxSystemInfoProbe.h"

#include "LinuxBootTimes.h"
#include "LinuxCommitPaging.h"
#include "LinuxCoredumps.h"
#include "LinuxDevices.h"
#include "LinuxFirmwareInfo.h"
#include "LinuxGraphics.h"
#include "LinuxKernelModules.h"
#include "LinuxNetworkAdapters.h"
#include "LinuxOsInfo.h"
#include "LinuxPlatformSecurity.h"
#include "LinuxSensors.h"
#include "LinuxStorage.h"
#include "Platform/ISystemInfoProbe.h"
#include "SystemdBus.h"
#include "UserNameLookup.h"

#include <array>
#include <bit>
#include <cstdint>
// NOLINTNEXTLINE(misc-include-cleaner) - cstdlib provides secure_getenv when _GNU_SOURCE is defined
#include <cstdlib>
#include <ctime>
#include <string>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <pwd.h>
#include <sys/socket.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <unistd.h>

namespace Platform
{

namespace
{

/// An environment variable, or null. secure_getenv ignores them in a setuid process, as
/// LinuxPathProvider does.
[[nodiscard]] const char* environment(const char* name)
{
#if defined(__GLIBC__) && defined(_GNU_SOURCE)
    // NOLINTNEXTLINE(misc-include-cleaner) - secure_getenv from cstdlib with _GNU_SOURCE
    return secure_getenv(name);
#else
    // NOLINTNEXTLINE(concurrency-mt-unsafe) - fallback when secure_getenv is unavailable
    return std::getenv(name);
#endif
}

[[nodiscard]] std::string environmentString(const char* name)
{
    const char* value = environment(name);
    return value != nullptr ? std::string(value) : std::string{};
}

/// Every adapter's IPv4 and IPv6 addresses, from getifaddrs() (#1518). Empty if it fails.
[[nodiscard]] std::vector<LinuxNetworkAdapters::ListedAddress> listAdapterAddresses()
{
    std::vector<LinuxNetworkAdapters::ListedAddress> listed;
    ifaddrs* list = nullptr;
    if (::getifaddrs(&list) != 0)
    {
        return listed;
    }
    for (const ifaddrs* entry = list; entry != nullptr; entry = entry->ifa_next)
    {
        if (entry->ifa_addr == nullptr || entry->ifa_name == nullptr)
        {
            continue;
        }
        std::array<char, INET6_ADDRSTRLEN> text{};
        AdapterAddress address;
        if (entry->ifa_addr->sa_family == AF_INET)
        {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) - getifaddrs() hands sockaddr_in as sockaddr
            const auto* in = reinterpret_cast<const sockaddr_in*>(entry->ifa_addr);
            if (::inet_ntop(AF_INET, &in->sin_addr, text.data(), text.size()) == nullptr)
            {
                continue;
            }
            if (entry->ifa_netmask != nullptr)
            {
                // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) - as above
                const auto* mask = reinterpret_cast<const sockaddr_in*>(entry->ifa_netmask);
                address.prefix = static_cast<std::uint32_t>(std::popcount(mask->sin_addr.s_addr));
            }
        }
        else if (entry->ifa_addr->sa_family == AF_INET6)
        {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) - getifaddrs() hands sockaddr_in6 as sockaddr
            const auto* in6 = reinterpret_cast<const sockaddr_in6*>(entry->ifa_addr);
            if (::inet_ntop(AF_INET6, &in6->sin6_addr, text.data(), text.size()) == nullptr)
            {
                continue;
            }
            address.v6 = true;
            if (entry->ifa_netmask != nullptr)
            {
                // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) - as above
                const auto* mask = reinterpret_cast<const sockaddr_in6*>(entry->ifa_netmask);
                for (const std::uint8_t byte : mask->sin6_addr.s6_addr)
                {
                    address.prefix += static_cast<std::uint32_t>(std::popcount(byte));
                }
            }
        }
        else
        {
            continue; // AF_PACKET and the like: the MAC is read from /sys
        }
        address.address = text.data();
        listed.push_back({.adapter = entry->ifa_name, .address = std::move(address)});
    }
    ::freeifaddrs(list);
    return listed;
}

} // namespace

SystemInfoCapabilities LinuxSystemInfoProbe::capabilities() const
{
    return {.hasOs = true, .unavailableReason = {}};
}

OsInfo LinuxSystemInfoProbe::readOs()
{
    OsInfo info;
    info.family = OsFamily::Linux;
    LinuxOsInfo::readFileFacts(m_Root, info);

    struct utsname uts{};
    if (uname(&uts) == 0)
    {
        info.kernel = std::string(uts.sysname) + " " + uts.release;
        info.architecture = uts.machine;
    }
    std::array<char, 256> host{};
    if (gethostname(host.data(), host.size() - 1) == 0)
    {
        info.computerName = host.data();
    }
    info.userName = lookUpUserName(geteuid(), getpwuid_r).value_or(std::string{});
    info.locale = LinuxOsInfo::chooseLocale(environment("LC_ALL"), environment("LANG"));
    info.desktop = environmentString("XDG_CURRENT_DESKTOP");
    info.sessionType = environmentString("XDG_SESSION_TYPE");

    const std::time_t now = std::time(nullptr);
    struct tm local{};
    if (localtime_r(&now, &local) != nullptr)
    {
        info.utcOffsetMinutes = static_cast<int>(local.tm_gmtoff / 60);
    }
    return info;
}

FirmwareInfo LinuxSystemInfoProbe::readFirmware()
{
    FirmwareInfo info;
    LinuxFirmwareInfo::readFirmwareFacts(m_Root, info);
    return info;
}

MemoryModulesInfo LinuxSystemInfoProbe::readMemoryModules()
{
    MemoryModulesInfo info;
    LinuxFirmwareInfo::readMemoryModuleFacts(m_Root, info);
    return info;
}

CommitPagingInfo LinuxSystemInfoProbe::readCommitPaging()
{
    CommitPagingInfo info;
    LinuxCommitPaging::readCommitPagingFacts(m_Root, info);
    return info;
}

StorageInfo LinuxSystemInfoProbe::readStorage()
{
    StorageInfo info;
    // statvfs() on the mount point as the running system sees it; free is f_bavail, what an unprivileged
    // user can still write (df's "Avail"), matching GetDiskFreeSpaceExW's caller-available figure.
    const LinuxStorage::VolumeSizer sizer = [](const std::string& mountPoint, std::uint64_t& sizeBytes, std::uint64_t& freeBytes)
    {
        struct statvfs stats{};
        if (::statvfs(mountPoint.c_str(), &stats) != 0)
        {
            return false;
        }
        sizeBytes = static_cast<std::uint64_t>(stats.f_blocks) * stats.f_frsize;
        freeBytes = static_cast<std::uint64_t>(stats.f_bavail) * stats.f_frsize;
        return true;
    };
    // SMART status through udisks2 (#1631): only this system's, never under a fixture root.
    LinuxStorage::readStorageFacts(m_Root, info, sizer, m_Root == "/" ? SystemdBus::makeDriveSmartReader() : LinuxDiskSmart::SmartReader{});
    return info;
}

GraphicsInfo LinuxSystemInfoProbe::readGraphics()
{
    GraphicsInfo info;
    LinuxGraphics::readGraphicsFacts(m_Root, info);
    info.displayServer = LinuxGraphics::displayServer(
        environmentString("XDG_SESSION_TYPE"), environmentString("WAYLAND_DISPLAY"), environmentString("DISPLAY"));
    return info;
}

PlatformSecurityInfo LinuxSystemInfoProbe::readPlatformSecurity()
{
    PlatformSecurityInfo info;
    LinuxPlatformSecurity::readPlatformSecurityFacts(m_Root, info);
    return info;
}

SensorsInfo LinuxSystemInfoProbe::readSensors()
{
    SensorsInfo info;
    LinuxSensors::readSensorFacts(m_Root, info);
    return info;
}

DevicesInfo LinuxSystemInfoProbe::readDevices()
{
    DevicesInfo info;
    LinuxDevices::readDeviceFacts(m_Root, info);
    return info;
}

DriversInfo LinuxSystemInfoProbe::readDrivers()
{
    DriversInfo info;
    LinuxKernelModules::readKernelModules(m_Root, info);
    return info;
}

CrashesInfo LinuxSystemInfoProbe::readCrashes()
{
    CrashesInfo info;
    const std::time_t now = std::time(nullptr);
    LinuxCoredumps::readCoredumps(m_Root, now > 0 ? static_cast<std::uint64_t>(now) : 0, info);
    return info;
}

NetworkAdaptersInfo LinuxSystemInfoProbe::readNetworkAdapters()
{
    NetworkAdaptersInfo info;
    // The addresses are this system's own (getifaddrs()); the rest is read under m_Root, so a fixture
    // root in a test still gets this machine's addresses for adapters of the same name, if any.
    LinuxNetworkAdapters::readNetworkAdapterFacts(m_Root, info, m_Root == "/" ? &listAdapterAddresses : nullptr);
    return info;
}

BootPerformanceInfo LinuxSystemInfoProbe::readBootPerformance()
{
    BootPerformanceInfo info;
    info.available = true;
    if (m_Root != "/")
    {
        // A fixture root: systemd is only ever this system's.
        info.unavailableReason = "Not read under a test root";
        return info;
    }
    const SystemdBus::BootTimestampsRead read = SystemdBus::readBootTimestamps();
    if (!read.ok)
    {
        info.unavailableReason = read.error;
        return info;
    }
    info.timingsRead = true;
    LinuxBootTimes::computePhases(
        {.firmware = read.firmware, .loader = read.loader, .initrd = read.initrd, .userspace = read.userspace, .finish = read.finish},
        info);
    return info;
}

} // namespace Platform
