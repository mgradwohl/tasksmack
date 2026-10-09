#include "LinuxSystemInfoProbe.h"

#include "LinuxFirmwareInfo.h"
#include "LinuxOsInfo.h"
#include "Platform/ISystemInfoProbe.h"
#include "UserNameLookup.h"

#include <array>
// NOLINTNEXTLINE(misc-include-cleaner) - cstdlib provides secure_getenv when _GNU_SOURCE is defined
#include <cstdlib>
#include <ctime>
#include <string>

#include <pwd.h>
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

} // namespace Platform
