#include "DRMGPUProbe.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

namespace Platform
{

namespace fs = std::filesystem;

DRMGPUProbe::DRMGPUProbe()
{
    m_Available = initialize();
    if (m_Available)
    {
        spdlog::debug("DRMGPUProbe: Initialized successfully, found {} DRM card(s)", m_Cards.size());
    }
    else
    {
        spdlog::debug("DRMGPUProbe: No compatible DRM cards found");
    }
}

bool DRMGPUProbe::initialize()
{
    // Discover all DRM cards
    auto cards = discoverDRMCards();

    // Filter to only Intel GPUs (i915, xe drivers)
    for (const auto& card : cards)
    {
        if (isIntelGPU(card))
        {
            m_Cards.push_back(card);
            spdlog::debug("DRMGPUProbe: Found Intel GPU at {}", card.cardPath);
        }
    }

    return !m_Cards.empty();
}

std::vector<DRMGPUProbe::DRMCard> DRMGPUProbe::discoverDRMCards() const
{
    std::vector<DRMCard> cards;

    const std::string drmPath = "/sys/class/drm";
    if (!fs::exists(drmPath))
    {
        spdlog::debug("DRMGPUProbe: /sys/class/drm not found");
        return cards;
    }

    // Iterate over /sys/class/drm/card* entries
    for (const auto& entry : fs::directory_iterator(drmPath))
    {
        const std::string cardName = entry.path().filename().string();

        // Only process card* entries (skip cardX-* connectors and renderD* for now)
        if (!cardName.starts_with("card") || cardName.find('-') != std::string::npos)
        {
            continue;
        }

        // Skip renderD* nodes (compute-only, no display)
        if (cardName.starts_with("renderD"))
        {
            continue;
        }

        DRMCard card;
        card.cardPath = entry.path().string();
        card.devicePath = card.cardPath + "/device";

        // Extract card index (card0 -> 0, card1 -> 1)
        try
        {
            card.cardIndex = static_cast<uint32_t>(std::stoul(cardName.substr(4)));
        }
        catch (...)
        {
            continue; // Invalid card name format
        }

        // Check if device symlink exists
        if (!fs::exists(card.devicePath))
        {
            spdlog::debug("DRMGPUProbe: Skipping {} - no device symlink", cardName);
            continue;
        }

        // Read driver name from /sys/class/drm/cardX/device/driver
        const std::string driverLink = card.devicePath + "/driver";
        if (fs::is_symlink(driverLink))
        {
            const auto driverTarget = fs::read_symlink(driverLink);
            card.driver = driverTarget.filename().string();
        }

        // Find hwmon directory for temperature sensors
        card.hwmonPath = findHwmonPath(card.devicePath);

        // Generate unique GPU ID (use PCI address if available)
        const std::string pciPath = card.devicePath;
        if (fs::is_symlink(pciPath))
        {
            const auto target = fs::read_symlink(pciPath);
            card.gpuId = target.filename().string(); // e.g., 0000:00:02.0
        }
        else
        {
            card.gpuId = cardName; // Fallback to cardX
        }

        cards.push_back(card);
    }

    return cards;
}

bool DRMGPUProbe::isIntelGPU(const DRMCard& card) const
{
    // Intel GPUs use i915 (legacy/current) or xe (future) drivers
    return card.driver == "i915" || card.driver == "xe";
}

std::string DRMGPUProbe::readSysfsString(const std::string& path) const
{
    std::ifstream file(path);
    if (!file.is_open())
    {
        return "";
    }

    std::string value;
    std::getline(file, value);

    // Trim whitespace
    const auto start = value.find_first_not_of(" \t\n\r");
    const auto end = value.find_last_not_of(" \t\n\r");

    if (start == std::string::npos)
    {
        return "";
    }

    return value.substr(start, end - start + 1);
}

uint64_t DRMGPUProbe::readSysfsUint64(const std::string& path) const
{
    const std::string valueStr = readSysfsString(path);
    if (valueStr.empty())
    {
        return 0;
    }

    try
    {
        return std::stoull(valueStr);
    }
    catch (...)
    {
        return 0;
    }
}

std::string DRMGPUProbe::findHwmonPath(const std::string& devicePath) const
{
    const std::string hwmonDir = devicePath + "/hwmon";
    if (!fs::exists(hwmonDir) || !fs::is_directory(hwmonDir))
    {
        return "";
    }

    // Find first hwmonX directory
    for (const auto& entry : fs::directory_iterator(hwmonDir))
    {
        const std::string hwmonName = entry.path().filename().string();
        if (hwmonName.starts_with("hwmon"))
        {
            return entry.path().string();
        }
    }

    return "";
}

std::string DRMGPUProbe::getVendorName(const std::string& vendorId) const
{
    // Intel PCI vendor ID is 0x8086
    if (vendorId.contains("8086"))
    {
        return "Intel";
    }

    return "Unknown";
}

GPUInfo DRMGPUProbe::cardToGPUInfo(const DRMCard& card) const
{
    GPUInfo info{};
    info.id = card.gpuId;

    // Read vendor ID from sysfs
    const std::string vendorPath = card.devicePath + "/vendor";
    const std::string vendorId = readSysfsString(vendorPath);
    info.vendor = getVendorName(vendorId);

    // Read device name from sysfs (PCI device string)
    const std::string devicePath = card.devicePath + "/device";
    const std::string deviceId = readSysfsString(devicePath);

    // Try to read a human-readable name from uevent
    const std::string ueventPath = card.devicePath + "/uevent";
    std::string deviceName = readSysfsString(ueventPath);

    // If uevent doesn't give us a good name, use PCI IDs
    if (deviceName.empty() || !deviceName.contains("PCI_ID"))
    {
        deviceName = "Intel GPU (" + vendorId + ":" + deviceId + ")";
    }
    else
    {
        // Extract device name from uevent if available
        // Format: PCI_ID=8086:XXXX
        const auto pciIdPos = deviceName.find("PCI_ID=");
        if (pciIdPos != std::string::npos)
        {
            const auto idStr = deviceName.substr(pciIdPos + 7, 9); // 8086:XXXX
            deviceName = "Intel GPU (" + idStr + ")";
        }
    }

    info.name = deviceName;

    // Determine if integrated or discrete
    // Intel integrated GPUs typically don't have dedicated memory reported via lspci
    // For now, mark all as integrated (most common case)
    // TODO: More sophisticated detection using PCI class/subclass
    info.isIntegrated = true;

    // Read memory info from driver-specific files
    // i915: /sys/class/drm/cardX/device/mem_info_vram_total (discrete only)
    const std::string vramTotalPath = card.devicePath + "/mem_info_vram_total";
    const uint64_t vramTotal = readSysfsUint64(vramTotalPath);

    if (vramTotal > 0)
    {
        // Has dedicated VRAM, likely discrete
        info.isIntegrated = false;
    }
    else
    {
        // Integrated GPU - no dedicated VRAM, uses system RAM (unknown/shared memory)
        // info.isIntegrated remains true from earlier initialization
    }

    return info;
}

std::vector<GPUInfo> DRMGPUProbe::enumerateGPUs()
{
    std::vector<GPUInfo> gpus;

    for (const auto& card : m_Cards)
    {
        gpus.push_back(cardToGPUInfo(card));
    }

    return gpus;
}

std::vector<GPUCounters> DRMGPUProbe::readGPUCounters()
{
    std::vector<GPUCounters> counters;

    for (const auto& card : m_Cards)
    {
        GPUCounters counter{};
        counter.gpuId = card.gpuId;

        // Read temperature from hwmon (if available)
        if (!card.hwmonPath.empty())
        {
            // Intel GPUs typically expose temp1_input (in millidegrees Celsius)
            const std::string tempPath = card.hwmonPath + "/temp1_input";
            const uint64_t tempMilliC = readSysfsUint64(tempPath);
            if (tempMilliC > 0)
            {
                counter.temperatureC = static_cast<std::int32_t>(tempMilliC / 1000);
            }
        }

        // Read GPU frequency from sysfs
        // i915: /sys/class/drm/cardX/gt_cur_freq_mhz (current frequency)
        // xe: Similar, but path may vary
        const std::string freqPath = card.cardPath + "/gt_cur_freq_mhz";
        const uint64_t freqMhz = readSysfsUint64(freqPath);
        if (freqMhz > 0)
        {
            counter.gpuClockMHz = static_cast<uint32_t>(freqMhz);
        }

        // Read memory info (used/total) if available
        // i915 discrete: /sys/class/drm/cardX/device/mem_info_vram_used
        const std::string vramUsedPath = card.devicePath + "/mem_info_vram_used";
        const uint64_t vramUsed = readSysfsUint64(vramUsedPath);
        if (vramUsed > 0)
        {
            counter.memoryUsedBytes = vramUsed;
        }

        const std::string vramTotalPath = card.devicePath + "/mem_info_vram_total";
        const uint64_t vramTotal = readSysfsUint64(vramTotalPath);
        if (vramTotal > 0)
        {
            counter.memoryTotalBytes = vramTotal;
        }

        // GPU utilization: Not directly available via sysfs for Intel
        // Would require reading i915_gem_objects debugfs or using IGT tools
        // Leave at 0 for now (future enhancement)

        counters.push_back(counter);
    }

    return counters;
}

std::vector<ProcessGPUCounters> DRMGPUProbe::readProcessGPUCounters()
{
    // Per-process GPU metrics are not exposed via DRM sysfs for Intel
    // Would require fdinfo parsing or DRM client stats (kernel 5.19+)
    // Not in scope for Phase 5
    return {};
}

GPUCapabilities DRMGPUProbe::capabilities() const
{
    GPUCapabilities caps{};

    if (!m_Available)
    {
        return caps;
    }

    // DRM probe supports temperature and clock speeds for Intel
    caps.hasTemperature = true;
    caps.hasClockSpeeds = true;

    // Memory metrics available for discrete Intel GPUs only
    // (integrated GPUs use system RAM, not tracked separately)
    caps.supportsMultiGPU = m_Cards.size() > 1;

    // Limited capabilities compared to NVML/ROCm
    caps.hasHotspotTemp = false;
    caps.hasPowerMetrics = false;
    caps.hasFanSpeed = false;
    caps.hasPCIeMetrics = false;
    caps.hasEngineUtilization = false;
    caps.hasPerProcessMetrics = false;
    caps.hasEncoderDecoder = false;

    return caps;
}

} // namespace Platform
