#pragma once

// Which GPUs of a vendor are on the PCI bus, as Linux lists them in sysfs (#1116). NVML and ROCm SMI
// enumerate their devices once, at init, and never notice a GPU hot-plugged (an eGPU), removed, or
// rebound to a driver afterwards. The vendor probes compare this list across full rescans and
// re-initialise their library when it changes. Reading a device's vendor and class attributes and
// its driver link only reads what the kernel cached when it enumerated the device, so it never
// wakes a runtime-suspended GPU (#1117).

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace Platform::PciDisplayDevices
{

inline constexpr std::uint32_t PCI_VENDOR_NVIDIA = 0x10DEU;
inline constexpr std::uint32_t PCI_VENDOR_AMD = 0x1002U;

/// The kernel drivers NVML and ROCm SMI talk through: a vendor GPU bound to another driver (nouveau,
/// radeon, vfio-pci) is invisible to that library however often it is initialised.
inline constexpr std::string_view DRIVER_NVIDIA = "nvidia";
inline constexpr std::string_view DRIVER_AMDGPU = "amdgpu";

/// PCI base class 0x03, display controller (VGA, 3D, other): the top byte of the 24-bit class code.
inline constexpr std::uint32_t PCI_BASE_CLASS_DISPLAY = 0x03U;
inline constexpr unsigned PCI_BASE_CLASS_SHIFT = 16U;

/// Reads a sysfs hex attribute ("0x10de\n") into `value`; false if it can't be read or isn't hex.
[[nodiscard]] inline bool readHexAttribute(const std::filesystem::path& path, std::uint32_t& value)
{
    std::ifstream file(path);
    std::string text;
    if (!file.is_open() || !std::getline(file, text))
    {
        return false;
    }
    std::string_view digits(text);
    while (!digits.empty() && (digits.back() == '\n' || digits.back() == '\r' || digits.back() == ' '))
    {
        digits.remove_suffix(1);
    }
    if (digits.starts_with("0x") || digits.starts_with("0X"))
    {
        digits.remove_prefix(2);
    }
    const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), value, 16);
    return error == std::errc{} && end == digits.data() + digits.size() && !digits.empty();
}

/// The display-class devices of `vendorId` under `pciDevicesRoot` (normally /sys/bus/pci/devices),
/// each as "<address>=<bound driver>" ("0000:01:00.0=nvidia"; the driver is empty when none is
/// bound), sorted. The driver is part of the entry because a library only sees a device once its
/// driver binds, which can be after the device appears. Empty when the root can't be read.
[[nodiscard]] inline std::vector<std::string> list(const std::string& pciDevicesRoot, std::uint32_t vendorId)
{
    std::vector<std::string> devices;
    std::error_code error;
    const std::filesystem::directory_iterator entries(pciDevicesRoot, error);
    if (error)
    {
        return devices;
    }
    for (const auto& entry : entries)
    {
        const auto& dir = entry.path();
        std::uint32_t vendor = 0;
        std::uint32_t pciClass = 0;
        if (!readHexAttribute(dir / "vendor", vendor) || vendor != vendorId || !readHexAttribute(dir / "class", pciClass) ||
            (pciClass >> PCI_BASE_CLASS_SHIFT) != PCI_BASE_CLASS_DISPLAY)
        {
            continue;
        }
        std::string driver;
        std::error_code linkError;
        const auto target = std::filesystem::read_symlink(dir / "driver", linkError);
        if (!linkError)
        {
            driver = target.filename().string();
        }
        devices.push_back(dir.filename().string() + "=" + driver);
    }
    std::ranges::sort(devices);
    return devices;
}

/// Whether any of `devices` (entries from list()) is bound to `driver`.
[[nodiscard]] inline bool anyBoundTo(const std::vector<std::string>& devices, std::string_view driver)
{
    return std::ranges::any_of(devices,
                               [driver](const std::string& device)
                               {
                                   const auto separator = device.rfind('=');
                                   return separator != std::string::npos && std::string_view(device).substr(separator + 1) == driver;
                               });
}

} // namespace Platform::PciDisplayDevices
