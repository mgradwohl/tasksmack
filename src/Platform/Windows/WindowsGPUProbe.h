#pragma once

#include "Platform/GPUTypes.h"
#include "Platform/IGPUProbe.h"
#include "Platform/Windows/WindowsDriverNodes.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Platform
{

class DisplayDevicePower;
class DXGIGPUProbe;
class NVMLGPUProbe;
class PDHGPUProbe;
struct D3DKMTAdapterFunctions;

/// Composite Windows GPU probe that delegates to vendor-specific probes.
/// - DXGI: Basic GPU enumeration (all vendors)
/// - NVML: NVIDIA-specific sensors (temp, power, clocks, fan, video encoder/decoder) and VRAM
/// - D3DKMT adapter perf data: temperature, clock and fan for the GPUs NVML doesn't cover (AMD,
///   Intel, Qualcomm, or NVIDIA without NVML), from the kernel graphics adapter, the figures Task
///   Manager draws (#2586, D3DKMTAdapterPerfData.h). NVML is preferred where it has the adapter.
/// - PDH: adapter and per-process GPU utilization, and memory, via Performance Counters (all
///   vendors, NVIDIA included, so every adapter's % means what Task Manager's does, #1264)
///
/// rescanGPUs() picks up a changed adapter set: DXGI's full rescan replaces its factory once it is
/// no longer current, and the re-enumeration that follows restarts NVML if the NVIDIA adapters
/// changed, re-matches NVML devices to adapters by PCI location (#1241) and rebuilds the LUID maps
/// PDH's figures are matched by. NVML itself restarts after a lost GPU, and a quick rescan reports a
/// GPU asleep at enumeration that has woken, so it gets its own sensors (#1294).
///
/// The NPUs and other MCDM compute accelerators, which DXGI never lists, come from DXCore
/// (DXGIGPUProbe::enumerateAccelerators()) and are listed after the GPUs with their DeviceKind
/// (#1799). They share the GPUs' PDH figures by LUID -- adapter utilization and memory, and
/// per-process samples, which are tagged with the adapter's kind -- and have no sensors. Every
/// adapter's driver version is read from its PnP device node.
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

    /// The last readProcessGPUCounters()'s per-process samples of adapters not listed: the Basic
    /// Render Driver, or an NPU or other compute accelerator DXCore couldn't enumerate (an older
    /// Windows), one per process and adapter LUID (#1826). readProcessGPUCounters() leaves them out of
    /// every figure; an enumerated NPU's samples are returned by it instead, tagged with their kind
    /// (#1799). Call it under the same serialization as readProcessGPUCounters() (GPUModel's probe lock).
    [[nodiscard]] std::vector<OtherAdapterProcessUsage> otherAdapterUsage() const;

  private:
    // Test-only: swaps in sub-probes backed by fakes (test_WindowsGPURescan.cpp).
    friend struct WindowsGPUProbeTestAccessor;

    /// Restart NVML when the NVIDIA adapters in @p dxgiGPUs differ from the last enumeration's,
    /// unless a rescan restarted it since that enumeration.
    void restartNVMLIfNVIDIAAdaptersChanged(const std::vector<GPUInfo>& dxgiGPUs);

    [[nodiscard]] std::unordered_set<std::string> mergeNVMLEnhancements(std::vector<GPUCounters>& dxgiCounters,
                                                                        std::unordered_set<std::string>& nvmlMemoryIds,
                                                                        bool takeUtilization,
                                                                        const std::unordered_set<std::string>& idleDevices);
    void mergePDHAdapterUtilization(std::vector<GPUCounters>& dxgiCounters, const std::unordered_set<std::string>& nvmlSourcedIds);

    /// Decide, at enumeration, which of @p gpus (DXGI's, in enumeration order) get their sensors from
    /// D3DKMT adapter perf data (#2586): every GPU without an NVML device, by one reading each
    /// (AdapterPerfDataMath.h's adapterPerfSensorCapabilities()), which sets its sensorCapabilities
    /// and lists it in m_PerfAdapters. A GPU asleep now isn't read -- the query could wake it -- and
    /// goes to m_PerfAsleepAtEnumeration with no sensors, for a quick rescan to report once it wakes.
    void assignAdapterPerfSensors(std::vector<GPUInfo>& gpus);
    /// Read each m_PerfAdapters adapter's perf data this sample and fill its counter's temperature,
    /// clock and fan; a failed read, or an adapter asleep now (left alone, and marked suspended), is a
    /// gap for each of its sensors.
    void mergeAdapterPerfSensors(std::vector<GPUCounters>& counters);
    /// Whether the GPU at @p location is asleep (m_IsAsleep); false without a location.
    [[nodiscard]] bool isAdapterAsleep(const std::optional<PciLocation>& location) const;

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

    // The LUIDs of the adapters listed at the last enumeration (listedAdapterLuids()): the GPUs, and
    // the NPUs and compute accelerators DXCore found (#1799). Rebuilt with m_DXGIIdToLuidId, so a
    // rescan or hot-plug updates it. Per-process PDH samples of any other adapter -- the Basic Render
    // Driver, or an accelerator DXCore couldn't enumerate -- go to m_OtherAdapterUsage rather than
    // being counted as GPU work (#1826); before the first enumeration no adapter is listed, so every
    // sample goes there.
    std::unordered_set<std::string> m_ListedAdapterLuids;

    // Each listed adapter's kind by LUID, which per-process samples are tagged with (#1799)
    std::unordered_map<std::string, DeviceKind> m_AdapterKindByLuid;

    // The last readProcessGPUCounters()'s samples of adapters not in m_ListedAdapterLuids (#1826)
    std::vector<OtherAdapterProcessUsage> m_OtherAdapterUsage;

    // Map adapter id to whether its memory figures count its shared segment (an integrated GPU, or
    // an NPU without memory of its own) rather than dedicated memory. Built during enumerateGPUs().
    std::unordered_map<std::string, bool> m_DXGIIdIsIntegrated;

    // Reads the PnP device nodes every adapter's driver version comes from (#1799): SetupAPI's PCI
    // devices in production, a fake in tests.
    std::vector<WindowsGraphics::DriverNode> (*m_ListDriverNodes)() = nullptr;

    // The NVIDIA adapters at the last enumeration (nvidiaAdapterFingerprint()); unset before the
    // first. A change (an NVIDIA GPU added or removed, re-created under a new LUID by a driver update
    // or reset, or another card under a reused LUID) restarts NVML, whose device list is fixed when
    // it starts (#1294).
    std::optional<std::vector<std::string>> m_NVIDIAAdapters;

    // NVML was restarted by a rescan since the last enumeration (after a lost GPU, or a retried
    // start), so a changed NVIDIA LUID set at that enumeration -- the same driver reset, seen by
    // DXGI -- doesn't restart it a second time.
    bool m_NVMLRestartedSinceEnumeration{false};

    // Each matched NVML device's adapter id, by device id, from the last enumeration.
    std::unordered_map<std::string, std::string> m_NVMLAdapterIds;
    // The last sample's PDH utilization per adapter id, where PDH read one, and when NVML last read
    // each NVIDIA device: an adapter PDH saw idle isn't queried through NVML (#1265,
    // nvmlDevicesToLeaveIdle()).
    std::unordered_map<std::string, double> m_LastPDHUtilization;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> m_NVMLLastRead;

    // The kernel adapter calls the perf-data reads make (#2586): gdi32's exports, or fakes in tests.
    std::unique_ptr<D3DKMTAdapterFunctions> m_PerfFunctions;
    // Whether a GPU is asleep, from the PnP manager's record of its power state, so a sleeping
    // AMD or Intel dGPU is never woken by a perf-data query (#1265): m_DevicePower's answer in
    // production, a fake in tests.
    std::shared_ptr<DisplayDevicePower> m_DevicePower;
    std::function<bool(const PciLocation&)> m_IsAsleep;

    /// One adapter whose sensors come from D3DKMT adapter perf data: the LUID its kernel adapter is
    /// opened by, where it sits (for the sleep check) and which sensors it reported at enumeration.
    struct PerfAdapter
    {
        std::uint32_t luidHighPart = 0;
        std::uint32_t luidLowPart = 0;
        std::optional<PciLocation> pciLocation;
        GPUCapabilities sensors{};
    };
    // By adapter id (GPUInfo::id), rebuilt by every enumerateGPUs().
    std::unordered_map<std::string, PerfAdapter> m_PerfAdapters;
    // The adapters asleep at the last enumeration, whose sensor set is still unknown, by id: a quick
    // rescan reports a change once one is awake, so the enumeration that follows reads it (#1289).
    std::unordered_map<std::string, PciLocation> m_PerfAsleepAtEnumeration;
};

} // namespace Platform
