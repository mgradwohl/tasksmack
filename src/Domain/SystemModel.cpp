#include "SystemModel.h"

#include "Numeric.h"
#include "SamplingConfig.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace Domain
{

SystemModel::SystemModel(std::unique_ptr<Platform::ISystemProbe> probe) : m_Probe(std::move(probe))
{
    if (m_Probe)
    {
        m_Capabilities = m_Probe->capabilities();
        spdlog::debug("SystemModel: initialized with probe (perCore={}, swap={})", m_Capabilities.hasPerCoreCpu, m_Capabilities.hasSwap);
    }
    else
    {
        spdlog::warn("SystemModel: initialized without probe");
    }
}

void SystemModel::trimHistory(double nowSeconds)
{
    const double cutoff = nowSeconds - m_MaxHistorySeconds;
    // Trim timestamps first to know how many samples to drop from other tracks.
    std::size_t removeCount = 0;
    while (!m_Timestamps.empty() && (m_Timestamps.front() < cutoff))
    {
        m_Timestamps.pop_front();
        ++removeCount;
    }

    auto trimSamples = [removeCount](auto& dq)
    {
        for (std::size_t i = 0; i < removeCount && !dq.empty(); ++i)
        {
            dq.pop_front();
        }
    };

    trimSamples(m_CpuHistory);
    trimSamples(m_CpuUserHistory);
    trimSamples(m_CpuSystemHistory);
    trimSamples(m_CpuIowaitHistory);
    trimSamples(m_CpuIdleHistory);
    trimSamples(m_MemoryHistory);
    trimSamples(m_MemoryCachedHistory);
    trimSamples(m_SwapHistory);

    for (auto& coreHist : m_PerCoreHistory)
    {
        trimSamples(coreHist);
    }

    // Ensure all history buffers remain aligned by truncating to the smallest non-empty size.
    std::size_t minSize = std::numeric_limits<std::size_t>::max();
    const auto updateMin = [&minSize](std::size_t size)
    {
        if (size > 0)
        {
            minSize = std::min(minSize, size);
        }
    };

    updateMin(m_Timestamps.size());
    updateMin(m_CpuHistory.size());
    updateMin(m_CpuUserHistory.size());
    updateMin(m_CpuSystemHistory.size());
    updateMin(m_CpuIowaitHistory.size());
    updateMin(m_CpuIdleHistory.size());
    updateMin(m_MemoryHistory.size());
    updateMin(m_MemoryCachedHistory.size());
    updateMin(m_SwapHistory.size());
    for (const auto& coreHist : m_PerCoreHistory)
    {
        updateMin(coreHist.size());
    }

    if (minSize != std::numeric_limits<std::size_t>::max())
    {
        auto trimToMin = [minSize](auto& dq)
        {
            while (dq.size() > minSize)
            {
                dq.pop_front();
            }
        };

        trimToMin(m_Timestamps);
        trimToMin(m_CpuHistory);
        trimToMin(m_CpuUserHistory);
        trimToMin(m_CpuSystemHistory);
        trimToMin(m_CpuIowaitHistory);
        trimToMin(m_CpuIdleHistory);
        trimToMin(m_MemoryHistory);
        trimToMin(m_MemoryCachedHistory);
        trimToMin(m_SwapHistory);
        for (auto& coreHist : m_PerCoreHistory)
        {
            trimToMin(coreHist);
        }
    }
}

void SystemModel::setMaxHistorySeconds(double seconds)
{
    std::unique_lock lock(m_Mutex);
    m_MaxHistorySeconds = Domain::Sampling::clampHistorySeconds(seconds);

    if (!m_Timestamps.empty())
    {
        trimHistory(m_Timestamps.back());
    }
}

void SystemModel::refresh()
{
    if (!m_Probe)
    {
        return;
    }

    auto counters = m_Probe->read();
    updateFromCounters(counters);
}

void SystemModel::updateFromCounters(const Platform::SystemCounters& counters)
{
    const double nowSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    updateFromCounters(counters, nowSeconds);
}

void SystemModel::updateFromCounters(const Platform::SystemCounters& counters, double nowSeconds)
{
    std::unique_lock lock(m_Mutex);
    computeSnapshot(counters, nowSeconds);
    m_PrevCounters = counters;
    m_HasPrevious = true;
}

SystemSnapshot SystemModel::snapshot() const
{
    std::shared_lock lock(m_Mutex);
    return m_Snapshot;
}

const Platform::SystemCapabilities& SystemModel::capabilities() const
{
    return m_Capabilities;
}

std::vector<float> SystemModel::cpuHistory() const
{
    std::shared_lock lock(m_Mutex);
    return std::vector<float>(m_CpuHistory.begin(), m_CpuHistory.end());
}

std::vector<float> SystemModel::cpuUserHistory() const
{
    std::shared_lock lock(m_Mutex);
    return std::vector<float>(m_CpuUserHistory.begin(), m_CpuUserHistory.end());
}

std::vector<float> SystemModel::cpuSystemHistory() const
{
    std::shared_lock lock(m_Mutex);
    return std::vector<float>(m_CpuSystemHistory.begin(), m_CpuSystemHistory.end());
}

std::vector<float> SystemModel::cpuIowaitHistory() const
{
    std::shared_lock lock(m_Mutex);
    return std::vector<float>(m_CpuIowaitHistory.begin(), m_CpuIowaitHistory.end());
}

std::vector<float> SystemModel::cpuIdleHistory() const
{
    std::shared_lock lock(m_Mutex);
    return std::vector<float>(m_CpuIdleHistory.begin(), m_CpuIdleHistory.end());
}

std::vector<float> SystemModel::memoryHistory() const
{
    std::shared_lock lock(m_Mutex);
    return std::vector<float>(m_MemoryHistory.begin(), m_MemoryHistory.end());
}

std::vector<float> SystemModel::memoryCachedHistory() const
{
    std::shared_lock lock(m_Mutex);
    return std::vector<float>(m_MemoryCachedHistory.begin(), m_MemoryCachedHistory.end());
}

std::vector<float> SystemModel::swapHistory() const
{
    std::shared_lock lock(m_Mutex);
    return std::vector<float>(m_SwapHistory.begin(), m_SwapHistory.end());
}

std::vector<std::vector<float>> SystemModel::perCoreHistory() const
{
    std::shared_lock lock(m_Mutex);
    std::vector<std::vector<float>> result;
    result.reserve(m_PerCoreHistory.size());

    for (const auto& coreHist : m_PerCoreHistory)
    {
        result.emplace_back(coreHist.begin(), coreHist.end());
    }

    return result;
}

std::vector<double> SystemModel::timestamps() const
{
    std::shared_lock lock(m_Mutex);
    return std::vector<double>(m_Timestamps.begin(), m_Timestamps.end());
}

void SystemModel::computeSnapshot(const Platform::SystemCounters& counters, double nowSeconds)
{
    SystemSnapshot snap;

    // Core count
    snap.coreCount = static_cast<int>(counters.cpuPerCore.size());

    // Memory (always available)
    snap.memoryTotalBytes = counters.memory.totalBytes;
    snap.memoryAvailableBytes = counters.memory.availableBytes;
    snap.memoryCachedBytes = counters.memory.cachedBytes;
    snap.memoryBuffersBytes = counters.memory.buffersBytes;

    // Used = total - available (MemAvailable accounts for cache/buffers that can be freed)
    if (counters.memory.availableBytes > 0)
    {
        snap.memoryUsedBytes = counters.memory.totalBytes - counters.memory.availableBytes;
    }
    else
    {
        // Fallback for older kernels without MemAvailable
        snap.memoryUsedBytes =
            counters.memory.totalBytes - counters.memory.freeBytes - counters.memory.cachedBytes - counters.memory.buffersBytes;
    }

    // Memory percentage
    if (counters.memory.totalBytes > 0)
    {
        const double totalBytes = Numeric::toDouble(counters.memory.totalBytes);
        snap.memoryUsedPercent = 100.0 * (Numeric::toDouble(snap.memoryUsedBytes) / totalBytes);
        snap.memoryCachedPercent = 100.0 * (Numeric::toDouble(snap.memoryCachedBytes) / totalBytes);
    }

    // Swap
    snap.swapTotalBytes = counters.memory.swapTotalBytes;
    snap.swapUsedBytes = counters.memory.swapTotalBytes - counters.memory.swapFreeBytes;
    if (counters.memory.swapTotalBytes > 0)
    {
        const double totalSwapBytes = Numeric::toDouble(counters.memory.swapTotalBytes);
        snap.swapUsedPercent = 100.0 * (Numeric::toDouble(snap.swapUsedBytes) / totalSwapBytes);
    }

    // Uptime
    snap.uptimeSeconds = counters.uptimeSeconds;

    // Static info
    snap.hostname = counters.hostname;
    snap.cpuModel = counters.cpuModel;

    // Load average and CPU frequency
    snap.loadAvg1 = counters.loadAvg1;
    snap.loadAvg5 = counters.loadAvg5;
    snap.loadAvg15 = counters.loadAvg15;
    snap.cpuFreqMHz = counters.cpuFreqMHz;

    // CPU usage (requires previous sample for delta)
    if (m_HasPrevious)
    {
        // Total CPU
        snap.cpuTotal = computeCpuUsage(counters.cpuTotal, m_PrevCounters.cpuTotal);

        // Per-core CPU
        const std::size_t numCores = std::min(counters.cpuPerCore.size(), m_PrevCounters.cpuPerCore.size());
        snap.cpuPerCore.reserve(numCores);

        // Resize per-core history if needed
        if (m_PerCoreHistory.size() < numCores)
        {
            m_PerCoreHistory.resize(numCores);
        }

        for (std::size_t i = 0; i < numCores; ++i)
        {
            auto coreUsage = computeCpuUsage(counters.cpuPerCore[i], m_PrevCounters.cpuPerCore[i]);
            snap.cpuPerCore.push_back(coreUsage);
        }
    }

    // Store snapshot
    m_Snapshot = snap;

    // Update history (only after we have valid deltas)
    if (m_HasPrevious)
    {
        m_CpuHistory.push_back(Numeric::clampPercentToFloat(snap.cpuTotal.totalPercent));
        m_CpuUserHistory.push_back(Numeric::clampPercentToFloat(snap.cpuTotal.userPercent));
        m_CpuSystemHistory.push_back(Numeric::clampPercentToFloat(snap.cpuTotal.systemPercent));
        m_CpuIowaitHistory.push_back(Numeric::clampPercentToFloat(snap.cpuTotal.iowaitPercent));
        m_CpuIdleHistory.push_back(Numeric::clampPercentToFloat(snap.cpuTotal.idlePercent));
        m_MemoryHistory.push_back(Numeric::clampPercentToFloat(snap.memoryUsedPercent));
        m_MemoryCachedHistory.push_back(Numeric::clampPercentToFloat(snap.memoryCachedPercent));
        m_SwapHistory.push_back(Numeric::clampPercentToFloat(snap.swapUsedPercent));
        m_Timestamps.push_back(nowSeconds);

        for (std::size_t i = 0; i < snap.cpuPerCore.size() && i < m_PerCoreHistory.size(); ++i)
        {
            m_PerCoreHistory[i].push_back(Numeric::clampPercentToFloat(snap.cpuPerCore[i].totalPercent));
        }

        trimHistory(nowSeconds);
    }
}

CpuUsage SystemModel::computeCpuUsage(const Platform::CpuCounters& current, const Platform::CpuCounters& previous)
{
    CpuUsage usage;

    const std::uint64_t totalDelta = current.total() - previous.total();
    if (totalDelta == 0)
    {
        return usage; // Avoid division by zero
    }

    const double totalDeltaDouble = Numeric::toDouble(totalDelta);

    auto percent = [totalDeltaDouble](std::uint64_t curr, std::uint64_t prev) -> double
    {
        const std::uint64_t delta = curr - prev;
        return 100.0 * (Numeric::toDouble(delta) / totalDeltaDouble);
    };

    usage.userPercent = percent(current.user + current.nice, previous.user + previous.nice);
    usage.systemPercent = percent(current.system, previous.system);
    usage.idlePercent = percent(current.idle, previous.idle);
    usage.iowaitPercent = percent(current.iowait, previous.iowait);
    usage.stealPercent = percent(current.steal, previous.steal);

    // Total = 100% - idle
    usage.totalPercent = 100.0 - usage.idlePercent;

    // Clamp to valid range
    usage.totalPercent = std::clamp(usage.totalPercent, 0.0, 100.0);
    usage.userPercent = std::clamp(usage.userPercent, 0.0, 100.0);
    usage.systemPercent = std::clamp(usage.systemPercent, 0.0, 100.0);
    usage.idlePercent = std::clamp(usage.idlePercent, 0.0, 100.0);
    usage.iowaitPercent = std::clamp(usage.iowaitPercent, 0.0, 100.0);
    usage.stealPercent = std::clamp(usage.stealPercent, 0.0, 100.0);

    return usage;
}

} // namespace Domain
