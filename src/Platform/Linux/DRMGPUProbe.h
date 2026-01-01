#pragma once

#include "Platform/GPUTypes.h"
#include "Platform/IGPUProbe.h"

#include <string>
#include <vector>

namespace Platform
{

/// Linux DRM (Direct Rendering Manager) GPU probe for Intel GPUs.
/// Uses sysfs (/sys/class/drm) for GPU enumeration and basic metrics.
/// Supports Intel integrated and discrete GPUs via i915/xe drivers.
class DRMGPUProbe : public IGPUProbe
{
  public:
    DRMGPUProbe();
    ~DRMGPUProbe() override = default;

    // Rule of 5
    DRMGPUProbe(const DRMGPUProbe&) = delete;
    DRMGPUProbe& operator=(const DRMGPUProbe&) = delete;
    DRMGPUProbe(DRMGPUProbe&&) = delete;
    DRMGPUProbe& operator=(DRMGPUProbe&&) = delete;

    [[nodiscard]] std::vector<GPUInfo> enumerateGPUs() override;
    [[nodiscard]] std::vector<GPUCounters> readGPUCounters() override;
    [[nodiscard]] std::vector<ProcessGPUCounters> readProcessGPUCounters() override;
    [[nodiscard]] GPUCapabilities capabilities() const override;

    [[nodiscard]] bool isAvailable() const { return m_Available; }

  private:
    struct DRMCard
    {
        std::string cardPath;        // e.g., /sys/class/drm/card0
        std::string devicePath;      // e.g., /sys/class/drm/card0/device
        std::string hwmonPath;       // e.g., /sys/class/drm/card0/device/hwmon/hwmon0
        uint32_t cardIndex{0};       // card0 -> 0, card1 -> 1
        bool isRenderOnly{false};    // renderD* nodes are compute-only
        std::string driver;          // i915, xe, amdgpu, nouveau, etc.
        std::string gpuId;           // Unique ID for tracking
    };

    bool initialize();
    [[nodiscard]] std::vector<DRMCard> discoverDRMCards() const;
    [[nodiscard]] bool isIntelGPU(const DRMCard& card) const;
    [[nodiscard]] std::string readSysfsString(const std::string& path) const;
    [[nodiscard]] uint64_t readSysfsUint64(const std::string& path) const;
    [[nodiscard]] std::string findHwmonPath(const std::string& devicePath) const;
    [[nodiscard]] std::string getVendorName(const std::string& vendorId) const;
    [[nodiscard]] GPUInfo cardToGPUInfo(const DRMCard& card) const;

    bool m_Available{false};
    std::vector<DRMCard> m_Cards;
};

} // namespace Platform
