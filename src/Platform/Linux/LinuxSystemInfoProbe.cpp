#include "LinuxSystemInfoProbe.h"

#include "LinuxCommitPaging.h"
#include "LinuxFirmwareInfo.h"
#include "LinuxOsInfo.h"
#include "LinuxStorage.h"
#include "Platform/ISystemInfoProbe.h"
#include "UserNameLookup.h"

#include <array>
#include <cstdint>
// NOLINTNEXTLINE(misc-include-cleaner) - cstdlib provides secure_getenv when _GNU_SOURCE is defined
#include <cstdlib>
#include <ctime>
#include <string>

#include <pwd.h>
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
    LinuxStorage::readStorageFacts(m_Root, info, sizer);
    return info;
}

} // namespace Platform
