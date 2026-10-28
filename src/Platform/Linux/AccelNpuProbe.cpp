#include "AccelNpuProbe.h"

#include "Platform/GPUTypes.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
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

namespace
{

/// A drm-usage-stats memory value ("4096 KiB", "12 MiB", "512") in bytes; nullopt in another form or
/// when too large.
[[nodiscard]] std::optional<std::uint64_t> memoryBytes(std::string_view value)
{
    const std::size_t start = value.find_first_not_of(" \t");
    if (start == std::string_view::npos)
    {
        return std::nullopt;
    }
    value.remove_prefix(start);
    std::uint64_t number = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), number);
    if (error != std::errc{} || end == value.data())
    {
        return std::nullopt;
    }
    value.remove_prefix(static_cast<std::size_t>(end - value.data()));
    const std::size_t unitStart = value.find_first_not_of(" \t");
    const std::string_view unit = (unitStart == std::string_view::npos) ? std::string_view{} : value.substr(unitStart);
    std::uint64_t scale = 1;
    if (unit == "KiB")
    {
        scale = 1024ULL;
    }
    else if (unit == "MiB")
    {
        scale = 1024ULL * 1024ULL;
    }
    else if (unit == "GiB")
    {
        scale = 1024ULL * 1024ULL * 1024ULL;
    }
    else if (!unit.empty())
    {
        return std::nullopt;
    }
    if (number > std::numeric_limits<std::uint64_t>::max() / scale)
    {
        return std::nullopt;
    }
    return number * scale;
}

} // namespace

ClientMemory parseFdinfo(std::string_view text)
{
    ClientMemory memory;
    std::optional<std::uint64_t> resident;
    std::optional<std::uint64_t> total;
    while (!text.empty())
    {
        const std::size_t end = text.find('\n');
        std::string_view line = text.substr(0, end);
        text.remove_prefix(end == std::string_view::npos ? text.size() : end + 1);
        if (!line.empty() && line.back() == '\r')
        {
            line.remove_suffix(1);
        }
        const std::size_t colon = line.find(':');
        if (colon == std::string_view::npos)
        {
            continue;
        }
        const std::string_view key = line.substr(0, colon);
        const std::string_view value = line.substr(colon + 1);
        if (key == "drm-client-id")
        {
            std::uint64_t id = 0;
            const std::string_view digits = value.substr(std::min(value.size(), value.find_first_not_of(" \t")));
            if (const auto [ptr, error] = std::from_chars(digits.data(), digits.data() + digits.size(), id); error == std::errc{})
            {
                memory.clientId = id;
            }
        }
        else if (key == "drm-resident-memory")
        {
            resident = memoryBytes(value);
        }
        else if (key == "drm-total-memory")
        {
            total = memoryBytes(value);
        }
    }
    memory.bytes = resident.has_value() ? resident : total;
    return memory;
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
[[nodiscard]] std::string_view vendorName(const std::filesystem::path& deviceDir)
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

AccelNpuProbe::AccelNpuProbe(std::filesystem::path accelClassRoot, Clock clock, std::filesystem::path procRoot)
    : m_Root(std::move(accelClassRoot)), m_Clock(std::move(clock)), m_ProcRoot(std::move(procRoot)), m_Devices(scan())
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
        device.nodePath = "/dev/accel/" + name;
        device.info.id = "ACCEL_" + (canonicalError || slot.empty() ? name : slot);
        device.info.name = AccelNpu::displayName(driver);
        device.info.vendor = vendorName(deviceDir);
        device.info.driverVersion = driver.empty() ? std::string("Unknown") : driver;
        device.info.kind = AccelNpu::kindOf(driver);
        device.info.isIntegrated = true; // Both NPUs today sit in the processor package
        // ...and map system memory, so a client's is shared memory (#1827); another accelerator (Gaudi's
        // habanalabs) has memory of its own, so its clients' is dedicated.
        device.info.memoryIsShared = device.info.kind == DeviceKind::Npu;
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
    // A device keeps its id (its PCI slot) when it comes back under another accelN, so its node is part
    // of what is compared: the clients are found by the node their fds link to (#1827).
    const auto ids = [](const std::vector<Device>& list)
    {
        std::vector<std::pair<std::string, std::string>> out;
        out.reserve(list.size());
        for (const Device& device : list)
        {
            out.emplace_back(device.info.id, device.nodePath);
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
    // The clients' device indices were into the old list: look for them again on the next read.
    m_Clients.clear();
    m_LastDiscoveryNs.reset();
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
        // npu_memory_utilization (intel_vpu) is the bytes the NPU has in use, with no total to set it
        // against: shown as bytes, never a percent of an invented total (#1827).
        c.memoryAvailable = false;
        c.memoryInUseBytes = readCounter(device.deviceDir / "npu_memory_utilization");
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

void AccelNpuProbe::discoverClients()
{
    std::vector<Client> found;
    std::error_code procError;
    for (std::filesystem::directory_iterator procIt(m_ProcRoot, procError), procEnd; !procError && procIt != procEnd;
         procIt.increment(procError))
    {
        const std::string name = procIt->path().filename().string();
        std::int32_t pid = 0;
        if (const auto [end, error] = std::from_chars(name.data(), name.data() + name.size(), pid);
            error != std::errc{} || end != name.data() + name.size() || pid <= 0)
        {
            continue;
        }
        // Another user's process can't be looked into without privileges, and a process can exit
        // mid-walk: either just has no fds here.
        std::error_code fdError;
        for (std::filesystem::directory_iterator fdIt(procIt->path() / "fd", fdError), fdEnd; !fdError && fdIt != fdEnd;
             fdIt.increment(fdError))
        {
            std::error_code linkError;
            const std::filesystem::path target = std::filesystem::read_symlink(fdIt->path(), linkError);
            if (linkError || !target.native().starts_with("/dev/accel/"))
            {
                continue;
            }
            const auto device = std::ranges::find(m_Devices, target.native(), &Device::nodePath);
            if (device != m_Devices.end())
            {
                found.push_back({.pid = pid,
                                 .fd = fdIt->path(),
                                 .fdinfo = procIt->path() / "fdinfo" / fdIt->path().filename(),
                                 .device = static_cast<std::size_t>(device - m_Devices.begin())});
            }
        }
    }
    m_Clients = std::move(found);
}

std::vector<ProcessGPUCounters> AccelNpuProbe::readProcessGPUCounters()
{
    if (m_Devices.empty())
    {
        return {};
    }
    const std::uint64_t now = nowNs();
    if (!m_LastDiscoveryNs.has_value() || now - *m_LastDiscoveryNs >= CLIENT_DISCOVERY_INTERVAL_NS)
    {
        discoverClients();
        m_LastDiscoveryNs = now;
    }

    // Each process's memory on each device: a client's fds share its drm-client-id, so it is counted
    // once; an fd whose fdinfo prints no id is its own client. Neither driver gives per-process
    // utilization, so only memory is read (GPUCapabilities::hasPerProcessNpuUtilization is false).
    std::map<std::pair<std::int32_t, std::size_t>, std::uint64_t> bytesByProcess;
    std::set<std::tuple<std::int32_t, std::size_t, std::uint64_t>> clientsSeen;
    for (const Client& client : m_Clients)
    {
        // The fd found by the last walk may since have been closed and its number reused for another
        // file (or the PID for another process): only while it still links to the device is it this
        // client (#1827), as DRMGPUProbe checks its clients' links.
        std::error_code linkError;
        if (std::filesystem::read_symlink(client.fd, linkError).native() != m_Devices[client.device].nodePath || linkError)
        {
            continue;
        }
        std::ifstream file(client.fdinfo);
        if (!file)
        {
            continue; // Closed, or the process exited, since the walk
        }
        const std::string text{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
        const AccelNpu::ClientMemory memory = AccelNpu::parseFdinfo(text);
        if (memory.clientId.has_value() && !clientsSeen.emplace(client.pid, client.device, *memory.clientId).second)
        {
            continue;
        }
        std::uint64_t& bytes = bytesByProcess[{client.pid, client.device}];
        bytes += memory.bytes.value_or(0);
    }

    std::vector<ProcessGPUCounters> counters;
    counters.reserve(bytesByProcess.size());
    for (const auto& [key, bytes] : bytesByProcess)
    {
        const Device& device = m_Devices[key.second];
        ProcessGPUCounters c;
        c.pid = key.first;
        c.gpuId = device.info.id;
        c.kind = device.info.kind;
        // System memory an NPU maps for it, or an accelerator's own memory (Device::info.memoryIsShared).
        (device.info.memoryIsShared ? c.gpuSharedMemoryBytes : c.gpuMemoryBytes) = bytes;
        counters.push_back(std::move(c));
    }
    return counters;
}

GPUCapabilities AccelNpuProbe::capabilities() const
{
    // Per-process NPU memory where there is an accel device to read it from (#1827); never utilization.
    return GPUCapabilities{.hasPerProcessNpuMetrics = !m_Devices.empty(), .supportsMultiGPU = m_Devices.size() > 1};
}

} // namespace Platform
