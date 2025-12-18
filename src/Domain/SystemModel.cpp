#include "SystemModel.h"

#include <spdlog/spdlog.h>

#include <algorithm>

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
    std::unique_lock lock(m_Mutex);
    computeSnapshot(counters);
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
    std::vector<float> result(m_CpuHistory.size());
    m_CpuHistory.copyTo(result.data(), result.size());
    return result;
}

std::vector<float> SystemModel::cpuUserHistory() const
{
    std::shared_lock lock(m_Mutex);
    std::vector<float> result(m_CpuUserHistory.size());
    m_CpuUserHistory.copyTo(result.data(), result.size());
    return result;
}

std::vector<float> SystemModel::cpuSystemHistory() const
{
    std::shared_lock lock(m_Mutex);
    std::vector<float> result(m_CpuSystemHistory.size());
    m_CpuSystemHistory.copyTo(result.data(), result.size());
    return result;
}

std::vector<float> SystemModel::cpuIowaitHistory() const
{
    std::shared_lock lock(m_Mutex);
    std::vector<float> result(m_CpuIowaitHistory.size());
    m_CpuIowaitHistory.copyTo(result.data(), result.size());
    return result;
}

std::vector<float> SystemModel::cpuIdleHistory() const
{
    std::shared_lock lock(m_Mutex);
    std::vector<float> result(m_CpuIdleHistory.size());
    m_CpuIdleHistory.copyTo(result.data(), result.size());
    return result;
}

std::vector<float> SystemModel::memoryHistory() const
{
    std::shared_lock lock(m_Mutex);
    std::vector<float> result(m_MemoryHistory.size());
    m_MemoryHistory.copyTo(result.data(), result.size());
    return result;
}

std::vector<float> SystemModel::swapHistory() const
{
    std::shared_lock lock(m_Mutex);
    std::vector<float> result(m_SwapHistory.size());
    m_SwapHistory.copyTo(result.data(), result.size());
    return result;
}

std::vector<std::vector<float>> SystemModel::perCoreHistory() const
{
    std::shared_lock lock(m_Mutex);
    std::vector<std::vector<float>> result;
    result.reserve(m_PerCoreHistory.size());

    for (const auto& coreHist : m_PerCoreHistory)
    {
        std::vector<float> coreData(coreHist.size());
        coreHist.copyTo(coreData.data(), coreData.size());
        result.push_back(std::move(coreData));
    }

    return result;
}

void SystemModel::computeSnapshot(const Platform::SystemCounters& counters)
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
        snap.memoryUsedPercent = 100.0 * static_cast<double>(snap.memoryUsedBytes) / static_cast<double>(counters.memory.totalBytes);
    }

    // Swap
    snap.swapTotalBytes = counters.memory.swapTotalBytes;
    snap.swapUsedBytes = counters.memory.swapTotalBytes - counters.memory.swapFreeBytes;
    if (counters.memory.swapTotalBytes > 0)
    {
        snap.swapUsedPercent = 100.0 * static_cast<double>(snap.swapUsedBytes) / static_cast<double>(counters.memory.swapTotalBytes);
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
        size_t numCores = std::min(counters.cpuPerCore.size(), m_PrevCounters.cpuPerCore.size());
        snap.cpuPerCore.reserve(numCores);

        // Resize per-core history if needed
        if (m_PerCoreHistory.size() < numCores)
        {
            m_PerCoreHistory.resize(numCores);
        }

        for (size_t i = 0; i < numCores; ++i)
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
        m_CpuHistory.push(static_cast<float>(snap.cpuTotal.totalPercent));
        m_CpuUserHistory.push(static_cast<float>(snap.cpuTotal.userPercent));
        m_CpuSystemHistory.push(static_cast<float>(snap.cpuTotal.systemPercent));
        m_CpuIowaitHistory.push(static_cast<float>(snap.cpuTotal.iowaitPercent));
        m_CpuIdleHistory.push(static_cast<float>(snap.cpuTotal.idlePercent));
        m_MemoryHistory.push(static_cast<float>(snap.memoryUsedPercent));
        m_SwapHistory.push(static_cast<float>(snap.swapUsedPercent));

        for (size_t i = 0; i < snap.cpuPerCore.size() && i < m_PerCoreHistory.size(); ++i)
        {
            m_PerCoreHistory[i].push(static_cast<float>(snap.cpuPerCore[i].totalPercent));
        }
    }
}

CpuUsage SystemModel::computeCpuUsage(const Platform::CpuCounters& current, const Platform::CpuCounters& previous)
{
    CpuUsage usage;

    uint64_t totalDelta = current.total() - previous.total();
    if (totalDelta == 0)
    {
        return usage; // Avoid division by zero
    }

    auto percent = [totalDelta](uint64_t curr, uint64_t prev) -> double
    {
        uint64_t delta = curr - prev;
        return 100.0 * static_cast<double>(delta) / static_cast<double>(totalDelta);
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
