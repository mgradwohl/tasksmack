// Keep this translation unit parseable on non-Linux platforms (e.g. Windows clangd)
// by compiling the implementation only when targeting Linux and required headers exist.
#if defined(__linux__) && __has_include(<unistd.h>)

#include "LinuxDiskProbe.h"

#include "Platform/StorageTypes.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <locale>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace Platform
{

LinuxDiskProbe::LinuxDiskProbe(std::filesystem::path diskstatsPath) : m_DiskstatsPath(std::move(diskstatsPath))
{
    spdlog::debug("LinuxDiskProbe: initialized");
}

LinuxDiskProbe::LinuxDiskProbe(std::filesystem::path diskstatsPath, std::filesystem::path sysBlockPath)
    : m_DiskstatsPath(std::move(diskstatsPath)), m_SysBlockPath(std::move(sysBlockPath))
{
    spdlog::debug("LinuxDiskProbe: initialized");
}

LinuxDiskProbe::LinuxDiskProbe() : LinuxDiskProbe("/proc/diskstats", "/sys/block")
{}

bool LinuxDiskProbe::isAlwaysExcluded(const std::string& deviceName)
{
    // Loop devices and RAM disks carry no disk I/O of their own; zram is compressed swap in memory; a device-mapper
    // volume (dm-N: LVM, LUKS) is stacked on a disk that is already counted.
    return deviceName.starts_with("loop") || deviceName.starts_with("ram") || deviceName.starts_with("zram") ||
           deviceName.starts_with("dm-");
}

std::optional<std::vector<LinuxDiskProbe::WholeDevice>> LinuxDiskProbe::listWholeDevices(const std::filesystem::path& sysBlockPath)
{
    std::error_code error;
    std::filesystem::directory_iterator entries(sysBlockPath, error);
    if (error)
    {
        return std::nullopt;
    }
    std::vector<WholeDevice> devices;
    for (; entries != std::filesystem::directory_iterator(); entries.increment(error))
    {
        if (error)
        {
            return std::nullopt;
        }
        WholeDevice device{.name = entries->path().filename().string()};
        std::error_code slavesError;
        const std::filesystem::directory_iterator slaves(entries->path() / "slaves", slavesError);
        device.stacked = device.name.starts_with("zd") || (!slavesError && slaves != std::filesystem::directory_iterator());
        devices.push_back(std::move(device));
    }
    return devices;
}

bool LinuxDiskProbe::shouldIncludeDevice(const std::string& deviceName)
{
    // Filter out loop devices, ram disks, and partitions for a cleaner view
    // Users can still see partitions by checking detailed device info if needed

    if (isAlwaysExcluded(deviceName))
    {
        return false;
    }

    // Skip device mapper partitions that are just LVM volumes (dm-0, dm-1)
    // Note: We could potentially include these for LVM setups, but for simplicity skip them
    // Physical devices or primary virtual devices (like nvme0n1, sda) are more useful

    // Include all physical devices and their meaningful representations
    // This includes: sda, sdb, nvme0n1, nvme1n1, vda (virtual disks), etc.
    // But exclude numbered partitions (sda1, sda2, nvme0n1p1, etc.) for the main view

    // Check if it ends with a digit (likely a partition)
    if (!deviceName.empty() && (std::isdigit(static_cast<unsigned char>(deviceName.back())) != 0))
    {
        // Exception: NVMe whole namespaces (nvme0n1, nvme1n2) — contain "nvme", no 'p'.
        if (deviceName.contains("nvme") && !deviceName.contains('p'))
        {
            return true;
        }

        // Exception: eMMC whole-disk devices (mmcblk0, mmcblk1).
        // Match only "mmcblk" + all-digit suffix to exclude boot partitions
        // (mmcblk0boot0, mmcblk0boot1) and RPMB devices that also end in a digit.
        if (deviceName.starts_with("mmcblk"))
        {
            constexpr std::string_view k_MmcblkPrefix{"mmcblk"};
            const std::string_view suffix = std::string_view{deviceName}.substr(k_MmcblkPrefix.size());
            const bool allDigits = !suffix.empty() && std::ranges::all_of(suffix, [](unsigned char c) { return std::isdigit(c) != 0; });
            return allDigits;
        }

        return false;
    }

    return true;
}

SystemDiskCounters LinuxDiskProbe::read()
{
    SystemDiskCounters result;

    std::ifstream diskstats(m_DiskstatsPath);
    if (!diskstats.is_open())
    {
        spdlog::warn("LinuxDiskProbe: Failed to open {}", m_DiskstatsPath.string());
        return result;
    }

    // /sys/block lists exactly the whole block devices, so md0, nbd0, rbd0, pmem0 or bcache0 (whole devices whose
    // names end in a digit) aren't taken for partitions (#4391). The name rule is the fallback without it.
    const std::optional<std::vector<WholeDevice>> wholeDevices = m_SysBlockPath.empty() ? std::nullopt : listWholeDevices(m_SysBlockPath);

    std::string line;
    while (std::getline(diskstats, line))
    {
        std::istringstream iss(line);
        // Kernel text: parse it in the classic locale, never the user's (digit grouping, #1648).
        iss.imbue(std::locale::classic());

        // /proc/diskstats format (Linux kernel 2.6+):
        // major minor device_name reads_completed reads_merged sectors_read time_reading
        // writes_completed writes_merged sectors_written time_writing
        // io_in_progress time_io weighted_time_io
        // (Plus additional fields in newer kernels that we ignore)

        int major = 0;
        int minor = 0;
        std::string deviceName;
        uint64_t readsCompleted = 0;
        uint64_t readsMerged = 0; // Not used but must be read
        uint64_t sectorsRead = 0;
        uint64_t timeReading = 0;
        uint64_t writesCompleted = 0;
        uint64_t writesMerged = 0; // Not used but must be read
        uint64_t sectorsWritten = 0;
        uint64_t timeWriting = 0;
        uint64_t ioInProgress = 0;
        uint64_t timeIo = 0;
        uint64_t weightedTimeIo = 0;

        iss >> major >> minor >> deviceName >> readsCompleted >> readsMerged >> sectorsRead >> timeReading >> writesCompleted >>
            writesMerged >> sectorsWritten >> timeWriting >> ioInProgress >> timeIo >> weightedTimeIo;

        if (iss.fail())
        {
            // Line didn't parse correctly, skip it
            continue;
        }

        bool stacked = false;
        if (wholeDevices.has_value())
        {
            const auto whole = std::ranges::find(*wholeDevices, deviceName, &WholeDevice::name);
            if (whole == wholeDevices->end() || isAlwaysExcluded(deviceName))
            {
                continue; // a partition, or a device left out either way
            }
            stacked = whole->stacked;
        }
        else if (!shouldIncludeDevice(deviceName))
        {
            continue;
        }

        DiskCounters disk;
        disk.deviceName = deviceName;
        disk.readsCompleted = readsCompleted;
        disk.readSectors = sectorsRead;
        disk.readTimeMs = timeReading;
        disk.writesCompleted = writesCompleted;
        disk.writeSectors = sectorsWritten;
        disk.writeTimeMs = timeWriting;
        disk.ioInProgressMs = ioInProgress;
        disk.ioTimeMs = timeIo;
        disk.weightedIoTimeMs = weightedTimeIo;
        disk.sectorSize = 512;            // Linux typically reports in 512-byte sectors
        disk.isPhysicalDevice = !stacked; // a stacked device is shown but kept out of the totals (#4391)

        result.disks.push_back(disk);
    }

    return result;
}

DiskCapabilities LinuxDiskProbe::capabilities() const
{
    DiskCapabilities caps;
    caps.hasDiskStats = true;
    caps.hasReadWriteBytes = true;
    caps.hasIoTime = true;
    caps.hasDeviceInfo = true;
    caps.canFilterPhysical = true;
    return caps;
}

} // namespace Platform

#endif // __linux__ && __has_include(<unistd.h>)
