// Keep this translation unit parseable on non-Linux platforms (e.g. Windows clangd)
// by compiling the implementation only when targeting Linux and required headers exist.
#if defined(__linux__) && __has_include(<unistd.h>)

#include "LinuxDiskProbe.h"

#include <spdlog/spdlog.h>

#include <fstream>
#include <sstream>
#include <string>

namespace Platform
{

LinuxDiskProbe::LinuxDiskProbe()
{
    spdlog::debug("LinuxDiskProbe: initialized");
}

bool LinuxDiskProbe::shouldIncludeDevice(const std::string& deviceName)
{
    // Filter out loop devices, ram disks, and partitions for a cleaner view
    // Users can still see partitions by checking detailed device info if needed

    // Skip loop devices (loop0, loop1, etc.)
    if (deviceName.starts_with("loop"))
    {
        return false;
    }

    // Skip ram disks
    if (deviceName.starts_with("ram"))
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
    if (!deviceName.empty() && std::isdigit(static_cast<unsigned char>(deviceName.back())))
    {
        // Exception: NVMe devices like "nvme0n1" end with a digit but are whole devices
        // Include nvme0n1, nvme1n1, etc. but skip partitions like sda1, sda2, nvme0n1p1
        return deviceName.contains("nvme") && !deviceName.contains('p');
    }

    return true;
}

SystemDiskCounters LinuxDiskProbe::read()
{
    SystemDiskCounters result;

    std::ifstream diskstats("/proc/diskstats");
    if (!diskstats.is_open())
    {
        spdlog::warn("LinuxDiskProbe: Failed to open /proc/diskstats");
        return result;
    }

    std::string line;
    while (std::getline(diskstats, line))
    {
        std::istringstream iss(line);

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

        // Filter devices
        if (!shouldIncludeDevice(deviceName))
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
        disk.sectorSize = 512;        // Linux typically reports in 512-byte sectors
        disk.isPhysicalDevice = true; // Filtered devices are considered "physical" for our purposes

        result.disks.push_back(disk);
    }

    spdlog::debug("LinuxDiskProbe: read {} devices", result.disks.size());
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
