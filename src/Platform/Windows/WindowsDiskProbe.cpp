// Keep this translation unit parseable on non-Windows platforms
#if defined(_WIN32)

#include "WindowsDiskProbe.h"

#include <spdlog/spdlog.h>

#include <string>
#include <vector>

// clang-format off
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winioctl.h>
// clang-format on

namespace Platform
{

WindowsDiskProbe::WindowsDiskProbe()
{
    spdlog::debug("WindowsDiskProbe: initialized");
}

SystemDiskCounters WindowsDiskProbe::read()
{
    SystemDiskCounters result;
    
    // Windows disk I/O metrics can be obtained through:
    // 1. Performance Counters (PDH) - most accurate
    // 2. GetDiskPerformanceInfo - simpler but requires elevated privileges
    // 3. WMI - flexible but slower
    
    // For now, we'll enumerate logical drives and get basic info
    // A full implementation would use Performance Counters (PDH) for accurate I/O stats
    
    DWORD drives = GetLogicalDrives();
    if (drives == 0)
    {
        spdlog::warn("WindowsDiskProbe: GetLogicalDrives failed");
        return result;
    }
    
    for (int i = 0; i < 26; ++i)
    {
        if ((drives & (1U << i)) == 0U)
        {
            continue;
        }
        
        char driveLetter = static_cast<char>('A' + i);
        std::string drivePath = std::string{driveLetter} + ":\\";
        
        UINT driveType = GetDriveTypeA(drivePath.c_str());
        
        // Only include fixed drives (skip removable, network, etc.)
        if (driveType != DRIVE_FIXED)
        {
            continue;
        }
        
        // Note: To get actual I/O counters on Windows, we would need to:
        // 1. Use PDH (Performance Data Helper) to query performance counters
        // 2. Query "\PhysicalDisk(*)\Disk Reads/sec", "\PhysicalDisk(*)\Disk Writes/sec", etc.
        // 3. Or use GetDiskPerformanceInfo (requires admin privileges)
        //
        // For this initial implementation, we create placeholder entries
        // A full implementation should use PDH for accurate metrics
        
        DiskCounters disk;
        disk.deviceName = std::string{driveLetter} + ":";
        disk.readsCompleted = 0;
        disk.readSectors = 0;
        disk.readTimeMs = 0;
        disk.writesCompleted = 0;
        disk.writeSectors = 0;
        disk.writeTimeMs = 0;
        disk.ioInProgressMs = 0;
        disk.ioTimeMs = 0;
        disk.weightedIoTimeMs = 0;
        disk.sectorSize = 512;
        disk.isPhysicalDevice = true;
        
        result.disks.push_back(disk);
    }
    
    spdlog::debug("WindowsDiskProbe: found {} fixed drives", result.disks.size());
    return result;
}

DiskCapabilities WindowsDiskProbe::capabilities() const
{
    DiskCapabilities caps;
    caps.hasDiskStats = true; // We enumerate drives, but...
    caps.hasReadWriteBytes = false; // ...we don't have I/O counters yet
    caps.hasIoTime = false;
    caps.hasDeviceInfo = true;
    caps.canFilterPhysical = true;
    return caps;
}

} // namespace Platform

#endif // _WIN32
