#include "GPUModel.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <ranges>

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
            m_Histories.emplace(info.id, History<GPUSnapshot, GPU_HISTORY_CAPACITY>{});
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
        const double timeDeltaSeconds = static_cast<double>(timeDelta.count()) / 1000.0;

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
        }

        // Update stored state under lock
        {
            const std::unique_lock lock(m_Mutex);
            m_Snapshots = std::move(newSnapshots);

            // Record timestamp for this sample
            const double nowSec = std::chrono::duration<double>(currentTime.time_since_epoch()).count();
            m_HistoryTimestamps.push_back(nowSec);

            // Trim timestamps to match history capacity
            if (m_HistoryTimestamps.size() > GPU_HISTORY_CAPACITY)
            {
                m_HistoryTimestamps.erase(m_HistoryTimestamps.begin(),
                                          m_HistoryTimestamps.begin() +
                                              static_cast<std::ptrdiff_t>(m_HistoryTimestamps.size() - GPU_HISTORY_CAPACITY));
            }

            // Push to history under lock protection
            for (const auto& [gpuId, snapshot] : m_Snapshots)
            {
                auto histIt = m_Histories.find(gpuId);
                if (histIt != m_Histories.end())
                {
                    histIt->second.push(snapshot);
                }
            }

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
    const std::shared_lock lock(m_Mutex);
    std::vector<GPUSnapshot> result;
    result.reserve(m_Snapshots.size());
    for (const auto& [_, snapshot] : m_Snapshots)
    {
        result.push_back(snapshot);
    }
    return result;
}

std::vector<GPUSnapshot> GPUModel::history(const std::string& gpuId) const
{
    const std::shared_lock lock(m_Mutex);
    auto it = m_Histories.find(gpuId);
    if (it == m_Histories.end())
    {
        return {};
    }

    // Copy history data to vector for thread-safe return
    std::vector<GPUSnapshot> result;
    result.reserve(it->second.size());
    for (size_t i = 0; i < it->second.size(); ++i)
    {
        result.push_back(it->second[i]);
    }
    return result;
}

std::vector<Platform::GPUInfo> GPUModel::gpuInfo() const
{
    const std::shared_lock lock(m_Mutex);
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
    snapshot.memoryUsedBytes = current.memoryUsedBytes;
    snapshot.memoryTotalBytes = current.memoryTotalBytes;
    snapshot.temperatureC = current.temperatureC;
    snapshot.hotspotTempC = current.hotspotTempC;
    snapshot.powerDrawWatts = current.powerDrawWatts;
    snapshot.powerLimitWatts = current.powerLimitWatts;
    snapshot.gpuClockMHz = current.gpuClockMHz;
    snapshot.memoryClockMHz = current.memoryClockMHz;
    snapshot.fanSpeedRPMPercent = current.fanSpeedRPMPercent;
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
            const std::uint64_t txDelta = current.pcieTxBytes - previous->pcieTxBytes;
            snapshot.pcieTxBytesPerSec = static_cast<double>(txDelta) / timeDeltaSeconds;
        }

        if (current.pcieRxBytes >= previous->pcieRxBytes)
        {
            const std::uint64_t rxDelta = current.pcieRxBytes - previous->pcieRxBytes;
            snapshot.pcieRxBytesPerSec = static_cast<double>(rxDelta) / timeDeltaSeconds;
        }
    }

    return snapshot;
}

// Template helper for extracting history fields - reduces code duplication
template<typename FieldPtr> std::vector<float> GPUModel::getHistoryField(const std::string& gpuId, FieldPtr field) const
{
    const std::shared_lock lock(m_Mutex);
    auto it = m_Histories.find(gpuId);
    if (it == m_Histories.end())
    {
        return {};
    }

    std::vector<float> result;
    result.reserve(it->second.size());
    for (size_t i = 0; i < it->second.size(); ++i)
    {
        result.push_back(static_cast<float>(it->second[i].*field));
    }
    return result;
}

std::vector<float> GPUModel::utilizationHistory(const std::string& gpuId) const
{
    return getHistoryField(gpuId, &GPUSnapshot::utilizationPercent);
}

std::vector<float> GPUModel::memoryPercentHistory(const std::string& gpuId) const
{
    return getHistoryField(gpuId, &GPUSnapshot::memoryUsedPercent);
}

std::vector<float> GPUModel::gpuClockHistory(const std::string& gpuId) const
{
    return getHistoryField(gpuId, &GPUSnapshot::gpuClockMHz);
}

std::vector<float> GPUModel::encoderHistory(const std::string& gpuId) const
{
    return getHistoryField(gpuId, &GPUSnapshot::encoderUtilPercent);
}

std::vector<float> GPUModel::decoderHistory(const std::string& gpuId) const
{
    return getHistoryField(gpuId, &GPUSnapshot::decoderUtilPercent);
}

std::vector<float> GPUModel::temperatureHistory(const std::string& gpuId) const
{
    return getHistoryField(gpuId, &GPUSnapshot::temperatureC);
}

std::vector<float> GPUModel::powerHistory(const std::string& gpuId) const
{
    return getHistoryField(gpuId, &GPUSnapshot::powerDrawWatts);
}

std::vector<float> GPUModel::fanSpeedHistory(const std::string& gpuId) const
{
    return getHistoryField(gpuId, &GPUSnapshot::fanSpeedRPMPercent);
}

std::vector<double> GPUModel::historyTimestamps() const
{
    const std::shared_lock lock(m_Mutex);
    return m_HistoryTimestamps;
}

} // namespace Domain
