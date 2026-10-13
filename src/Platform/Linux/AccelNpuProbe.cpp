#include "AccelNpuProbe.h"

#include "Platform/GPUTypes.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace Platform
{

namespace AccelNpu
{

std::optional<double> busyPercent(std::uint64_t previousBusyUs, std::uint64_t busyUs, std::uint64_t wallDeltaNs) noexcept
{
    if (wallDeltaNs == 0 || busyUs < previousBusyUs)
    {
        return std::nullopt;
    }
    const double percent = (static_cast<double>(busyUs - previousBusyUs) * 1000.0 * 100.0) / static_cast<double>(wallDeltaNs);
    return std::clamp(percent, 0.0, 100.0);
}

std::string displayName(const std::string& driver)
{
    if (driver == "intel_vpu")
    {
        return "Intel NPU";
    }
    if (driver == "amdxdna")
    {
        return "AMD NPU";
    }
    return "Accelerator (" + (driver.empty() ? std::string("unknown driver") : driver) + ")";
}

DeviceKind kindOf(const std::string& driver) noexcept
{
    return (driver == "intel_vpu" || driver == "amdxdna") ? DeviceKind::Npu : DeviceKind::ComputeAccelerator;
}

} // namespace AccelNpu

namespace
{

/// The first line of a small sysfs file, or empty.
[[nodiscard]] std::string readLine(const std::filesystem::path& path)
{
    std::ifstream file(path);
    std::string line;
    std::getline(file, line);
    return line;
}

/// A decimal counter file, or nullopt when missing or malformed.
[[nodiscard]] std::optional<std::uint64_t> readCounter(const std::filesystem::path& path)
{
    const std::string text = readLine(path);
    std::uint64_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end == text.data())
    {
        return std::nullopt;
    }
    return value;
}

/// The vendor from a PCI vendor ID file ("0x8086").
[[nodiscard]] std::string vendorName(const std::filesystem::path& deviceDir)
{
    const std::string id = readLine(deviceDir / "vendor");
    if (id == "0x8086")
    {
        return "Intel";
    }
    if (id == "0x1022" || id == "0x1002")
    {
        return "AMD";
    }
    return "Unknown";
}

} // namespace

AccelNpuProbe::AccelNpuProbe(std::filesystem::path accelClassRoot, Clock clock)
    : m_Root(std::move(accelClassRoot)), m_Clock(std::move(clock)), m_Devices(scan())
{
    if (!m_Devices.empty())
    {
        spdlog::debug("AccelNpuProbe: {} accel device(s)", m_Devices.size());
    }
}

std::uint64_t AccelNpuProbe::nowNs() const
{
    if (m_Clock)
    {
        return m_Clock();
    }
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

std::vector<AccelNpuProbe::Device> AccelNpuProbe::scan() const
{
    std::vector<Device> devices;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(m_Root, ec), end; !ec && it != end; it.increment(ec))
    {
        const std::string name = it->path().filename().string();
        if (!name.starts_with("accel"))
        {
            continue;
        }
        const std::filesystem::path deviceDir = it->path() / "device";
        std::error_code linkError;
        const std::string driver = std::filesystem::read_symlink(deviceDir / "driver", linkError).filename().string();
        // The PCI slot ("0000:00:0b.0") is a stable id: the accel minor can change across boots.
        std::error_code canonicalError;
        const std::string slot = std::filesystem::canonical(deviceDir, canonicalError).filename().string();
        Device device;
        device.deviceDir = deviceDir;
        device.info.id = "ACCEL_" + (canonicalError || slot.empty() ? name : slot);
        device.info.name = AccelNpu::displayName(driver);
        device.info.vendor = vendorName(deviceDir);
        device.info.driverVersion = driver.empty() ? std::string("Unknown") : driver;
        device.info.kind = AccelNpu::kindOf(driver);
        device.info.isIntegrated = true; // Both NPUs today sit in the processor package
        std::uint32_t index = 0;
        static_cast<void>(std::from_chars(name.data() + 5, name.data() + name.size(), index));
        device.info.deviceIndex = index;
        device.info.sensorCapabilities = GPUCapabilities{}; // No temperature, power, clocks, fan or codecs
        devices.push_back(std::move(device));
    }
    std::ranges::sort(devices, {}, [](const Device& device) { return device.info.deviceIndex; });
    return devices;
}

std::vector<GPUInfo> AccelNpuProbe::enumerateGPUs()
{
    std::vector<GPUInfo> infos;
    infos.reserve(m_Devices.size());
    for (const Device& device : m_Devices)
    {
        infos.push_back(device.info);
    }
    return infos;
}

bool AccelNpuProbe::rescanGPUs(GPURescan depth)
{
    if (depth != GPURescan::Full)
    {
        return false;
    }
    std::vector<Device> found = scan();
    const auto ids = [](const std::vector<Device>& list)
    {
        std::vector<std::string> out;
        out.reserve(list.size());
        for (const Device& device : list)
        {
            out.push_back(device.info.id);
        }
        return out;
    };
    if (ids(found) == ids(m_Devices))
    {
        return false;
    }
    // Keep a persisting device's last busy sample, so its utilization carries on.
    for (Device& device : found)
    {
        const auto old = std::ranges::find(m_Devices, device.info.id, [](const Device& d) { return d.info.id; });
        if (old != m_Devices.end())
        {
            device.previousBusyUs = old->previousBusyUs;
            device.previousNs = old->previousNs;
        }
    }
    m_Devices = std::move(found);
    return true;
}

std::vector<GPUCounters> AccelNpuProbe::readGPUCounters()
{
    std::vector<GPUCounters> counters;
    counters.reserve(m_Devices.size());
    for (Device& device : m_Devices)
    {
        GPUCounters c;
        c.gpuId = device.info.id;
        c.kind = device.info.kind;
        c.temperatureAvailable = false;
        c.powerAvailable = false;
        c.gpuClockAvailable = false;
        c.memoryAvailable = false; // npu_memory_utilization has no total to set it against yet
        c.encoderAvailable = false;
        c.decoderAvailable = false;
        c.utilizationAvailable = false;
        const std::optional<std::uint64_t> busyUs = readCounter(device.deviceDir / "npu_busy_time_us");
        const std::uint64_t now = nowNs();
        if (busyUs.has_value())
        {
            // The first sample only sets the baseline: a rate needs two.
            if (device.previousBusyUs.has_value())
            {
                if (const std::optional<double> percent = AccelNpu::busyPercent(*device.previousBusyUs, *busyUs, now - device.previousNs))
                {
                    c.utilizationAvailable = true;
                    c.utilizationPercent = *percent;
                }
            }
            device.previousBusyUs = busyUs;
            device.previousNs = now;
        }
        counters.push_back(std::move(c));
    }
    return counters;
}

std::vector<ProcessGPUCounters> AccelNpuProbe::readProcessGPUCounters()
{
    return {}; // Neither driver reports per-process utilization; fdinfo memory is a follow-up (#1827)
}

GPUCapabilities AccelNpuProbe::capabilities() const
{
    return GPUCapabilities{.supportsMultiGPU = m_Devices.size() > 1};
}

} // namespace Platform
