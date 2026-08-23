#include "GPUModel.h"

#include "GPUSnapshot.h"
#include "History.h"
#include "Numeric.h"
#include "Platform/GPUTypes.h"
#include "Platform/IGPUProbe.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
// NOLINTNEXTLINE(misc-include-cleaner) - std::ranges::find_if and std::ranges::find are in <ranges>
#include <ranges>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

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
        std::vector<Platform::GPUCounters> currentCounters;
        {
            const std::scoped_lock probeLock(m_ProbeMutex);
            currentCounters = m_Probe->readGPUCounters();
        }
        auto currentTime = std::chrono::steady_clock::now();

        // Calculate time delta
        auto timeDelta = std::chrono::duration_cast<std::chrono::milliseconds>(currentTime - m_PrevSampleTime);
        const double timeDeltaSeconds = static_cast<double>(timeDelta.count()) / 1000.0;

        // Compute snapshots
        SnapshotMap newSnapshots;
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

            // Push to history under lock protection — stamp capture time first so that
            // per-GPU timestamps stay aligned with each GPU's own history entries.
            for (auto& [gpuId, snapshot] : m_Snapshots)
            {
                snapshot.captureTimeSec = nowSec;
                auto histIt = m_Histories.find(gpuId);
                if (histIt != m_Histories.end())
                {
                    histIt->second.push(snapshot);
                }
            }
            publish();

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

std::shared_ptr<const GPUPublication> GPUModel::publication() const noexcept
{
    return m_Publication.load(std::memory_order_acquire);
}

void GPUModel::publish()
{
    auto publication = std::make_shared<GPUPublication>();
    publication->version = ++m_PublicationVersion;
    publication->gpuInfo = m_GPUInfo;
    {
        const std::scoped_lock probeLock(m_ProbeMutex);
        publication->capabilities = m_Probe->capabilities();
    }
    publication->snapshots.reserve(m_Snapshots.size());
    for (const auto& [gpuId, snapshot] : m_Snapshots)
    {
        publication->snapshots.push_back(snapshot);
    }
    for (const auto& [gpuId, history] : m_Histories)
    {
        auto& publishedHistory = publication->histories[gpuId];
        publishedHistory.snapshots.reserve(history.size());
        publishedHistory.timestamps.reserve(history.size());
        publishedHistory.utilization.reserve(history.size());
        publishedHistory.memoryPercent.reserve(history.size());
        publishedHistory.gpuClock.reserve(history.size());
        publishedHistory.encoder.reserve(history.size());
        publishedHistory.decoder.reserve(history.size());
        publishedHistory.temperature.reserve(history.size());
        publishedHistory.power.reserve(history.size());
        publishedHistory.fanSpeed.reserve(history.size());
        for (std::size_t index = 0; index < history.size(); ++index)
        {
            const auto sample = history[index];
            publishedHistory.snapshots.push_back(sample);
            publishedHistory.timestamps.push_back(sample.captureTimeSec);
            publishedHistory.utilization.push_back(static_cast<float>(sample.utilizationPercent));
            publishedHistory.memoryPercent.push_back(static_cast<float>(sample.memoryUsedPercent));
            publishedHistory.gpuClock.push_back(static_cast<float>(sample.gpuClockMHz));
            publishedHistory.encoder.push_back(static_cast<float>(sample.encoderUtilPercent));
            publishedHistory.decoder.push_back(static_cast<float>(sample.decoderUtilPercent));
            publishedHistory.temperature.push_back(static_cast<float>(sample.temperatureC));
            publishedHistory.power.push_back(static_cast<float>(sample.powerDrawWatts));
            publishedHistory.fanSpeed.push_back(static_cast<float>(sample.fanSpeedRPMPercent));
        }
    }
    m_Publication.store(std::move(publication), std::memory_order_release);
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

std::vector<GPUSnapshot> GPUModel::history(std::string_view gpuId) const
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
    result.resize(it->second.size());
    const std::size_t copied = it->second.copyTo(result.data(), result.size());
    result.resize(copied);
    return result;
}

std::optional<GPUSnapshot> GPUModel::snapshotAt(std::string_view gpuId, std::size_t index) const
{
    const std::shared_lock lock(m_Mutex);
    auto it = m_Histories.find(gpuId);
    if (it == m_Histories.end() || index >= it->second.size())
    {
        return std::nullopt;
    }
    return it->second[index];
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
    const std::scoped_lock probeLock(m_ProbeMutex);
    return m_Probe->capabilities();
}

std::vector<Platform::ProcessGPUCounters> GPUModel::readProcessGPUCounters() const
{
    if (!m_Probe)
    {
        return {};
    }
    const std::scoped_lock probeLock(m_ProbeMutex);
    return m_Probe->readProcessGPUCounters();
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
        snapshot.luidId = infoIt->luidId; // For PDH counter matching
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
        snapshot.pcieTxBytesPerSec = Numeric::counterRate(current.pcieTxBytes, previous->pcieTxBytes, timeDeltaSeconds);
        snapshot.pcieRxBytesPerSec = Numeric::counterRate(current.pcieRxBytes, previous->pcieRxBytes, timeDeltaSeconds);
    }

    return snapshot;
}

// Template helper for extracting history fields - reduces code duplication
template<typename FieldPtr> std::vector<float> GPUModel::getHistoryField(std::string_view gpuId, FieldPtr field) const
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

std::vector<float> GPUModel::utilizationHistory(std::string_view gpuId) const
{
    return getHistoryField(gpuId, &GPUSnapshot::utilizationPercent);
}

std::vector<float> GPUModel::memoryPercentHistory(std::string_view gpuId) const
{
    return getHistoryField(gpuId, &GPUSnapshot::memoryUsedPercent);
}

std::vector<float> GPUModel::gpuClockHistory(std::string_view gpuId) const
{
    return getHistoryField(gpuId, &GPUSnapshot::gpuClockMHz);
}

std::vector<float> GPUModel::encoderHistory(std::string_view gpuId) const
{
    return getHistoryField(gpuId, &GPUSnapshot::encoderUtilPercent);
}

std::vector<float> GPUModel::decoderHistory(std::string_view gpuId) const
{
    return getHistoryField(gpuId, &GPUSnapshot::decoderUtilPercent);
}

std::vector<float> GPUModel::temperatureHistory(std::string_view gpuId) const
{
    return getHistoryField(gpuId, &GPUSnapshot::temperatureC);
}

std::vector<float> GPUModel::powerHistory(std::string_view gpuId) const
{
    return getHistoryField(gpuId, &GPUSnapshot::powerDrawWatts);
}

std::vector<float> GPUModel::fanSpeedHistory(std::string_view gpuId) const
{
    return getHistoryField(gpuId, &GPUSnapshot::fanSpeedRPMPercent);
}

std::vector<double> GPUModel::historyTimestamps() const
{
    const std::shared_lock lock(m_Mutex);
    return m_HistoryTimestamps;
}

std::vector<double> GPUModel::historyTimestamps(std::string_view gpuId) const
{
    const std::shared_lock lock(m_Mutex);
    auto it = m_Histories.find(gpuId);
    if (it == m_Histories.end())
    {
        return {};
    }
    std::vector<double> result;
    result.reserve(it->second.size());
    for (std::size_t i = 0; i < it->second.size(); ++i)
    {
        result.push_back(it->second.ref(i).captureTimeSec);
    }
    return result;
}

void GPUModel::setInstanceRefreshInterval(std::chrono::seconds interval)
{
    if (m_Probe)
    {
        const std::scoped_lock probeLock(m_ProbeMutex);
        m_Probe->setInstanceRefreshInterval(interval);
    }
}

} // namespace Domain
