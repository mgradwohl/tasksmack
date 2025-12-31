#include "Domain/GPUModel.h"

#include <spdlog/spdlog.h>

#include <algorithm>

namespace Domain
{

GPUModel::GPUModel(std::unique_ptr<Platform::IGPUProbe> probe)
    : m_Probe(std::move(probe)), m_PrevSampleTime(std::chrono::steady_clock::now())
{
    if (!m_Probe)
    {
        spdlog::warn("GPUModel: No GPU probe provided");
        return;
    }

    // Enumerate GPUs once at construction
    try
    {
        m_GPUInfo = m_Probe->enumerateGPUs();
        spdlog::info("GPUModel: Detected {} GPU(s)", m_GPUInfo.size());

        // Initialize history buffers for each GPU
        for (const auto& info : m_GPUInfo)
        {
            m_Histories.emplace(info.id, History<GPUSnapshot>{});
        }
    }
    catch (const std::exception& e)
    {
        spdlog::error("GPUModel: Failed to enumerate GPUs: {}", e.what());
    }
}

void GPUModel::refresh()
{
    if (!m_Probe)
    {
        return;
    }

    try
    {
        // Read current counters
        auto currentCounters = m_Probe->readGPUCounters();
        auto currentTime = std::chrono::steady_clock::now();

        // Calculate time delta
        auto timeDelta = std::chrono::duration_cast<std::chrono::milliseconds>(currentTime - m_PrevSampleTime);
        double timeDeltaSeconds = timeDelta.count() / 1000.0;

        // Compute snapshots
        std::unordered_map<std::string, GPUSnapshot> newSnapshots;
        for (const auto& current : currentCounters)
        {
            // Look for previous counters
            const Platform::GPUCounters* previous = nullptr;
            auto prevIt = m_PrevCounters.find(current.gpuId);
            if (prevIt != m_PrevCounters.end())
            {
                previous = &prevIt->second;
            }

            // Compute snapshot
            auto snapshot = computeSnapshot(current, previous, timeDeltaSeconds);
            newSnapshots[current.gpuId] = snapshot;

            // Add to history
            auto histIt = m_Histories.find(current.gpuId);
            if (histIt != m_Histories.end())
            {
                histIt->second.add(snapshot);
            }
        }

        // Update stored state under lock
        {
            std::unique_lock lock(m_Mutex);
            m_Snapshots = std::move(newSnapshots);
            m_PrevCounters.clear();
            for (const auto& counter : currentCounters)
            {
                m_PrevCounters[counter.gpuId] = counter;
            }
            m_PrevSampleTime = currentTime;
        }
    }
    catch (const std::exception& e)
    {
        spdlog::error("GPUModel::refresh: {}", e.what());
    }
}

std::vector<GPUSnapshot> GPUModel::snapshots() const
{
    std::shared_lock lock(m_Mutex);
    std::vector<GPUSnapshot> result;
    result.reserve(m_Snapshots.size());
    for (const auto& [_, snapshot] : m_Snapshots)
    {
        result.push_back(snapshot);
    }
    return result;
}

const History<GPUSnapshot>& GPUModel::history(const std::string& gpuId) const
{
    std::shared_lock lock(m_Mutex);
    auto it = m_Histories.find(gpuId);
    if (it == m_Histories.end())
    {
        static const History<GPUSnapshot> empty;
        return empty;
    }
    return it->second;
}

std::vector<Platform::GPUInfo> GPUModel::gpuInfo() const
{
    std::shared_lock lock(m_Mutex);
    return m_GPUInfo;
}

Platform::GPUCapabilities GPUModel::capabilities() const
{
    if (!m_Probe)
    {
        return Platform::GPUCapabilities{};
    }
    return m_Probe->capabilities();
}

GPUSnapshot
GPUModel::computeSnapshot(const Platform::GPUCounters& current, const Platform::GPUCounters* previous, double timeDeltaSeconds) const
{
    GPUSnapshot snapshot;

    // Copy identity
    snapshot.gpuId = current.gpuId;

    // Find GPU info for this ID
    auto infoIt = std::ranges::find_if(m_GPUInfo, [&](const auto& info) { return info.id == current.gpuId; });
    if (infoIt != m_GPUInfo.end())
    {
        snapshot.name = infoIt->name;
        snapshot.vendor = infoIt->vendor;
        snapshot.isIntegrated = infoIt->isIntegrated;
    }

    // Copy instantaneous values
    snapshot.utilizationPercent = current.utilizationPercent;
    snapshot.memoryUtilPercent = current.memoryUtilPercent;
    snapshot.memoryUsedBytes = current.memoryUsedBytes;
    snapshot.memoryTotalBytes = current.memoryTotalBytes;
    snapshot.temperatureC = current.temperatureC;
    snapshot.hotspotTempC = current.hotspotTempC;
    snapshot.powerDrawWatts = current.powerDrawWatts;
    snapshot.powerLimitWatts = current.powerLimitWatts;
    snapshot.gpuClockMHz = current.gpuClockMHz;
    snapshot.memoryClockMHz = current.memoryClockMHz;
    snapshot.fanSpeedRPM = current.fanSpeedRPM;
    snapshot.computeUtilPercent = current.computeUtilPercent;
    snapshot.encoderUtilPercent = current.encoderUtilPercent;
    snapshot.decoderUtilPercent = current.decoderUtilPercent;

    // Compute derived values
    if (current.memoryTotalBytes > 0)
    {
        snapshot.memoryUsedPercent = (static_cast<double>(current.memoryUsedBytes) / static_cast<double>(current.memoryTotalBytes)) * 100.0;
    }

    if (current.powerLimitWatts > 0.0)
    {
        snapshot.powerUtilPercent = (current.powerDrawWatts / current.powerLimitWatts) * 100.0;
    }

    // Compute rates from deltas (only if we have previous data and valid time delta)
    if (previous != nullptr && timeDeltaSeconds > 0.0)
    {
        // PCIe bandwidth rates
        if (current.pcieTxBytes >= previous->pcieTxBytes)
        {
            std::uint64_t txDelta = current.pcieTxBytes - previous->pcieTxBytes;
            snapshot.pcieTxBytesPerSec = static_cast<double>(txDelta) / timeDeltaSeconds;
        }

        if (current.pcieRxBytes >= previous->pcieRxBytes)
        {
            std::uint64_t rxDelta = current.pcieRxBytes - previous->pcieRxBytes;
            snapshot.pcieRxBytesPerSec = static_cast<double>(rxDelta) / timeDeltaSeconds;
        }
    }

    return snapshot;
}

} // namespace Domain
