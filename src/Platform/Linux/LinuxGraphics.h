#pragma once

// The Linux Graphics & displays facts (#1519), read under an injected root ("/" in the app, a fixture tree
// in tests), all unprivileged and without waking a suspended GPU:
// - Adapters: each /sys/class/drm/cardN's device/uevent (DRIVER, PCI_ID, PCI_SLOT_NAME), the driver
//   module's version (/sys/module/<driver>/version: out-of-tree modules such as nvidia have one, in-kernel
//   ones don't), amdgpu's product_name and VRAM size, and NVIDIA's model from /proc/driver/nvidia.
// - Monitors: each connected connector (/sys/class/drm/cardN-<connector>/status) and its EDID
//   (Platform/EdidParser.h).
// - The display server from XDG_SESSION_TYPE, WAYLAND_DISPLAY and DISPLAY, passed in by the probe.
// Standard library only, so the fixture tests run on every platform.

#include "Platform/EdidParser.h"
#include "Platform/ISystemInfoProbe.h"
#include "Platform/Linux/LinuxOsInfo.h"
#include "Platform/Linux/LinuxStorage.h"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace Platform::LinuxGraphics
{

/// "Wayland", "X11" or the session type as given ("tty" excepted); from the variables when the session
/// type is unset. Empty without a graphical session.
[[nodiscard]] inline std::string displayServer(std::string_view sessionType, std::string_view waylandDisplay, std::string_view display)
{
    if (sessionType == "wayland" || (sessionType.empty() && !waylandDisplay.empty()))
    {
        return "Wayland";
    }
    if (sessionType == "x11" || (sessionType.empty() && !display.empty()))
    {
        return "X11";
    }
    return sessionType == "tty" ? std::string{} : std::string(sessionType);
}

/// A KEY=value line's value in a sysfs uevent file; empty when absent.
[[nodiscard]] inline std::string ueventValue(std::string_view text, std::string_view key)
{
    std::size_t start = 0;
    while (start < text.size())
    {
        std::size_t end = text.find('\n', start);
        end = end == std::string_view::npos ? text.size() : end;
        const std::string_view line = text.substr(start, end - start);
        if (line.size() > key.size() && line.starts_with(key) && line[key.size()] == '=')
        {
            return LinuxStorage::trimmed(line.substr(key.size() + 1));
        }
        start = end + 1;
    }
    return {};
}

/// The vendor's name for a PCI vendor id, or empty.
[[nodiscard]] inline std::string_view vendorName(std::uint32_t vendorId)
{
    switch (vendorId)
    {
    case 0x10DE:
        return "NVIDIA";
    case 0x1002:
        return "AMD";
    case 0x8086:
        return "Intel";
    default:
        return {};
    }
}

/// The "Model:" line of /proc/driver/nvidia/gpus/<slot>/information; empty when absent.
[[nodiscard]] inline std::string nvidiaModel(std::string_view information)
{
    const std::size_t at = information.find("Model:");
    if (at == std::string_view::npos)
    {
        return {};
    }
    const std::size_t end = information.find('\n', at);
    return LinuxStorage::trimmed(information.substr(at + 6, end == std::string_view::npos ? std::string_view::npos : end - at - 6));
}

/// /sys/class/drm's entries, sorted; empty when it can't be listed (ok false).
[[nodiscard]] inline std::vector<std::string> drmEntries(const std::filesystem::path& drm, bool& ok)
{
    std::vector<std::string> names;
    std::error_code ec;
    std::filesystem::directory_iterator it(drm, ec);
    ok = !ec;
    for (; !ec && it != std::filesystem::directory_iterator{}; it.increment(ec))
    {
        names.push_back(it->path().filename().string());
    }
    std::ranges::sort(names);
    return names;
}

[[nodiscard]] inline GraphicsAdapter readAdapter(const std::filesystem::path& root, const std::filesystem::path& device)
{
    GraphicsAdapter adapter;
    const std::string uevent = LinuxOsInfo::readFile(device / "uevent");
    adapter.driver = ueventValue(uevent, "DRIVER");
    adapter.location = ueventValue(uevent, "PCI_SLOT_NAME");
    const std::string pciId = ueventValue(uevent, "PCI_ID"); // "1002:73BF"
    if (const std::size_t colon = pciId.find(':'); colon != std::string::npos)
    {
        static_cast<void>(std::from_chars(pciId.data(), pciId.data() + colon, adapter.vendorId, 16));
        static_cast<void>(std::from_chars(pciId.data() + colon + 1, pciId.data() + pciId.size(), adapter.deviceId, 16));
    }
    if (!adapter.driver.empty())
    {
        adapter.driverVersion = LinuxOsInfo::readLine(root / "sys/module" / adapter.driver / "version");
    }
    adapter.name = LinuxOsInfo::readLine(device / "product_name"); // amdgpu
    if (adapter.name.empty() && !adapter.location.empty())
    {
        adapter.name = nvidiaModel(LinuxOsInfo::readFile(root / "proc/driver/nvidia/gpus" / adapter.location / "information"));
    }
    if (adapter.name.empty() && !pciId.empty())
    {
        const std::string_view vendor = vendorName(adapter.vendorId);
        adapter.name = std::format("{}GPU ({})", vendor.empty() ? std::string{} : std::string(vendor) + " ", pciId);
    }
    const std::string vram = LinuxOsInfo::readLine(device / "mem_info_vram_total"); // amdgpu
    static_cast<void>(std::from_chars(vram.data(), vram.data() + vram.size(), adapter.dedicatedBytes));
    return adapter;
}

/// Every DRM card's adapter and every connected connector's monitor under @p root.
inline void readGraphicsFacts(const std::filesystem::path& root, GraphicsInfo& info)
{
    info.available = true;
    info.family = OsFamily::Linux;
    const std::filesystem::path drm = root / "sys/class/drm";
    bool listed = false;
    const std::vector<std::string> entries = drmEntries(drm, listed);
    info.adaptersRead = listed;
    info.monitorsRead = listed;
    for (const std::string& name : entries)
    {
        if (!name.starts_with("card"))
        {
            continue; // renderD*, version
        }
        const std::size_t dash = name.find('-');
        if (dash == std::string::npos)
        {
            info.adapters.push_back(readAdapter(root, drm / name / "device"));
            continue;
        }
        if (LinuxOsInfo::readLine(drm / name / "status") != "connected")
        {
            continue;
        }
        Monitor monitor;
        monitor.connector = name.substr(dash + 1);
        const std::string edid = LinuxOsInfo::readFile(drm / name / "edid");
        const std::vector<std::uint8_t> bytes(edid.begin(), edid.end());
        if (const auto parsed = Edid::parseEdid(bytes))
        {
            monitor.name = parsed->displayName();
            monitor.serial = parsed->serialText();
            monitor.widthMm = parsed->widthMm;
            monitor.heightMm = parsed->heightMm;
        }
        info.monitors.push_back(std::move(monitor));
    }
}

} // namespace Platform::LinuxGraphics
