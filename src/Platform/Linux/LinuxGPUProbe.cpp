#include "LinuxGPUProbe.h"

#include "AccelNpuProbe.h"
#include "DRMGPUProbe.h"
#include "NVMLGPUProbe.h"
#include "Platform/GPUTypes.h"
#include "ROCmGPUProbe.h"

#include <spdlog/spdlog.h>

#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace Platform
{

LinuxGPUProbe::LinuxGPUProbe(const std::string& drmBasePath, const std::string& pciDevicesRoot, std::string accelClassRoot)
    : LinuxGPUProbe(std::make_unique<DRMGPUProbe>(drmBasePath),
                    std::make_unique<DRMGPUProbe>(
                        drmBasePath, DRMGPUProbe::VramQuery{}, "/proc", DRMGPUProbe::MonotonicClock{}, DRMGPUProbe::CardKind::AmdClients),
                    pciDevicesRoot,
                    std::move(accelClassRoot))
{}

LinuxGPUProbe::LinuxGPUProbe(std::unique_ptr<DRMGPUProbe> drmProbe,
                             std::unique_ptr<DRMGPUProbe> amdClientsProbe,
                             const std::string& pciDevicesRoot,
                             std::string accelClassRoot)
    : m_NVMLProbe(std::make_unique<NVMLGPUProbe>(pciDevicesRoot)),
      m_DRMProbe(std::move(drmProbe)),
      m_ROCmProbe(std::make_unique<ROCmGPUProbe>(pciDevicesRoot)),
      m_AmdClientsProbe(std::move(amdClientsProbe)),
      m_AccelProbe(std::make_unique<AccelNpuProbe>(std::move(accelClassRoot)))
{
    shareRocmIds();
    std::vector<std::string> probes;
    if (m_NVMLProbe->isAvailable())
    {
        probes.emplace_back("NVML");
    }
    if (m_DRMProbe->isAvailable())
    {
        probes.emplace_back("DRM");
    }
    if (m_ROCmProbe->isAvailable())
    {
        probes.emplace_back("ROCm");
    }
    if (m_AccelProbe->isAvailable())
    {
        probes.emplace_back("accel");
    }

    std::string probeSummary = probes.empty() ? "None" : "";
    for (std::size_t i = 0; i < probes.size(); ++i)
    {
        probeSummary += probes[i];
        if (i < probes.size() - 1)
        {
            probeSummary += " + ";
        }
    }

    spdlog::debug("LinuxGPUProbe: Initialized with {} probe(s)", probeSummary);
}

LinuxGPUProbe::~LinuxGPUProbe() = default;

bool LinuxGPUProbe::amdClientsActive() const
{
    return m_AmdClientsProbe && m_AmdClientsProbe->isAvailable() && m_ROCmProbe && m_ROCmProbe->isAvailable();
}

void LinuxGPUProbe::shareRocmIds()
{
    if (m_AmdClientsProbe && m_ROCmProbe)
    {
        m_AmdClientsProbe->setClientGpuIds(m_ROCmProbe->gpuIdsByPciAddress());
    }
}

std::vector<GPUInfo> LinuxGPUProbe::enumerateGPUs()
{
    std::vector<GPUInfo> gpus;

    // Phase 4: Try NVML first (NVIDIA)
    if (m_NVMLProbe && m_NVMLProbe->isAvailable())
    {
        auto nvmlGPUs = m_NVMLProbe->enumerateGPUs();
        gpus.insert(gpus.end(), nvmlGPUs.begin(), nvmlGPUs.end());
    }

    // Phase 5: Try DRM (Intel)
    if (m_DRMProbe && m_DRMProbe->isAvailable())
    {
        auto drmGPUs = m_DRMProbe->enumerateGPUs();
        gpus.insert(gpus.end(), drmGPUs.begin(), drmGPUs.end());
    }

    // Phase 6: Try ROCm (AMD)
    if (m_ROCmProbe && m_ROCmProbe->isAvailable())
    {
        auto rocmGPUs = m_ROCmProbe->enumerateGPUs();
        gpus.insert(gpus.end(), rocmGPUs.begin(), rocmGPUs.end());
        shareRocmIds();
    }

    // NPUs and compute accelerators, from the accel subsystem (#1827). Listed even when none is now:
    // a rescan may find one.
    if (m_AccelProbe)
    {
        auto accelerators = m_AccelProbe->enumerateGPUs();
        gpus.insert(gpus.end(), accelerators.begin(), accelerators.end());
    }

    if (gpus.empty())
    {
        spdlog::debug("LinuxGPUProbe: No GPU vendor libraries available");
    }

    return gpus;
}

std::vector<GPUCounters> LinuxGPUProbe::readGPUCounters()
{
    std::vector<GPUCounters> counters;

    // Phase 4: NVML metrics for NVIDIA GPUs
    if (m_NVMLProbe && m_NVMLProbe->isAvailable())
    {
        auto nvmlCounters = m_NVMLProbe->readGPUCounters();
        counters.insert(counters.end(), nvmlCounters.begin(), nvmlCounters.end());
    }

    // Phase 5: DRM metrics for Intel GPUs
    if (m_DRMProbe && m_DRMProbe->isAvailable())
    {
        auto drmCounters = m_DRMProbe->readGPUCounters();
        counters.insert(counters.end(), drmCounters.begin(), drmCounters.end());
    }

    // Phase 6: ROCm metrics for AMD GPUs
    if (m_ROCmProbe && m_ROCmProbe->isAvailable())
    {
        auto rocmCounters = m_ROCmProbe->readGPUCounters();
        counters.insert(counters.end(), rocmCounters.begin(), rocmCounters.end());
    }
    // The amdgpu cards' clients, read now for readProcessGPUCounters(); ROCm reports the cards (#2589).
    if (amdClientsActive())
    {
        static_cast<void>(m_AmdClientsProbe->readGPUCounters());
    }

    // NPU busy time (#1827)
    if (m_AccelProbe)
    {
        auto accelCounters = m_AccelProbe->readGPUCounters();
        counters.insert(counters.end(), accelCounters.begin(), accelCounters.end());
    }

    return counters;
}

std::vector<ProcessGPUCounters> LinuxGPUProbe::readProcessGPUCounters()
{
    std::vector<ProcessGPUCounters> counters;

    // Phase 4: NVML per-process metrics for NVIDIA
    if (m_NVMLProbe && m_NVMLProbe->isAvailable())
    {
        auto nvmlCounters = m_NVMLProbe->readProcessGPUCounters();
        counters.insert(counters.end(), nvmlCounters.begin(), nvmlCounters.end());
    }

    // Phase 5: DRM per-process metrics for Intel GPUs, from the clients' fdinfo the last readGPUCounters()
    // read (#2589, #2892)
    if (m_DRMProbe && m_DRMProbe->isAvailable())
    {
        auto drmCounters = m_DRMProbe->readProcessGPUCounters();
        counters.insert(counters.end(), drmCounters.begin(), drmCounters.end());
    }

    // Phase 6: AMD per-process metrics: ROCm SMI has none, so they come from the amdgpu cards' DRM
    // clients, under ROCm's GPU ids (#2589)
    if (amdClientsActive())
    {
        auto amdCounters = m_AmdClientsProbe->readProcessGPUCounters();
        counters.insert(counters.end(), amdCounters.begin(), amdCounters.end());
    }

    // NPU and compute-accelerator memory per process, from the accel clients' fdinfo (#1827).
    if (m_AccelProbe && m_AccelProbe->isAvailable())
    {
        auto accelCounters = m_AccelProbe->readProcessGPUCounters();
        counters.insert(counters.end(), accelCounters.begin(), accelCounters.end());
    }

    return counters;
}

bool LinuxGPUProbe::rescanGPUs(GPURescan depth)
{
    // Every probe is rescanned, so no short-circuit: one changing doesn't excuse the others.
    const bool nvmlChanged = m_NVMLProbe && m_NVMLProbe->rescanGPUs(depth);
    const bool drmChanged = m_DRMProbe && m_DRMProbe->rescanGPUs(depth);
    const bool rocmChanged = m_ROCmProbe && m_ROCmProbe->rescanGPUs(depth);
    // Its cards are ROCm's, which reports a change in them; this only finds their clients again.
    if (m_AmdClientsProbe)
    {
        static_cast<void>(m_AmdClientsProbe->rescanGPUs(depth));
        shareRocmIds();
    }
    const bool accelChanged = m_AccelProbe && m_AccelProbe->rescanGPUs(depth);
    return nvmlChanged || drmChanged || rocmChanged || accelChanged;
}

GPUCapabilities LinuxGPUProbe::capabilities() const
{
    GPUCapabilities caps{};

    // Phase 4: NVML capabilities
    if (m_NVMLProbe && m_NVMLProbe->isAvailable())
    {
        const auto nvmlCaps = m_NVMLProbe->capabilities();

        caps.hasTemperature = caps.hasTemperature || nvmlCaps.hasTemperature;
        caps.hasPowerMetrics = caps.hasPowerMetrics || nvmlCaps.hasPowerMetrics;
        caps.hasClockSpeeds = caps.hasClockSpeeds || nvmlCaps.hasClockSpeeds;
        caps.hasFanSpeed = caps.hasFanSpeed || nvmlCaps.hasFanSpeed;
        caps.hasEngineUtilization = caps.hasEngineUtilization || nvmlCaps.hasEngineUtilization;
        caps.hasPerProcessMetrics = caps.hasPerProcessMetrics || nvmlCaps.hasPerProcessMetrics;
        caps.hasPerProcessUtilization = caps.hasPerProcessUtilization || nvmlCaps.hasPerProcessUtilization;
        caps.hasEncoderDecoder = caps.hasEncoderDecoder || nvmlCaps.hasEncoderDecoder;
        caps.supportsMultiGPU = caps.supportsMultiGPU || nvmlCaps.supportsMultiGPU;
    }

    // Phase 5: DRM capabilities (Intel)
    if (m_DRMProbe && m_DRMProbe->isAvailable())
    {
        const auto drmCaps = m_DRMProbe->capabilities();

        caps.hasTemperature = caps.hasTemperature || drmCaps.hasTemperature;
        caps.hasPowerMetrics = caps.hasPowerMetrics || drmCaps.hasPowerMetrics;
        caps.hasClockSpeeds = caps.hasClockSpeeds || drmCaps.hasClockSpeeds;
        caps.hasFanSpeed = caps.hasFanSpeed || drmCaps.hasFanSpeed;
        caps.hasEngineUtilization = caps.hasEngineUtilization || drmCaps.hasEngineUtilization;
        caps.hasPerProcessMetrics = caps.hasPerProcessMetrics || drmCaps.hasPerProcessMetrics;
        caps.hasPerProcessUtilization = caps.hasPerProcessUtilization || drmCaps.hasPerProcessUtilization;
        caps.hasEncoderDecoder = caps.hasEncoderDecoder || drmCaps.hasEncoderDecoder;
        caps.supportsMultiGPU = caps.supportsMultiGPU || drmCaps.supportsMultiGPU;
    }

    // Phase 6: ROCm capabilities (AMD)
    if (m_ROCmProbe && m_ROCmProbe->isAvailable())
    {
        const auto rocmCaps = m_ROCmProbe->capabilities();

        caps.hasTemperature = caps.hasTemperature || rocmCaps.hasTemperature;
        caps.hasPowerMetrics = caps.hasPowerMetrics || rocmCaps.hasPowerMetrics;
        caps.hasClockSpeeds = caps.hasClockSpeeds || rocmCaps.hasClockSpeeds;
        caps.hasFanSpeed = caps.hasFanSpeed || rocmCaps.hasFanSpeed;
        caps.hasEngineUtilization = caps.hasEngineUtilization || rocmCaps.hasEngineUtilization;
        caps.hasPerProcessMetrics = caps.hasPerProcessMetrics || rocmCaps.hasPerProcessMetrics;
        caps.hasPerProcessUtilization = caps.hasPerProcessUtilization || rocmCaps.hasPerProcessUtilization;
        caps.hasEncoderDecoder = caps.hasEncoderDecoder || rocmCaps.hasEncoderDecoder;
        caps.supportsMultiGPU = caps.supportsMultiGPU || rocmCaps.supportsMultiGPU;
    }
    // AMD per process, from the amdgpu cards' DRM clients (#2589)
    if (amdClientsActive())
    {
        const GPUCapabilities clientCaps = m_AmdClientsProbe->capabilities();
        caps.hasPerProcessMetrics = caps.hasPerProcessMetrics || clientCaps.hasPerProcessMetrics;
        caps.hasPerProcessUtilization = caps.hasPerProcessUtilization || clientCaps.hasPerProcessUtilization;
    }

    // The accel devices add per-process NPU memory, apart from the GPUs' per-process data, and no sensor
    // (#1827); several are still several.
    if (m_AccelProbe && m_AccelProbe->isAvailable())
    {
        const GPUCapabilities accelCaps = m_AccelProbe->capabilities();
        caps.hasPerProcessNpuMetrics = caps.hasPerProcessNpuMetrics || accelCaps.hasPerProcessNpuMetrics;
        caps.supportsMultiGPU = caps.supportsMultiGPU || accelCaps.supportsMultiGPU;
    }

    return caps;
}

} // namespace Platform
