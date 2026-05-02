#include "DRMGPUProbe.h"

#include "Platform/GPUTypes.h"

#include <spdlog/spdlog.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace Platform
{

namespace Fs = std::filesystem;

namespace
{
// PCI class codes: full 24-bit value (class | subclass | prog-if), mask off prog-if with 0xFFFF00
// to get the class+subclass pair for comparison.
constexpr uint32_t PCI_CLASS_SUBCLASS_MASK = 0xFFFF00U;

// Display/3D controller PCI class+subclass values (prog-if bits cleared)
constexpr uint32_t PCI_CLASS_VGA_COMPATIBLE = 0x030000U;     // VGA compatible controller
constexpr uint32_t PCI_CLASS_3D_CONTROLLER = 0x030200U;      // 3D controller (compute-only, no display)
constexpr uint32_t PCI_CLASS_DISPLAY_CONTROLLER = 0x038000U; // Display controller (non-VGA)

// PCI vendor IDs
constexpr uint32_t PCI_VENDOR_INTEL = 0x8086U;
constexpr uint32_t PCI_VENDOR_NVIDIA = 0x10DEU;
constexpr uint32_t PCI_VENDOR_AMD = 0x1002U;
} // namespace

DRMGPUProbe::DRMGPUProbe(std::string drmBasePath) : m_DrmBasePath(std::move(drmBasePath))
{
    // Must call initialize() in the body, not the initializer list,
    // because initialize() uses m_Cards which must be constructed first
    // NOLINTNEXTLINE(cppcoreguidelines-prefer-member-initializer)
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

    std::error_code fsErr;
    if (!Fs::is_directory(m_DrmBasePath, fsErr) || fsErr)
    {
        spdlog::debug("DRMGPUProbe: {} is not a directory or not accessible", m_DrmBasePath);
        return cards;
    }

    // Helper to validate DRM card entry names
    const auto isValidCardName = [](const std::string& name) -> bool
    {
        // Only process card* entries, skip cardX-* connectors and renderD* nodes
        return name.starts_with("card") && !name.contains('-') && !name.starts_with("renderD");
    };

    // Iterate over DRM card entries
    Fs::directory_iterator dirIter(m_DrmBasePath, fsErr);
    if (fsErr)
    {
        spdlog::debug("DRMGPUProbe: failed to open {} for iteration: {}", m_DrmBasePath, fsErr.message());
        return cards;
    }
    for (const auto& entry : dirIter)
    {
        const std::string cardName = entry.path().filename().string();

        // Only process card* entries (skip cardX-* connectors and renderD* for now)
        if (!isValidCardName(cardName))
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
        if (!Fs::exists(card.devicePath))
        {
            spdlog::debug("DRMGPUProbe: Skipping {} - no device symlink", cardName);
            continue;
        }

        // Read driver name from /sys/class/drm/cardX/device/driver
        const std::string driverLink = card.devicePath + "/driver";
        if (Fs::is_symlink(driverLink))
        {
            const auto driverTarget = Fs::read_symlink(driverLink);
            card.driver = driverTarget.filename().string();
        }

        // Find hwmon directory for temperature sensors
        card.hwmonPath = findHwmonPath(card.devicePath);

        // Generate unique GPU ID (use PCI address if available)
        const std::string pciPath = card.devicePath;
        if (Fs::is_symlink(pciPath))
        {
            const auto target = Fs::read_symlink(pciPath);
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

bool DRMGPUProbe::isIntelGPU(const DRMCard& card)
{
    // Intel GPUs use i915 (legacy/current) or xe (future) drivers
    return card.driver == "i915" || card.driver == "xe";
}

std::string DRMGPUProbe::readSysfsString(const std::string& path)
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

uint64_t DRMGPUProbe::readSysfsUint64(const std::string& path)
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

std::string DRMGPUProbe::findHwmonPath(const std::string& devicePath)
{
    const std::string hwmonDir = devicePath + "/hwmon";
    if (!Fs::exists(hwmonDir) || !Fs::is_directory(hwmonDir))
    {
        return "";
    }

    // Find first hwmonX directory
    for (const auto& entry : Fs::directory_iterator(hwmonDir))
    {
        const std::string hwmonName = entry.path().filename().string();
        if (hwmonName.starts_with("hwmon"))
        {
            return entry.path().string();
        }
    }

    return "";
}

std::string DRMGPUProbe::getVendorName(const std::string& vendorId)
{
    const uint32_t id = parseHexUint32(vendorId);
    switch (id)
    {
    case PCI_VENDOR_INTEL:
        return "Intel";
    case PCI_VENDOR_NVIDIA:
        return "NVIDIA";
    case PCI_VENDOR_AMD:
        return "AMD";
    default:
        return "Unknown";
    }
}

uint32_t DRMGPUProbe::parseHexUint32(const std::string& hexStr)
{
    if (hexStr.empty())
    {
        return 0;
    }
    try
    {
        // std::stoul handles the "0x" prefix automatically with base 16.
        // PCI class codes are 24-bit and vendor IDs are 16-bit, so the result
        // always fits in uint32_t and the static_cast is safe.
        return static_cast<uint32_t>(std::stoul(hexStr, nullptr, 16));
    }
    catch (...)
    {
        // Malformed or unexpected sysfs content: treat as unknown.
        spdlog::debug("DRMGPUProbe: failed to parse hex value '{}'", hexStr);
        return 0;
    }
}

bool DRMGPUProbe::detectIsIntegrated(const std::string& vendorId, uint32_t pciClass, uint64_t vramTotal)
{
    const uint32_t classSubclass = (pciClass & PCI_CLASS_SUBCLASS_MASK);
    const uint32_t vendor = parseHexUint32(vendorId);

    // 3D controller (compute-only, no display output) is always discrete.
    // Examples: NVIDIA MX/RTX laptop cards, Intel Arc in compute mode.
    if (classSubclass == PCI_CLASS_3D_CONTROLLER)
    {
        return false;
    }

    // VGA-compatible controllers have display output.
    // Intel VGA GPUs are integrated unless they carry dedicated VRAM (e.g., Arc discrete).
    // Non-Intel VGA controllers (NVIDIA/AMD) are discrete.
    // If the vendor is unknown (e.g., /vendor file missing), fall back conservatively to
    // VRAM presence rather than incorrectly classifying as discrete.
    if (classSubclass == PCI_CLASS_VGA_COMPATIBLE)
    {
        if (vendor == PCI_VENDOR_INTEL || vendor == 0)
        {
            // Intel iGPU (or unknown vendor — conservative): integrated unless VRAM is present.
            return vramTotal == 0;
        }
        return false; // NVIDIA/AMD VGA controllers are discrete
    }

    // Display controllers that are not VGA-compatible (e.g., Intel Arc on some platforms).
    // Use VRAM presence as the tiebreaker for Intel; others are discrete.
    // Same conservative fallback for unknown vendor.
    if (classSubclass == PCI_CLASS_DISPLAY_CONTROLLER)
    {
        if (vendor == PCI_VENDOR_INTEL || vendor == 0)
        {
            return vramTotal == 0;
        }
        return false;
    }

    // Unrecognised PCI class: fall back to VRAM presence.
    // No VRAM → assume integrated (conservative default).
    return vramTotal == 0;
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
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
        deviceName = info.vendor + " GPU (" + vendorId + ":" + deviceId + ")";
    }
    else
    {
        // Extract device name from uevent if available
        // Format: PCI_ID=8086:XXXX
        const auto pciIdPos = deviceName.find("PCI_ID=");
        if (pciIdPos != std::string::npos)
        {
            const auto idStr = deviceName.substr(pciIdPos + 7, 9); // 8086:XXXX
            deviceName = info.vendor + " GPU (" + idStr + ")";
        }
    }

    info.name = deviceName;

    // Read memory info from driver-specific files.
    // i915: /sys/class/drm/cardX/device/mem_info_vram_total (discrete only)
    const std::string vramTotalPath = card.devicePath + "/mem_info_vram_total";
    const uint64_t vramTotal = readSysfsUint64(vramTotalPath);

    // Read PCI class from sysfs to distinguish integrated from discrete using
    // the PCI class/subclass, with VRAM presence as a secondary signal.
    // /sys/class/drm/cardX/device/class contains the 24-bit PCI class code, e.g. "0x030000".
    const std::string pciClassPath = card.devicePath + "/class";
    const std::string pciClassStr = readSysfsString(pciClassPath);
    const uint32_t pciClass = parseHexUint32(pciClassStr);

    info.isIntegrated = detectIsIntegrated(vendorId, pciClass, vramTotal);

    return info;
}

std::vector<GPUInfo> DRMGPUProbe::enumerateGPUs()
{
    std::vector<GPUInfo> gpus;
    gpus.reserve(m_Cards.size());

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
