#include "WindowsGPUProbe.h"

#include "DXGIGPUProbe.h"
#include "NVMLGPUProbe.h"
#include "PDHGPUProbe.h"
#include "Platform/GPUTypes.h"
#include "WindowsGPUProbeMath.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Platform
{

WindowsGPUProbe::WindowsGPUProbe()
    : m_DXGIProbe(std::make_unique<DXGIGPUProbe>()),
      m_NVMLProbe(std::make_unique<NVMLGPUProbe>()),
      m_PDHProbe(std::make_unique<PDHGPUProbe>(PDHGPUProbe::Role::Process)),
      m_PDHAdapterProbe(std::make_unique<PDHGPUProbe>(PDHGPUProbe::Role::Adapter))
{
    std::string probeSummary = "DXGI";
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
            // NVML's per-process counters name each device by its matched adapter's id (#1317).
            m_NVMLAdapterIds = nvmlDeviceAdapterIds(gpus, nvmlGPUs, m_DXGIToNVMLMap);
            m_NVMLProbe->setProcessGpuIds(m_NVMLAdapterIds);
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
        // Sensor metrics are per adapter: its own NVML device's, or none without one (#1040).
        assignSensorCapabilities(gpus, nvmlGPUs, m_DXGIToNVMLMap, nvmlCaps);
        for (const auto& gpu : gpus)
        {
            m_DXGIIdIsIntegrated[gpu.id] = gpu.isIntegrated;
            if (!gpu.luidId.empty())
            {
                m_DXGIIdToLuidId[gpu.id] = gpu.luidId;
                spdlog::debug("WindowsGPUProbe: LUID mapping: {} → {}", gpu.id, gpu.luidId);
            }
        }

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
    return adaptersChanged || nvmlChanged;
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
    auto nvmlCounters = m_NVMLProbe->readGPUCounters();
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

    // Use PDH for per-process GPU data (utilization + memory)
    // This is the same mechanism Task Manager uses - works cross-vendor
    if (m_PDHProbe && m_PDHProbe->isAvailable())
    {
        auto pdhCounters = m_PDHProbe->readProcessGPUCounters();
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

GPUCapabilities WindowsGPUProbe::capabilities() const
{
    GPUCapabilities caps{};

    // Start with DXGI capabilities (basic enumeration)
    if (m_DXGIProbe)
    {
        caps = m_DXGIProbe->capabilities();
    }

    // Merge NVML capabilities for system-level GPU metrics (temp, power, clocks)
    if (m_NVMLProbe && m_NVMLProbe->isAvailable())
    {
        auto nvmlCaps = m_NVMLProbe->capabilities();

        caps.hasTemperature = caps.hasTemperature || nvmlCaps.hasTemperature;
        caps.hasHotspotTemp = caps.hasHotspotTemp || nvmlCaps.hasHotspotTemp;
        caps.hasPowerMetrics = caps.hasPowerMetrics || nvmlCaps.hasPowerMetrics;
        caps.hasClockSpeeds = caps.hasClockSpeeds || nvmlCaps.hasClockSpeeds;
        caps.hasFanSpeed = caps.hasFanSpeed || nvmlCaps.hasFanSpeed;
        caps.hasPCIeMetrics = caps.hasPCIeMetrics || nvmlCaps.hasPCIeMetrics;
        caps.hasEncoderDecoder = caps.hasEncoderDecoder || nvmlCaps.hasEncoderDecoder;
        caps.supportsMultiGPU = caps.supportsMultiGPU || nvmlCaps.supportsMultiGPU;
    }

    // Merge PDH capabilities for per-process metrics (works cross-vendor)
    if (m_PDHProbe && m_PDHProbe->isAvailable())
    {
        auto pdhCaps = m_PDHProbe->capabilities();

        caps.hasEngineUtilization = caps.hasEngineUtilization || pdhCaps.hasEngineUtilization;
        caps.hasPerProcessMetrics = caps.hasPerProcessMetrics || pdhCaps.hasPerProcessMetrics;
        caps.supportsMultiGPU = caps.supportsMultiGPU || pdhCaps.supportsMultiGPU;
    }

    return caps;
}

} // namespace Platform
