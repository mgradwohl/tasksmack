#pragma once

#include "Platform/GPUTypes.h"
#include "Platform/IGPUProbe.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Platform
{

/// Linux DRM (Direct Rendering Manager) GPU probe for Intel GPUs.
/// Uses sysfs (/sys/class/drm) for GPU enumeration and basic metrics.
/// Supports Intel integrated and discrete GPUs via i915/xe drivers.
/// An optional custom DRM base path can be provided for unit-testing with
/// a synthetic sysfs directory tree instead of the real /sys filesystem.
class DRMGPUProbe : public IGPUProbe
{
  public:
    explicit DRMGPUProbe(std::string drmBasePath = "/sys/class/drm");
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

    [[nodiscard]] bool isAvailable() const
    {
        return m_Available;
    }

  private:
    struct DRMCard
    {
        std::string cardPath;     // e.g., /sys/class/drm/card0
        std::string devicePath;   // e.g., /sys/class/drm/card0/device
        std::string hwmonPath;    // e.g., /sys/class/drm/card0/device/hwmon/hwmon0
        uint32_t cardIndex{0};    // card0 -> 0, card1 -> 1
        bool isRenderOnly{false}; // renderD* nodes are compute-only
        std::string driver;       // i915, xe, amdgpu, nouveau, etc.
        std::string gpuId;        // Unique ID for tracking
    };

    bool initialize();
    [[nodiscard]] std::vector<DRMCard> discoverDRMCards() const;
    [[nodiscard]] static bool isIntelGPU(const DRMCard& card);
    [[nodiscard]] static std::string readSysfsString(const std::string& path);
    [[nodiscard]] static uint64_t readSysfsUint64(const std::string& path);
    [[nodiscard]] static std::string findHwmonPath(const std::string& devicePath);
    [[nodiscard]] static std::string getVendorName(const std::string& vendorId);
    [[nodiscard]] static uint32_t parseHexUint32(const std::string& hexStr);
    /// Integrated vs discrete (#1113). Dedicated memory, or a 3D-controller class, means discrete;
    /// otherwise an Intel GPU's PCI bus decides when known -- every Intel iGPU is a root-complex
    /// integrated endpoint on bus 0 (00:02.0), while Arc/discrete cards sit behind a PCIe port on a
    /// non-zero bus. i915 exposes no dedicated-memory file at all, so before this an Arc on i915
    /// classified as integrated. Without a bus, the class/VRAM fallback applies as before.
    [[nodiscard]] static bool
    detectIsIntegrated(const std::string& vendorId, uint32_t pciClass, uint64_t vramTotal, std::optional<uint32_t> pciBus);
    /// The bus number of a sysfs PCI address ("0000:03:00.0" -> 3), or nullopt if `address` isn't one.
    [[nodiscard]] static std::optional<uint32_t> pciBusFromAddress(std::string_view address);
    /// Dedicated (device-local) memory, in bytes, from whichever driver file the card has; 0 if none.
    [[nodiscard]] static uint64_t readVramTotal(const DRMCard& card);
    [[nodiscard]] GPUInfo cardToGPUInfo(const DRMCard& card) const;

    bool m_Available{false};
    std::vector<DRMCard> m_Cards;
    std::string m_DrmBasePath; // Injectable base path for testing
};

} // namespace Platform
