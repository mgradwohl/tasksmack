#include "WindowsGPUProbe.h"

#include "D3DKMTAdapterPerfData.h"
#include "DXGIAdapterLocation.h"
#include "DXGIGPUProbe.h"
#include "DXGIGPUProbeMath.h"
#include "DisplayDevicePower.h"
#include "NVMLGPUProbe.h"
#include "PDHGPUProbe.h"
#include "Platform/GPUTypes.h"
#include "Platform/Windows/AdapterPerfDataMath.h"
#include "Platform/Windows/WindowsDriverNodes.h"
#include "WindowsGPUProbeMath.h"

#include <spdlog/spdlog.h>

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <setupapi.h>
// clang-format on

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Platform
{

namespace
{

/// Every present PCI device node's first hardware id, bus location and driver version, whatever its
/// class: a GPU is a Display device and an NPU a ComputeAccelerator one, and a compute accelerator
/// may be in another class again (#1799).
[[nodiscard]] std::vector<WindowsGraphics::DriverNode> listPciDriverNodes()
{
    return WindowsGraphics::listDriverNodes(nullptr, L"PCI", DIGCF_PRESENT | DIGCF_ALLCLASSES);
}

} // namespace

WindowsGPUProbe::WindowsGPUProbe()
    : m_DXGIProbe(std::make_unique<DXGIGPUProbe>()),
      m_NVMLProbe(std::make_unique<NVMLGPUProbe>()),
      m_PDHProbe(std::make_unique<PDHGPUProbe>(PDHGPUProbe::Role::Process)),
      m_PDHAdapterProbe(std::make_unique<PDHGPUProbe>(PDHGPUProbe::Role::Adapter)),
      m_ListDriverNodes(listPciDriverNodes),
      m_PerfFunctions(std::make_unique<D3DKMTAdapterFunctions>()),
      m_DevicePower(std::make_shared<DisplayDevicePower>()),
      m_IsAsleep([power = m_DevicePower](const PciLocation& location) { return power->isAsleep(location); })
{
    std::string probeSummary = "DXGI + D3DKMT perf data";
    if (m_NVMLProbe->isAvailable())
    {
        probeSummary += " + NVML";
    }
    if (m_PDHProbe->isAvailable())
    {
        probeSummary += " + PDH";
    }
    spdlog::debug("WindowsGPUProbe: Initialized with {} probe(s)", probeSummary);
}

WindowsGPUProbe::~WindowsGPUProbe() = default;

std::vector<GPUInfo> WindowsGPUProbe::enumerateGPUs()
{
    // Use DXGI as primary enumeration source (works for all vendors)
    if (m_DXGIProbe)
    {
        auto gpus = m_DXGIProbe->enumerateGPUs();
        restartNVMLIfNVIDIAAdaptersChanged(gpus);
        std::vector<GPUInfo> nvmlGPUs;
        GPUCapabilities nvmlCaps{};
        m_DXGIToNVMLMap.clear();
        m_NVMLAdapterIds.clear();

        // If NVML is available, try to match NVIDIA GPUs for enhanced data
        if (m_NVMLProbe && m_NVMLProbe->isAvailable())
        {
            nvmlGPUs = m_NVMLProbe->enumerateGPUs();
            nvmlCaps = m_NVMLProbe->capabilities();
            spdlog::debug("WindowsGPUProbe: Found {} DXGI GPUs and {} NVML GPUs", gpus.size(), nvmlGPUs.size());

            // Map NVIDIA DXGI adapters to NVML devices by name, each NVML device claimed once so
            // identical cards do not all map to the first (#1040).
            m_DXGIToNVMLMap = mapDXGIToNVML(gpus, nvmlGPUs);
            // Each NVML device's matched adapter id, so PDH's per-adapter readings say which NVML
            // devices to leave idle (#1265).
            m_NVMLAdapterIds = nvmlDeviceAdapterIds(gpus, nvmlGPUs, m_DXGIToNVMLMap);
            // The mapping holds enumeration positions; counter reads are reordered to match by id.
            m_NVMLEnumeratedIds.clear();
            for (const auto& nvmlGPU : nvmlGPUs)
            {
                m_NVMLEnumeratedIds.push_back(nvmlGPU.id);
            }
            for (std::size_t dxgiIdx = 0; dxgiIdx < gpus.size(); ++dxgiIdx)
            {
                if (gpus[dxgiIdx].vendor == "NVIDIA" && !m_DXGIToNVMLMap.contains(static_cast<uint32_t>(dxgiIdx)))
                {
                    spdlog::warn("WindowsGPUProbe: Failed to match DXGI GPU '{}' with any NVML GPU", gpus[dxgiIdx].name);
                }
            }
            spdlog::info("WindowsGPUProbe: Created {} DXGI-to-NVML mappings", m_DXGIToNVMLMap.size());
        }
        else
        {
            spdlog::debug("WindowsGPUProbe: NVML not available for NVIDIA GPU enhancement");
        }

        // Build DXGI id → LUID map for PDH per-GPU utilization matching.
        // PDH process counters use "GPU_0x{High}_0x{Low}" as their gpuId; DXGI
        // stores the same value in GPUInfo::luidId. We need to look up a counter's
        // LUID from its gpuId (its PCI location, "PCI_01:00.0_10DE:2684", or LUID) to match PDH data.
        // Clear before rebuilding because enumerateGPUs() may be called multiple times
        // (e.g., on device change) and the adapter list can change between calls.
        m_DXGIIdToLuidId.clear();
        m_DXGIIdIsIntegrated.clear();
        // Sensor metrics are per adapter: its own NVML device's, or none without one (#1040)...
        assignSensorCapabilities(gpus, nvmlGPUs, m_DXGIToNVMLMap, nvmlCaps);
        // ...unless the kernel adapter reports them, for a GPU without an NVML device (#2586).
        assignAdapterPerfSensors(gpus);

        // The NPUs and compute accelerators DXGI never lists, after the GPUs, so the NVML mapping's
        // positions above still hold (#1799). They have no sensors (enumerateAccelerators()).
        std::vector<GPUInfo> accelerators = m_DXGIProbe->enumerateAccelerators();
        gpus.insert(gpus.end(), std::make_move_iterator(accelerators.begin()), std::make_move_iterator(accelerators.end()));

        // Every adapter's driver version from its PnP device node: DXGI has none, and DXCore's is
        // 0.0.0.0 for an NPU, which has no user-mode driver (#1799).
        if (m_ListDriverNodes != nullptr)
        {
            WindowsGraphics::applyDriverVersions(gpus, m_ListDriverNodes());
        }

        for (const auto& gpu : gpus)
        {
            // The segment its memory in use is read from: shared for an integrated GPU or an NPU
            // without memory of its own (#1164)
            m_DXGIIdIsIntegrated[gpu.id] = gpu.memoryIsShared;
            if (!gpu.luidId.empty())
            {
                m_DXGIIdToLuidId[gpu.id] = gpu.luidId;
                spdlog::debug("WindowsGPUProbe: LUID mapping: {} → {}", gpu.id, gpu.luidId);
            }
        }
        // Which adapters' per-process samples count (#1826), and as which kind (#1799)
        m_ListedAdapterLuids = listedAdapterLuids(gpus);
        m_AdapterKindByLuid = adapterKindsByLuid(gpus);

        return gpus;
    }

    return {};
}

void WindowsGPUProbe::restartNVMLIfNVIDIAAdaptersChanged(const std::vector<GPUInfo>& dxgiGPUs)
{
    std::vector<std::string> nvidiaAdapters = nvidiaAdapterFingerprint(dxgiGPUs);
    const bool changed = m_NVIDIAAdapters.has_value() && *m_NVIDIAAdapters != nvidiaAdapters;
    m_NVIDIAAdapters = std::move(nvidiaAdapters);
    const bool restartedForThisChange = m_NVMLRestartedSinceEnumeration;
    m_NVMLRestartedSinceEnumeration = false;
    // NVML lists the GPUs present when it started, so a new NVIDIA adapter is only seen by a restart
    // (which may wake a sleeping dGPU once, as starting it does) (#1294). A driver reset is seen by
    // both probes in the same rescan: NVML restarts for the lost GPU, and the new LUID changes the
    // set here. NVML started after the reset already lists the GPUs present, so a second shutdown
    // and init for the same change would only cost (and possibly wake) once more.
    if (changed && m_NVMLProbe)
    {
        if (restartedForThisChange)
        {
            spdlog::debug("WindowsGPUProbe: NVIDIA adapters changed; NVML already restarted for it");
            return;
        }
        spdlog::info("WindowsGPUProbe: NVIDIA adapters changed; restarting NVML");
        static_cast<void>(m_NVMLProbe->restart());
    }
}

bool WindowsGPUProbe::rescanGPUs(GPURescan depth)
{
    // Every probe is rescanned, so no short-circuit: one changing doesn't excuse the other. A DXGI
    // change means the adapter set did; the re-enumeration it asks for restarts NVML too if the
    // NVIDIA adapters are among the changes (restartNVMLIfNVIDIAAdaptersChanged()).
    const bool adaptersChanged = m_DXGIProbe && m_DXGIProbe->rescanGPUs(depth);
    bool nvmlChanged = false;
    if (m_NVMLProbe)
    {
        const std::uint64_t restartsBefore = m_NVMLProbe->restartCount();
        // NVML installed and an NVIDIA adapter present, but NVML not running -- it failed to start,
        // or to restart after a lost GPU (a driver mid-reset, say) -- is tried again at the
        // full-rescan rate. Without an NVIDIA adapter it never could start; one appearing changes
        // the adapter set, which restarts NVML anyway.
        const bool hasNVIDIAAdapter = m_NVIDIAAdapters.has_value() && !m_NVIDIAAdapters->empty();
        if (depth == GPURescan::Full && !m_NVMLProbe->isAvailable() && m_NVMLProbe->isLoaded() && hasNVIDIAAdapter)
        {
            nvmlChanged = m_NVMLProbe->restart();
        }
        else
        {
            nvmlChanged = m_NVMLProbe->rescanGPUs(depth);
        }
        // The enumeration this rescan leads to needn't restart NVML again (see
        // restartNVMLIfNVIDIAAdaptersChanged()).
        m_NVMLRestartedSinceEnumeration = m_NVMLRestartedSinceEnumeration || m_NVMLProbe->restartCount() != restartsBefore;
    }
    // A GPU asleep at enumeration, whose perf-data sensors are still unknown, that is awake now: the
    // PnP power state says so without touching it (#1265), and the re-enumeration reads it (#2586).
    // Reported once; the enumeration rebuilds the set.
    bool perfAdapterWoke = false;
    for (auto it = m_PerfAsleepAtEnumeration.begin(); it != m_PerfAsleepAtEnumeration.end();)
    {
        if (!isAdapterAsleep(it->second))
        {
            spdlog::debug("WindowsGPUProbe: Adapter {} woke; re-enumerating for its sensors", it->first);
            perfAdapterWoke = true;
            it = m_PerfAsleepAtEnumeration.erase(it);
        }
        else
        {
            ++it;
        }
    }
    return adaptersChanged || nvmlChanged || perfAdapterWoke;
}

std::vector<GPUCounters> WindowsGPUProbe::readGPUCounters()
{
    if (!m_DXGIProbe)
    {
        return {};
    }

    // Get base counters from DXGI: utilization and memory in use start unread, so an adapter
    // neither NVML nor PDH reads this sample publishes a gap rather than 0% and 0 B (#1245).
    auto counters = m_DXGIProbe->readGPUCounters();

    // Merge NVML sensors for NVIDIA GPUs, filling nvmlMemoryIds with those whose NVML memory read
    // actually succeeded. Utilization is PDH's for every adapter, NVIDIA included, so it means what
    // Task Manager's and the per-process figures do; NVML's is taken (and its GPUs returned) only
    // when PDH can't supply one at all (#1264).
    const bool pdhAvailable = m_PDHAdapterProbe && m_PDHAdapterProbe->isAvailable();
    std::unordered_set<std::string> nvmlSourcedIds;
    std::unordered_set<std::string> nvmlMemoryIds;
    if (m_NVMLProbe && m_NVMLProbe->isAvailable())
    {
        // An NVIDIA GPU PDH saw idle last interval isn't queried through NVML, whose queries could
        // keep a hybrid dGPU from suspending; its previous NVML readings stand (#1265). Without a PDH
        // reading for it, it is queried as before.
        const std::unordered_set<std::string> idleDevices =
            pdhAvailable ? nvmlDevicesToLeaveIdle(m_NVMLAdapterIds, m_LastPDHUtilization, m_NVMLLastRead, std::chrono::steady_clock::now())
                         : std::unordered_set<std::string>{};
        m_NVMLProbe->setIdleDevices(idleDevices);
        nvmlSourcedIds = mergeNVMLEnhancements(counters, nvmlMemoryIds, /*takeUtilization=*/!pdhAvailable, idleDevices);
        // Their memory in use is PDH's current figure, not the repeated NVML one.
        excludeIdleNVMLMemory(nvmlMemoryIds, idleDevices, m_NVMLAdapterIds);
    }

    // The kernel adapter's temperature, clock and fan for the GPUs NVML doesn't cover (#2586).
    mergeAdapterPerfSensors(counters);

    // The NPUs' and compute accelerators' counters, after the GPUs' (whose positions NVML's merge
    // above relies on): their utilization and memory in use are PDH's alone, by LUID, like a GPU's
    // (#1799).
    std::vector<GPUCounters> accelerators = m_DXGIProbe->readAcceleratorCounters();
    counters.insert(counters.end(), std::make_move_iterator(accelerators.begin()), std::make_move_iterator(accelerators.end()));

    // PDH per-adapter utilization matched to each adapter
    mergePDHAdapterUtilization(counters, nvmlSourcedIds);

    // With PDH available NVML's utilization is never taken, so every reading here is PDH's.
    m_LastPDHUtilization.clear();
    if (pdhAvailable)
    {
        for (const auto& counter : counters)
        {
            if (counter.utilizationAvailable)
            {
                m_LastPDHUtilization[counter.gpuId] = counter.utilizationPercent;
            }
        }
    }

    // And their memory in use, from the same collect: adapter-wide, not this process's (#1029).
    if (m_PDHAdapterProbe && m_PDHAdapterProbe->isAvailable())
    {
        assignPDHMemoryToDXGICounters(counters, m_PDHAdapterProbe->adapterMemory(), m_DXGIIdToLuidId, m_DXGIIdIsIntegrated, nvmlMemoryIds);
    }

    return counters;
}

std::unordered_set<std::string> WindowsGPUProbe::mergeNVMLEnhancements(std::vector<GPUCounters>& dxgiCounters,
                                                                       std::unordered_set<std::string>& nvmlMemoryIds,
                                                                       bool takeUtilization,
                                                                       const std::unordered_set<std::string>& idleDevices)
{
    if (!m_NVMLProbe || !m_NVMLProbe->isAvailable())
    {
        spdlog::debug("WindowsGPUProbe::mergeNVMLEnhancements: NVML not available, skipping merge");
        return {};
    }

    // Get NVML counters
    const auto nvmlCounters = m_NVMLProbe->readGPUCounters();
    // Only a real read restarts the idle gate's maximum age (#1265).
    recordNVMLReads(m_NVMLLastRead, nvmlCounters, idleDevices, std::chrono::steady_clock::now());
    if (nvmlCounters.empty())
    {
        spdlog::debug("WindowsGPUProbe::mergeNVMLEnhancements: NVML returned no counters");
        return {};
    }

    spdlog::debug("WindowsGPUProbe::mergeNVMLEnhancements: Have {} DXGI counters and {} NVML counters, {} mappings",
                  dxgiCounters.size(),
                  nvmlCounters.size(),
                  m_DXGIToNVMLMap.size());

    // m_DXGIToNVMLMap holds positions in enumeration order; NVML reads counters in hash order, so
    // put them back in enumeration order by device id first (#1040).
    return mergeNVMLIntoDXGICounters(
        dxgiCounters, orderNVMLCountersByIds(nvmlCounters, m_NVMLEnumeratedIds), m_DXGIToNVMLMap, &nvmlMemoryIds, takeUtilization);
}

void WindowsGPUProbe::mergePDHAdapterUtilization(std::vector<GPUCounters>& dxgiCounters,
                                                 const std::unordered_set<std::string>& nvmlSourcedIds)
{
    // Skip if no PDH. (nvmlSourcedIds is empty whenever PDH is available, #1264.)
    if (!m_PDHAdapterProbe || !m_PDHAdapterProbe->isAvailable())
    {
        return;
    }

    // Collect on this sampler's own query every sample: PDH rates are computed between
    // consecutive collects.
    static_cast<void>(m_PDHAdapterProbe->readProcessGPUCounters());
    if (allGPUsHaveNVMLUtilization(dxgiCounters, nvmlSourcedIds))
    {
        return;
    }

    // The per-adapter figure from that collect: per engine the sum over processes, then the
    // busiest engine (Task Manager's definition), keyed by "GPU_0x{HighPart}_0x{LowPart}" -- the
    // same format as GPUInfo::luidId from DXGI. Summing process totals instead counted parallel
    // engines as if they were serial (#1033).
    if (!m_PDHAdapterProbe->adapterUtilizationCurrent())
    {
        return; // Warm-up or a failed collect: DXGI's counters stay unread, a gap, not 0% (#1245)
    }
    // A successful collect with no engine instances for an adapter means nothing ran on it: an
    // idle GPU reads 0%, not a permanent gap (#1166).
    const auto utilizationByLuid = m_PDHAdapterProbe->adapterUtilization();

    // Assign per-GPU utilization by matching each DXGI counter's LUID-based id
    // to the corresponding PDH bucket. m_DXGIIdToLuidId is populated in
    // enumerateGPUs() and maps each adapter's id to its LUID ("GPU_0x00000000_0x0000D3A0").
    assignPDHUtilizationToDXGICounters(dxgiCounters,
                                       utilizationByLuid,
                                       m_DXGIIdToLuidId,
                                       nvmlSourcedIds,
                                       /*absentMeansIdle=*/true,
                                       m_PDHAdapterProbe->adapterUtilizationUnread());
}

std::vector<ProcessGPUCounters> WindowsGPUProbe::readProcessGPUCounters()
{
    std::vector<ProcessGPUCounters> allCounters;
    m_OtherAdapterUsage.clear();

    // Use PDH for per-process GPU data (utilization + memory)
    // This is the same mechanism Task Manager uses - works cross-vendor
    if (m_PDHProbe && m_PDHProbe->isAvailable())
    {
        auto pdhCounters = m_PDHProbe->readProcessGPUCounters();
        // Only the listed adapters' samples count: PDH also has the Basic Render Driver's instances
        // (and an accelerator's DXCore couldn't enumerate), which are kept apart (#1826).
        m_OtherAdapterUsage = separateOtherAdapterUsage(pdhCounters, m_ListedAdapterLuids);
        if (!m_OtherAdapterUsage.empty())
        {
            spdlog::debug("WindowsGPUProbe: {} per-process sample(s) on adapters not listed, kept out of every figure",
                          m_OtherAdapterUsage.size());
        }
        // An NPU's or compute accelerator's samples are its kind's, never a GPU's (#1799)
        tagProcessCounterKinds(pdhCounters, m_AdapterKindByLuid);
        if (!pdhCounters.empty())
        {
            spdlog::debug("WindowsGPUProbe: Got {} per-process GPU entries from PDH (utilization + memory)", pdhCounters.size());
            allCounters = std::move(pdhCounters);
        }
    }

    // PDH provides both GPU Engine (utilization) and GPU Process Memory counters
    // No need to merge from other sources - PDH is the authoritative source for per-process data

    return allCounters;
}

std::vector<OtherAdapterProcessUsage> WindowsGPUProbe::otherAdapterUsage() const
{
    return m_OtherAdapterUsage;
}

bool WindowsGPUProbe::isAdapterAsleep(const std::optional<PciLocation>& location) const
{
    return location.has_value() && m_IsAsleep && m_IsAsleep(*location);
}

void WindowsGPUProbe::assignAdapterPerfSensors(std::vector<GPUInfo>& gpus)
{
    m_PerfAdapters.clear();
    m_PerfAsleepAtEnumeration.clear();
    if (!m_PerfFunctions)
    {
        return;
    }
    // Devices may have come or gone since the last enumeration: look each devnode up again.
    if (m_DevicePower)
    {
        m_DevicePower->reset();
    }
    for (std::size_t dxgiIdx = 0; dxgiIdx < gpus.size(); ++dxgiIdx)
    {
        GPUInfo& gpu = gpus[dxgiIdx];
        // NVML's sensors are the richer set (power in watts, encoder/decoder): an adapter matched to
        // an NVML device keeps them. An accelerator has no sensors (#1799).
        if (gpu.kind != DeviceKind::Gpu || m_DXGIToNVMLMap.contains(static_cast<std::uint32_t>(dxgiIdx)))
        {
            continue;
        }
        const auto luid = luidFromPdhFormat(gpu.luidId);
        if (!luid.has_value())
        {
            continue;
        }
        if (gpu.pciLocation.has_value() && isAdapterAsleep(gpu.pciLocation))
        {
            // Not read, so as not to wake it (#1265): no sensors until a quick rescan finds it awake.
            spdlog::debug("WindowsGPUProbe: Adapter {} ({}) is asleep; its sensors are read once it wakes", gpu.id, gpu.name);
            m_PerfAsleepAtEnumeration.emplace(gpu.id, gpu.pciLocation.value_or(PciLocation{}));
            continue;
        }
        const LUID adapterLuid{.LowPart = luid->second, .HighPart = static_cast<LONG>(luid->first)};
        const GPUCapabilities sensors = adapterPerfSensorCapabilities(readAdapterPerfData(adapterLuid, *m_PerfFunctions));
        if (!hasAnyAdapterPerfSensor(sensors))
        {
            spdlog::debug("WindowsGPUProbe: Adapter {} ({}) reports no perf-data sensors", gpu.id, gpu.name);
            continue;
        }
        // Once per enumeration, like NVML's "Enumerated N NVIDIA GPU(s)": which sensors the adapter has.
        spdlog::info("WindowsGPUProbe: Adapter {} ({}) sensors from D3DKMT perf data: temperature {}, clock {}, fan {}",
                     gpu.id,
                     gpu.name,
                     sensors.hasTemperature,
                     sensors.hasClockSpeeds,
                     sensors.hasFanSpeed);
        gpu.sensorCapabilities = sensors;
        m_PerfAdapters.insert_or_assign(
            gpu.id,
            PerfAdapter{.luidHighPart = luid->first, .luidLowPart = luid->second, .pciLocation = gpu.pciLocation, .sensors = sensors});
    }
}

void WindowsGPUProbe::mergeAdapterPerfSensors(std::vector<GPUCounters>& counters)
{
    if (!m_PerfFunctions || m_PerfAdapters.empty())
    {
        return;
    }
    for (auto& counter : counters)
    {
        const auto adapter = m_PerfAdapters.find(counter.gpuId);
        if (adapter == m_PerfAdapters.end())
        {
            continue;
        }
        // A sleeping GPU is left alone, as NVML's are (#1265): its sensors are gaps this sample, while
        // PDH's utilization and memory, the OS's own figures, still fill in.
        if (isAdapterAsleep(adapter->second.pciLocation))
        {
            counter.suspended = true;
            applyAdapterPerfReading(counter, std::nullopt, adapter->second.sensors);
            continue;
        }
        const LUID luid{.LowPart = adapter->second.luidLowPart, .HighPart = static_cast<LONG>(adapter->second.luidHighPart)};
        // A failed read is "no sensor" this sample, never a logged error per sample.
        applyAdapterPerfReading(counter, readAdapterPerfData(luid, *m_PerfFunctions), adapter->second.sensors);
    }
}

GPUCapabilities WindowsGPUProbe::capabilities() const
{
    GPUCapabilities caps{};

    // Start with DXGI capabilities (basic enumeration)
    if (m_DXGIProbe)
    {
        caps = m_DXGIProbe->capabilities();
        // Temperature, clock and fan from the kernel adapter's perf data, for any adapter whose
        // driver fills them (#2586). This describes the probe: which adapters report which of them
        // is per adapter (GPUInfo::sensorCapabilities, set by every enumeration), and the GPU tab
        // draws an adapter's sensors from that, so a machine whose adapters report none draws none.
        // GPUModel reads this before the first enumeration, so it can't wait on one.
        if (m_PerfFunctions)
        {
            caps.hasTemperature = true;
            caps.hasClockSpeeds = true;
            caps.hasFanSpeed = true;
        }
    }

    // Merge NVML capabilities for system-level GPU metrics (temp, power, clocks)
    if (m_NVMLProbe && m_NVMLProbe->isAvailable())
    {
        const auto nvmlCaps = m_NVMLProbe->capabilities();

        caps.hasTemperature = caps.hasTemperature || nvmlCaps.hasTemperature;
        caps.hasPowerMetrics = caps.hasPowerMetrics || nvmlCaps.hasPowerMetrics;
        caps.hasClockSpeeds = caps.hasClockSpeeds || nvmlCaps.hasClockSpeeds;
        caps.hasFanSpeed = caps.hasFanSpeed || nvmlCaps.hasFanSpeed;
        caps.hasEncoderDecoder = caps.hasEncoderDecoder || nvmlCaps.hasEncoderDecoder;
        caps.supportsMultiGPU = caps.supportsMultiGPU || nvmlCaps.supportsMultiGPU;
    }

    // Merge PDH capabilities for per-process metrics (works cross-vendor)
    if (m_PDHProbe && m_PDHProbe->isAvailable())
    {
        const auto pdhCaps = m_PDHProbe->capabilities();

        caps.hasEngineUtilization = caps.hasEngineUtilization || pdhCaps.hasEngineUtilization;
        caps.hasPerProcessMetrics = caps.hasPerProcessMetrics || pdhCaps.hasPerProcessMetrics;
        // Per-process counters come from PDH alone (readProcessGPUCounters()), so its utilization does too.
        caps.hasPerProcessUtilization = caps.hasPerProcessUtilization || pdhCaps.hasPerProcessUtilization;
        // ...and the NPUs' and compute accelerators' per-process data likewise (#1973)
        caps.hasPerProcessNpuMetrics = caps.hasPerProcessNpuMetrics || pdhCaps.hasPerProcessNpuMetrics;
        caps.hasPerProcessNpuUtilization = caps.hasPerProcessNpuUtilization || pdhCaps.hasPerProcessNpuUtilization;
        caps.supportsMultiGPU = caps.supportsMultiGPU || pdhCaps.supportsMultiGPU;
    }

    return caps;
}

} // namespace Platform
