#pragma once

#include "Platform/GPUTypes.h"
#include "Platform/IGPUProbe.h"

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Platform
{

class DXGIGPUProbe;
class NVMLGPUProbe;
class PDHGPUProbe;

/// Composite Windows GPU probe that delegates to vendor-specific probes.
/// - DXGI: Basic GPU enumeration (all vendors)
/// - NVML: NVIDIA-specific sensors (temp, power, clocks, fan) and VRAM
/// - PDH: adapter and per-process GPU utilization, and memory, via Performance Counters (all
///   vendors, NVIDIA included, so every adapter's % means what Task Manager's does, #1264)
///
/// rescanGPUs() picks up a changed adapter set: DXGI's full rescan replaces its factory once it is
/// no longer current, and the re-enumeration that follows restarts NVML if the NVIDIA adapters
/// changed, re-matches NVML devices to adapters by PCI location (#1241) and rebuilds the LUID maps
/// PDH's figures are matched by. NVML itself restarts after a lost GPU, and a quick rescan reports a
/// GPU asleep at enumeration that has woken, so it gets its own sensors (#1294).
class WindowsGPUProbe : public IGPUProbe
{
  public:
    WindowsGPUProbe();
    ~WindowsGPUProbe() override;

    // Rule of 5
    WindowsGPUProbe(const WindowsGPUProbe&) = delete;
    WindowsGPUProbe& operator=(const WindowsGPUProbe&) = delete;
    WindowsGPUProbe(WindowsGPUProbe&&) = delete;
    WindowsGPUProbe& operator=(WindowsGPUProbe&&) = delete;

    /// @note enumerateGPUs() must be called before readGPUCounters() to populate
    /// the DXGI→LUID mapping required for per-adapter PDH utilization merging.
    /// Without a prior call to enumerateGPUs(), the PDH per-adapter utilization
    /// merge is unavailable, so PDH-backed adapter utilization will be missing
    /// (unread, a gap). NVML supplies an NVIDIA adapter's utilizationPercent only
    /// when PDH is unavailable altogether (#1264).
    /// A debug message may also be logged when PDH adapter utilization data is
    /// present but the DXGI→LUID mapping needed to merge that data is missing.
    [[nodiscard]] std::vector<GPUInfo> enumerateGPUs() override;
    [[nodiscard]] std::vector<GPUCounters> readGPUCounters() override;
    [[nodiscard]] std::vector<ProcessGPUCounters> readProcessGPUCounters() override;
    [[nodiscard]] GPUCapabilities capabilities() const override;
    [[nodiscard]] bool rescanGPUs(GPURescan depth) override;

  private:
    // Test-only: swaps in sub-probes backed by fakes (test_WindowsGPURescan.cpp).
    friend struct WindowsGPUProbeTestAccessor;

    /// Restart NVML when the NVIDIA adapters in @p dxgiGPUs differ from the last enumeration's.
    void restartNVMLIfNVIDIAAdaptersChanged(const std::vector<GPUInfo>& dxgiGPUs);

    [[nodiscard]] std::unordered_set<std::string>
    mergeNVMLEnhancements(std::vector<GPUCounters>& dxgiCounters, std::unordered_set<std::string>& nvmlMemoryIds, bool takeUtilization);
    void mergePDHAdapterUtilization(std::vector<GPUCounters>& dxgiCounters, const std::unordered_set<std::string>& nvmlSourcedIds);

    std::unique_ptr<DXGIGPUProbe> m_DXGIProbe;
    std::unique_ptr<NVMLGPUProbe> m_NVMLProbe;
    // Two PDH probes, one per sampler: m_PDHProbe for per-process counters (process sampler),
    // m_PDHAdapterProbe for adapter utilization (system sampler). PDH computes rates between
    // consecutive collects on a query, so with one shared query each sampler's reading covered
    // only the slice since the *other* sampler's collect (#1034). Each query now spans its own
    // sampler's whole interval. (Access was already serialized by GPUModel's m_ProbeMutex; this
    // is about the measurement window, not thread safety.) Each adds only its role's counters, so
    // neither collects the other's memory counters or builds aggregates it discards (#1175).
    std::unique_ptr<PDHGPUProbe> m_PDHProbe;
    std::unique_ptr<PDHGPUProbe> m_PDHAdapterProbe;

    // Map DXGI GPU index to NVML GPU index (for merging data)
    std::unordered_map<uint32_t, uint32_t> m_DXGIToNVMLMap;

    // NVML device ids in enumeration order -- the order m_DXGIToNVMLMap's NVML indices refer to.
    std::vector<std::string> m_NVMLEnumeratedIds;

    // Map DXGI GPU id ("GPU0") to LUID-based id ("GPU_0x00000000_0x0000D3A0")
    // Built during enumerateGPUs(), used in mergePDHAdapterUtilization()
    // to assign per-GPU utilization from PDH counters (which are keyed by LUID)
    std::unordered_map<std::string, std::string> m_DXGIIdToLuidId;

    // Map DXGI GPU id ("GPU0") to whether it is integrated, which decides whether its memory in
    // use is the shared or the dedicated segment. Built during enumerateGPUs().
    std::unordered_map<std::string, bool> m_DXGIIdIsIntegrated;

    // The NVIDIA adapters' LUIDs at the last enumeration, sorted; unset before the first. A change
    // (an NVIDIA GPU added or removed, or re-created under a new LUID by a driver update or reset)
    // restarts NVML, whose device list is fixed when it starts (#1294).
    std::optional<std::vector<std::string>> m_NVIDIAAdapterLuids;
};

} // namespace Platform
