#pragma once

// PCI runtime power management, as Linux reports it in sysfs (#1117). On a hybrid laptop the
// discrete GPU drops into runtime D3 (D3cold when the platform supports it) whenever it is idle,
// and any query that has to talk to the device -- NVML and ROCm SMI sensor reads, i915/xe hwmon --
// resumes it. Polling it every sample therefore keeps it awake and drains the battery. A probe
// checks power/runtime_status first, which only reads the PM core's bookkeeping and never wakes
// the device, and leaves a sleeping GPU alone.

#include <cstdint>
#include <format>
#include <fstream>
#include <string>
#include <string_view>

namespace Platform::PciRuntimePm
{

/// Where Linux lists PCI devices by address (each entry a symlink to the device's sysfs directory).
inline constexpr std::string_view PCI_DEVICES_ROOT = "/sys/bus/pci/devices";

/// The sysfs name of a PCI device ("0000:01:00.0"): domain, bus, device and function in hex, as the
/// kernel prints them (the domain at least four digits).
[[nodiscard]] inline std::string pciAddress(std::uint32_t domain, std::uint32_t bus, std::uint32_t device, std::uint32_t function)
{
    return std::format("{:04x}:{:02x}:{:02x}.{:x}", domain, bus, device, function);
}

/// Whether a power/runtime_status value means the device is asleep. "suspending" counts: a query
/// then would wait for the suspend to finish and resume it again. "resuming" and "active" are
/// awake, and anything else ("unsupported", "error", an empty read) is treated as awake, as before
/// this check existed.
[[nodiscard]] constexpr bool isSuspendedStatus(std::string_view status) noexcept
{
    while (!status.empty() && (status.back() == '\n' || status.back() == '\r' || status.back() == ' ' || status.back() == '\t'))
    {
        status.remove_suffix(1);
    }
    return status == "suspended" || status == "suspending";
}

/// Whether the PCI device whose sysfs directory is `deviceDir` is runtime-suspended now. False when
/// the file can't be read (no runtime PM, a fake path, no permission), so a probe that can't tell
/// keeps reading the device as it always has.
[[nodiscard]] inline bool isRuntimeSuspended(const std::string& deviceDir)
{
    if (deviceDir.empty())
    {
        return false;
    }
    std::ifstream file(deviceDir + "/power/runtime_status");
    if (!file.is_open())
    {
        return false;
    }
    std::string status;
    std::getline(file, status);
    return isSuspendedStatus(status);
}

} // namespace Platform::PciRuntimePm
