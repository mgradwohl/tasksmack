#include "WindowsGPUProbe.h"

#include "DXGIGPUProbe.h"
#include "NVMLGPUProbe.h"
#include "PDHGPUProbe.h"
#include "Platform/GPUTypes.h"
#include "Platform/Windows/WindowsDriverNodes.h"
#include "WinString.h"
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
#include <devpropdef.h>
// clang-format on

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Platform
{

namespace
{

// DEVPKEY_Device_DriverVersion (devpkey.h), spelled out so no translation unit needs <initguid.h>.
constexpr DEVPROPKEY PNP_DRIVER_VERSION_KEY{
    .fmtid = {.Data1 = 0xa8b865dd, .Data2 = 0x2e3d, .Data3 = 0x4094, .Data4 = {0xad, 0x97, 0xe5, 0x93, 0xa7, 0x0c, 0x75, 0xd6}},
    .pid = 3,
};

/// Every present PCI device node's first hardware id, bus location and driver version, whatever its
/// class: a GPU is a Display device and an NPU a ComputeAccelerator one, and a compute accelerator
/// may be in another class again (#1799). Read from the PnP manager's records, so no device is woken.
// NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast) - SetupAPI byte buffers
[[nodiscard]] std::vector<WindowsGraphics::DriverNode> listPciDriverNodes()
{
    std::vector<WindowsGraphics::DriverNode> nodes;
    HDEVINFO devices = SetupDiGetClassDevsW(nullptr, L"PCI", nullptr, DIGCF_PRESENT | DIGCF_ALLCLASSES);
    if (devices == INVALID_HANDLE_VALUE)
    {
        return nodes;
    }
    SP_DEVINFO_DATA device{};
    device.cbSize = sizeof(device);
    DWORD index = 0;
    while (SetupDiEnumDeviceInfo(devices, index, &device) != FALSE)
    {
        ++index;
        WindowsGraphics::DriverNode node;
        std::array<wchar_t, 512> text{}; // the last element stays 0
        constexpr auto TEXT_BYTES = static_cast<DWORD>((512 - 1) * sizeof(wchar_t));
        if (SetupDiGetDeviceRegistryPropertyW(
                devices, &device, SPDRP_HARDWAREID, nullptr, reinterpret_cast<PBYTE>(text.data()), TEXT_BYTES, nullptr) != FALSE)
        {
            node.hardwareId = WinString::wideToUtf8(text.data()); // a REG_MULTI_SZ's first string
        }
        DWORD value = 0;
        if (SetupDiGetDeviceRegistryPropertyW(
                devices, &device, SPDRP_BUSNUMBER, nullptr, reinterpret_cast<PBYTE>(&value), sizeof(value), nullptr) != FALSE)
        {
            node.bus = value;
        }
        if (SetupDiGetDeviceRegistryPropertyW(
                devices, &device, SPDRP_ADDRESS, nullptr, reinterpret_cast<PBYTE>(&value), sizeof(value), nullptr) != FALSE)
        {
            node.address = value;
        }
        DEVPROPTYPE type = 0;
        text.fill(L'\0');
        if (SetupDiGetDevicePropertyW(
                devices, &device, &PNP_DRIVER_VERSION_KEY, &type, reinterpret_cast<PBYTE>(text.data()), TEXT_BYTES, nullptr, 0) != FALSE &&
            type == DEVPROP_TYPE_STRING)
        {
            node.driverVersion = WinString::wideToUtf8(text.data());
        }
        nodes.push_back(std::move(node));
    }
    SetupDiDestroyDeviceInfoList(devices);
    return nodes;
}
// NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)

} // namespace

WindowsGPUProbe::WindowsGPUProbe()
    : m_DXGIProbe(std::make_unique<DXGIGPUProbe>()),
      m_NVMLProbe(std::make_unique<NVMLGPUProbe>()),
      m_PDHProbe(std::make_unique<PDHGPUProbe>(PDHGPUProbe::Role::Process)),
      m_PDHAdapterProbe(std::make_unique<PDHGPUProbe>(PDHGPUProbe::Role::Adapter)),
      m_ListDriverNodes(listPciDriverNodes)
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
        // Sensor metrics are per adapter: its own NVML device's, or none without one (#1040).
        assignSensorCapabilities(gpus, nvmlGPUs, m_DXGIToNVMLMap, nvmlCaps);

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
        caps.hasPowerMetrics = caps.hasPowerMetrics || nvmlCaps.hasPowerMetrics;
        caps.hasClockSpeeds = caps.hasClockSpeeds || nvmlCaps.hasClockSpeeds;
        caps.hasFanSpeed = caps.hasFanSpeed || nvmlCaps.hasFanSpeed;
        caps.hasEncoderDecoder = caps.hasEncoderDecoder || nvmlCaps.hasEncoderDecoder;
        caps.supportsMultiGPU = caps.supportsMultiGPU || nvmlCaps.supportsMultiGPU;
    }

    // Merge PDH capabilities for per-process metrics (works cross-vendor)
    if (m_PDHProbe && m_PDHProbe->isAvailable())
    {
        auto pdhCaps = m_PDHProbe->capabilities();

        caps.hasEngineUtilization = caps.hasEngineUtilization || pdhCaps.hasEngineUtilization;
        caps.hasPerProcessMetrics = caps.hasPerProcessMetrics || pdhCaps.hasPerProcessMetrics;
        // Per-process counters come from PDH alone (readProcessGPUCounters()), so its utilization does too.
        caps.hasPerProcessUtilization = caps.hasPerProcessUtilization || pdhCaps.hasPerProcessUtilization;
        caps.supportsMultiGPU = caps.supportsMultiGPU || pdhCaps.supportsMultiGPU;
    }

    return caps;
}

} // namespace Platform
