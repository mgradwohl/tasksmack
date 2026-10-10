#pragma once

// The Windows Drivers facts (#1521), unprivileged and enumeration only: the running kernel and file system
// driver services from the Service Control Manager (EnumServicesStatusExW with SERVICE_DRIVER and
// SERVICE_ACTIVE), each with its start type and image path from Windows::readServiceConfig() (the
// Services tab's reader), and the image's file version and company from its version resource.
// The SCM is the one source: a running driver service is a loaded driver, and it names its start type and
// image. Loaded images that aren't services (the kernel, the HAL, kdcom and the like, which
// EnumDeviceDrivers would add) aren't listed. No driver is started, stopped or changed.
// Every call goes through an injectable table; WindowsDrivers.cpp supplies the real one, tests substitute
// fakes.

#include "Platform/IServiceProbe.h"
#include "Platform/ISystemInfoProbe.h"
#include "Platform/Windows/WindowsServiceProbeMath.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Platform::WindowsDrivers
{

/// One running driver service, as listDriverServices() reads it.
struct DriverServiceRecord
{
    std::string name;                                       ///< lpServiceName
    std::string displayName;                                ///< lpDisplayName
    std::uint32_t serviceType = 0;                          ///< SERVICE_KERNEL_DRIVER (1) / SERVICE_FILE_SYSTEM_DRIVER (2)
    std::uint32_t currentState = 0;                         ///< SERVICE_RUNNING (4), ...
    ServiceStartType startType = ServiceStartType::Unknown; ///< From the service's configuration
    std::string binaryPath;                                 ///< lpBinaryPathName as configured: "\SystemRoot\System32\drivers\x.sys"
};

/// An image file's version resource.
struct FileVersionRecord
{
    std::string version; ///< VS_FIXEDFILEINFO's file version, "10.0.26100.1"
    std::string company; ///< StringFileInfo's CompanyName
};

/// The calls readDrivers() makes.
struct Functions
{
    std::optional<std::vector<DriverServiceRecord>> (*listDriverServices)() = nullptr; ///< nullopt when the SCM fails
    std::string (*windowsDirectory)() = nullptr;                                       ///< "C:\Windows"
    FileVersionRecord (*readFileVersion)(const std::string& path) = nullptr;           ///< Empty fields when it has none
};

/// A driver's image as a file path. The SCM keeps it as the kernel loads it: "\SystemRoot\..." or a path
/// relative to the Windows directory ("System32\drivers\x.sys"), an NT "\??\C:\..." path, or nothing, which
/// means "%SystemRoot%\System32\drivers\<name>.sys".
[[nodiscard]] inline std::string driverImagePath(std::string_view binaryPath, std::string_view name, std::string_view windowsDir)
{
    const auto startsWithIgnoringCase = [](std::string_view text, std::string_view prefix)
    {
        return text.size() >= prefix.size() &&
               std::ranges::equal(text.substr(0, prefix.size()),
                                  prefix,
                                  [](unsigned char a, unsigned char b) { return std::tolower(a) == std::tolower(b); });
    };
    std::string dir(windowsDir);
    while (!dir.empty() && (dir.back() == '\\' || dir.back() == '/'))
    {
        dir.pop_back();
    }
    // A driver's image path has no arguments, and may hold spaces unquoted; drop surrounding quotes and spaces.
    std::string_view path = binaryPath;
    const std::size_t first = path.find_first_not_of(" \t\"");
    path = first == std::string_view::npos ? std::string_view{} : path.substr(first, path.find_last_not_of(" \t\"") - first + 1);
    if (path.empty())
    {
        return name.empty() || dir.empty() ? std::string{} : std::format(R"({}\System32\drivers\{}.sys)", dir, name);
    }
    for (const std::string_view prefix : {std::string_view(R"(\??\)"), std::string_view(R"(\\?\)")})
    {
        if (path.starts_with(prefix))
        {
            return std::string(path.substr(prefix.size()));
        }
    }
    for (const std::string_view root : {std::string_view("\\SystemRoot\\"), std::string_view("%SystemRoot%\\")})
    {
        if (startsWithIgnoringCase(path, root))
        {
            return dir.empty() ? std::string{} : std::format("{}\\{}", dir, path.substr(root.size()));
        }
    }
    const bool absolute = path.starts_with('\\') || (path.size() >= 2 && path[1] == ':');
    if (absolute)
    {
        return std::string(path);
    }
    return dir.empty() ? std::string{} : std::format("{}\\{}", dir, path);
}

/// VS_FIXEDFILEINFO's file version: "10.0.26100.1"; empty when both halves are 0.
[[nodiscard]] inline std::string formatFileVersion(std::uint32_t ms, std::uint32_t ls)
{
    constexpr unsigned HALF = 16;
    constexpr std::uint32_t MASK = 0xFFFF;
    if (ms == 0 && ls == 0)
    {
        return {};
    }
    return std::format("{}.{}.{}.{}", ms >> HALF, ms & MASK, ls >> HALF, ls & MASK);
}

/// The running driver services with their images' versions, through @p fns.
inline void readDrivers(DriversInfo& info, const Functions& fns)
{
    info.available = true;
    info.family = OsFamily::Windows;
    const std::optional<std::vector<DriverServiceRecord>> services = fns.listDriverServices();
    if (!services.has_value())
    {
        return;
    }
    info.listed = true;
    const std::string windowsDir = fns.windowsDirectory();
    constexpr std::uint32_t FILE_SYSTEM_DRIVER = 0x2;
    for (const DriverServiceRecord& service : *services)
    {
        KernelDriver driver;
        driver.name = service.name;
        driver.displayName = service.displayName;
        driver.fileSystem = (service.serviceType & FILE_SYSTEM_DRIVER) != 0;
        driver.state = Windows::ServiceMath::stateFromCode(service.currentState);
        driver.startType = service.startType;
        driver.path = driverImagePath(service.binaryPath, service.name, windowsDir);
        if (!driver.path.empty())
        {
            FileVersionRecord file = fns.readFileVersion(driver.path);
            driver.version = std::move(file.version);
            driver.company = std::move(file.company);
        }
        info.drivers.push_back(std::move(driver));
    }
}

} // namespace Platform::WindowsDrivers
