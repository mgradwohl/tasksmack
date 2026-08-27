#include "SystemModel.h"

#include "History.h"
#include "Numeric.h"
#include "Platform/IPowerProbe.h"
#include "Platform/ISystemProbe.h"
#include "Platform/PowerTypes.h"
#include "Platform/SystemTypes.h"
#include "SamplingConfig.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <utility>
#include <vector>

namespace Domain
{

SystemModel::SystemModel(std::unique_ptr<Platform::ISystemProbe> probe, std::unique_ptr<Platform::IPowerProbe> powerProbe)
    : m_Probe(std::move(probe)), m_PowerProbe(std::move(powerProbe))
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

    if (m_PowerProbe)
    {
        m_PowerCapabilities = m_PowerProbe->capabilities();
        spdlog::debug("SystemModel: initialized with power probe (hasBattery={})", m_PowerCapabilities.hasBattery);
    }
}

void SystemModel::trimHistory([[maybe_unused]] double nowSeconds)
{
    // With ring buffers, trimming on demand clears them when maxHistorySeconds is 0.
    // Otherwise, ring buffers auto-wrap and we just validate capacity.

    if (m_MaxHistorySeconds == 0.0)
    {
        // Clear all ring buffers - user wants minimal memory usage
        m_Timestamps.clear();
        m_CpuHistory.clear();
        m_CpuUserHistory.clear();
        m_CpuSystemHistory.clear();
        m_CpuIowaitHistory.clear();
        m_CpuIdleHistory.clear();
        m_MemoryHistory.clear();
        m_MemoryCachedHistory.clear();
        m_SwapHistory.clear();
        m_PowerHistory.clear();
        m_BatteryChargeHistory.clear();
        m_NetRxHistory.clear();
        m_NetTxHistory.clear();
        for (auto& entry : m_PerInterfaceRxHistory)
            entry.second.clear();
        for (auto& entry : m_PerInterfaceTxHistory)
            entry.second.clear();
        for (auto& entry : m_PerCoreHistory)
            entry.clear();
        return;
    }

    if (m_Timestamps.empty())
    {
        return;
    }

    // Check if we're approaching or exceeding capacity
    // Ring buffer capacity is HistoryCapacity::STANDARD (1800 samples).
    // At 1Hz sampling, this supports 1800 seconds (30 minutes).
    // For the default 300-second window, we'll never hit this in normal operation.
    if (m_Timestamps.full())
    {
        // Oldest timestamp is at logical index 0
        const double oldestTimestamp = m_Timestamps[0];
        const double newestTimestamp = m_Timestamps.latest();
        const double span = newestTimestamp - oldestTimestamp;

        // Log warning if actual history span exceeds configured max
        if (span > m_MaxHistorySeconds + 1.0) // +1.0 for rounding tolerance
        {
            spdlog::warn("SystemModel: history span ({:.1f}s) exceeds configured max ({:.1f}s); "
                         "oldest data is being discarded. Consider increasing HistoryCapacity::STANDARD.",
                         span,
                         m_MaxHistorySeconds);
        }
    }
}

void SystemModel::setMaxHistorySeconds(double seconds)
{
    std::unique_lock lock(m_Mutex); // NOLINT(misc-const-correctness) - lock guard pattern
    const double clamped = Domain::Sampling::clampHistorySeconds(seconds);
    m_MaxHistorySeconds = clamped;

    // With ring buffers, validate that the requested history window fits in capacity.
    // Ring buffer capacity is HistoryCapacity::STANDARD (1800 samples).
    // At 1Hz sampling, this supports 1800 seconds. For faster sampling rates, effective
    // retention will be less. This is a trade-off for eliminating allocation churn.
    const double maxSupportedSeconds = HistoryCapacity::STANDARD * 1.0; // 1 second per sample at 1Hz
    if (clamped > maxSupportedSeconds)
    {
        spdlog::warn("SystemModel::setMaxHistorySeconds: requested {:.0f}s exceeds ring buffer capacity ({:.0f}s). "
                     "History will be truncated at capacity.",
                     clamped,
                     maxSupportedSeconds);
    }

    if (!m_Timestamps.empty())
    {
        trimHistory(m_Timestamps.latest());
    }
}

void SystemModel::refresh()
{
    if (!m_Probe)
    {
        return;
    }

    auto counters = m_Probe->read();

    // Also read power data if probe is available (outside mutex - it's I/O)
    if (m_PowerProbe)
    {
        auto powerCounters = m_PowerProbe->read();
        auto powerStatus = computePowerStatus(powerCounters);

        // Only lock to update the snapshot
        std::unique_lock lock(m_Mutex); // NOLINT(misc-const-correctness) - lock guard pattern
        m_Snapshot.power = powerStatus;
    }

    updateFromCounters(counters);
}

void SystemModel::updateFromCounters(const Platform::SystemCounters& counters)
{
    const double nowSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    updateFromCounters(counters, nowSeconds);
}

void SystemModel::updateFromCounters(const Platform::SystemCounters& counters, double nowSeconds)
{
    std::unique_lock lock(m_Mutex); // NOLINT(misc-const-correctness) - lock guard pattern
    computeSnapshot(counters, nowSeconds);
    m_PrevCounters = counters;
    m_HasPrevious = true;
    publish();
}

SystemSnapshot SystemModel::snapshot() const
{
    std::shared_lock lock(m_Mutex); // NOLINT(misc-const-correctness) - lock guard pattern
    return m_Snapshot;
}

std::shared_ptr<const SystemPublication> SystemModel::publication() const noexcept
{
    std::shared_lock lock(m_Mutex); // NOLINT(misc-const-correctness) - lock guard pattern
    return m_Publication;
}

std::uint64_t SystemModel::publicationVersion() const noexcept
{
    return m_PublishedPublicationVersion.load(std::memory_order_acquire);
}

void SystemModel::publish()
{
    auto publication = std::make_shared<SystemPublication>();
    publication->version = ++m_PublicationVersion;
    publication->snapshot = m_Snapshot;
    publication->timestamps = HistoryUtils::toVector(m_Timestamps);
    publication->cpuHistory = HistoryUtils::toVector(m_CpuHistory);
    publication->cpuUserHistory = HistoryUtils::toVector(m_CpuUserHistory);
    publication->cpuSystemHistory = HistoryUtils::toVector(m_CpuSystemHistory);
    publication->cpuIowaitHistory = HistoryUtils::toVector(m_CpuIowaitHistory);
    publication->cpuIdleHistory = HistoryUtils::toVector(m_CpuIdleHistory);
    publication->memoryHistory = HistoryUtils::toVector(m_MemoryHistory);
    publication->memoryCachedHistory = HistoryUtils::toVector(m_MemoryCachedHistory);
    publication->swapHistory = HistoryUtils::toVector(m_SwapHistory);
    publication->powerHistory = HistoryUtils::toVector(m_PowerHistory);
    publication->batteryChargeHistory = HistoryUtils::toVector(m_BatteryChargeHistory);
    publication->netRxHistory = HistoryUtils::toVector(m_NetRxHistory);
    publication->netTxHistory = HistoryUtils::toVector(m_NetTxHistory);
    publication->perCoreHistory.reserve(m_PerCoreHistory.size());
    for (const auto& history : m_PerCoreHistory)
    {
        publication->perCoreHistory.push_back(HistoryUtils::toVector(history));
    }
    for (const auto& [name, history] : m_PerInterfaceRxHistory)
    {
        publication->perInterfaceRxHistory.emplace(name, HistoryUtils::toVector(history));
    }
    for (const auto& [name, history] : m_PerInterfaceTxHistory)
    {
        publication->perInterfaceTxHistory.emplace(name, HistoryUtils::toVector(history));
    }
    m_Publication = std::move(publication);
    m_PublishedPublicationVersion.store(m_PublicationVersion, std::memory_order_release);
}

const Platform::SystemCapabilities& SystemModel::capabilities() const
{
    return m_Capabilities;
}

std::vector<float> SystemModel::cpuHistory() const
{
    std::shared_lock lock(m_Mutex); // NOLINT(misc-const-correctness) - lock guard pattern
    return HistoryUtils::toVector(m_CpuHistory);
}

std::vector<float> SystemModel::cpuUserHistory() const
{
    std::shared_lock lock(m_Mutex); // NOLINT(misc-const-correctness) - lock guard pattern
    return HistoryUtils::toVector(m_CpuUserHistory);
}

std::vector<float> SystemModel::cpuSystemHistory() const
{
    std::shared_lock lock(m_Mutex); // NOLINT(misc-const-correctness) - lock guard pattern
    return HistoryUtils::toVector(m_CpuSystemHistory);
}

std::vector<float> SystemModel::cpuIowaitHistory() const
{
    std::shared_lock lock(m_Mutex); // NOLINT(misc-const-correctness) - lock guard pattern
    return HistoryUtils::toVector(m_CpuIowaitHistory);
}

std::vector<float> SystemModel::cpuIdleHistory() const
{
    std::shared_lock lock(m_Mutex); // NOLINT(misc-const-correctness) - lock guard pattern
    return HistoryUtils::toVector(m_CpuIdleHistory);
}

std::vector<float> SystemModel::memoryHistory() const
{
    std::shared_lock lock(m_Mutex); // NOLINT(misc-const-correctness) - lock guard pattern
    return HistoryUtils::toVector(m_MemoryHistory);
}

std::vector<float> SystemModel::powerHistory() const
{
    std::shared_lock lock(m_Mutex); // NOLINT(misc-const-correctness) - lock guard pattern
    return HistoryUtils::toVector(m_PowerHistory);
}

std::vector<float> SystemModel::batteryChargeHistory() const
{
    std::shared_lock lock(m_Mutex); // NOLINT(misc-const-correctness) - lock guard pattern
    return HistoryUtils::toVector(m_BatteryChargeHistory);
}

std::vector<float> SystemModel::netRxHistory() const
{
    std::shared_lock lock(m_Mutex); // NOLINT(misc-const-correctness) - lock guard pattern
    return HistoryUtils::toVector(m_NetRxHistory);
}

std::vector<float> SystemModel::netTxHistory() const
{
    std::shared_lock lock(m_Mutex); // NOLINT(misc-const-correctness) - lock guard pattern
    return HistoryUtils::toVector(m_NetTxHistory);
}

std::vector<float> SystemModel::netRxHistoryForInterface(const std::string& interfaceName) const
{
    std::shared_lock lock(m_Mutex); // NOLINT(misc-const-correctness) - lock guard pattern
    const auto it = m_PerInterfaceRxHistory.find(interfaceName);
    if (it != m_PerInterfaceRxHistory.end())
    {
        return HistoryUtils::toVector(it->second);
    }
    return {};
}

std::vector<float> SystemModel::netTxHistoryForInterface(const std::string& interfaceName) const
{
    std::shared_lock lock(m_Mutex); // NOLINT(misc-const-correctness) - lock guard pattern
    const auto it = m_PerInterfaceTxHistory.find(interfaceName);
    if (it != m_PerInterfaceTxHistory.end())
    {
        return HistoryUtils::toVector(it->second);
    }
    return {};
}

std::vector<float> SystemModel::memoryCachedHistory() const
{
    std::shared_lock lock(m_Mutex); // NOLINT(misc-const-correctness) - lock guard pattern
    return HistoryUtils::toVector(m_MemoryCachedHistory);
}

std::vector<float> SystemModel::swapHistory() const
{
    std::shared_lock lock(m_Mutex); // NOLINT(misc-const-correctness) - lock guard pattern
    return HistoryUtils::toVector(m_SwapHistory);
}

std::vector<std::vector<float>> SystemModel::perCoreHistory() const
{
    std::shared_lock lock(m_Mutex); // NOLINT(misc-const-correctness) - lock guard pattern
    std::vector<std::vector<float>> result;
    result.reserve(m_PerCoreHistory.size());

    for (const auto& coreHist : m_PerCoreHistory)
    {
        result.push_back(HistoryUtils::toVector(coreHist));
    }

    return result;
}

std::vector<double> SystemModel::timestamps() const
{
    std::shared_lock lock(m_Mutex); // NOLINT(misc-const-correctness) - lock guard pattern
    return HistoryUtils::toVector(m_Timestamps);
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

    // Per-interface network - always populate metadata, compute rates only with previous data
    double timeDelta = m_HasPrevious ? (nowSeconds - m_PrevTimestamp) : 0.0;

    // Suppress delta-based rates for implausibly short intervals to prevent
    // large startup rate spikes (e.g. from the immediate first background poll)
    constexpr double MIN_ELAPSED_FOR_RATES = static_cast<double>(Sampling::REFRESH_INTERVAL_MIN_MS) / 2000.0;
    if (m_HasPrevious && timeDelta < MIN_ELAPSED_FOR_RATES)
    {
        timeDelta = 0.0;
    }

    snap.networkInterfaces.reserve(counters.networkInterfaces.size());
    for (const auto& iface : counters.networkInterfaces)
    {
        SystemSnapshot::InterfaceSnapshot ifaceSnap;
        ifaceSnap.name = iface.name;
        ifaceSnap.displayName = iface.displayName;
        ifaceSnap.isUp = iface.isUp;
        ifaceSnap.linkSpeedMbps = iface.linkSpeedMbps;

        // Compute rates only if we have previous data and positive time delta
        if (m_HasPrevious && timeDelta > 0.0)
        {
            const auto* prevIface = findPreviousInterface(iface.name);
            if (prevIface != nullptr)
            {
                if (iface.rxBytes >= prevIface->rxBytes)
                {
                    ifaceSnap.rxBytesPerSec = Numeric::counterRate(iface.rxBytes, prevIface->rxBytes, timeDelta);
                }
                if (iface.txBytes >= prevIface->txBytes)
                {
                    ifaceSnap.txBytesPerSec = Numeric::counterRate(iface.txBytes, prevIface->txBytes, timeDelta);
                }
            }
        }

        snap.networkInterfaces.push_back(std::move(ifaceSnap));
    }

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

        // Compute total network rates (aggregate of all interfaces, bytes per second)
        if (timeDelta > 0.0)
        {
            // Only compute if counters increased (handle overflow/restart)
            if (counters.netRxBytes >= m_PrevCounters.netRxBytes)
            {
                snap.netRxBytesPerSec = Numeric::counterRate(counters.netRxBytes, m_PrevCounters.netRxBytes, timeDelta);
            }
            if (counters.netTxBytes >= m_PrevCounters.netTxBytes)
            {
                snap.netTxBytesPerSec = Numeric::counterRate(counters.netTxBytes, m_PrevCounters.netTxBytes, timeDelta);
            }
        }
    }

    // Store snapshot (preserve power status that was set separately in refresh())
    const auto preservedPower = m_Snapshot.power;
    m_Snapshot = snap;
    m_Snapshot.power = preservedPower;

    // Update history (only after we have valid deltas)
    if (m_HasPrevious)
    {
        m_CpuHistory.push(Numeric::clampPercentToFloat(snap.cpuTotal.totalPercent));
        m_CpuUserHistory.push(Numeric::clampPercentToFloat(snap.cpuTotal.userPercent));
        m_CpuSystemHistory.push(Numeric::clampPercentToFloat(snap.cpuTotal.systemPercent));
        m_CpuIowaitHistory.push(Numeric::clampPercentToFloat(snap.cpuTotal.iowaitPercent));
        m_CpuIdleHistory.push(Numeric::clampPercentToFloat(snap.cpuTotal.idlePercent));
        m_MemoryHistory.push(Numeric::clampPercentToFloat(snap.memoryUsedPercent));
        m_MemoryCachedHistory.push(Numeric::clampPercentToFloat(snap.memoryCachedPercent));
        m_SwapHistory.push(Numeric::clampPercentToFloat(snap.swapUsedPercent));
        m_PowerHistory.push(static_cast<float>(preservedPower.powerWatts));
        // Track battery charge % if available (0-100 range, use -1 as "no data")
        const float chargeVal = preservedPower.hasBattery ? static_cast<float>(preservedPower.chargePercent) : -1.0F;
        m_BatteryChargeHistory.push(chargeVal);
        // Network history (bytes per second)
        m_NetRxHistory.push(static_cast<float>(snap.netRxBytesPerSec));
        m_NetTxHistory.push(static_cast<float>(snap.netTxBytesPerSec));

        // Per-interface network history
        for (const auto& ifaceSnap : snap.networkInterfaces)
        {
            m_PerInterfaceRxHistory[ifaceSnap.name].push(static_cast<float>(ifaceSnap.rxBytesPerSec));
            m_PerInterfaceTxHistory[ifaceSnap.name].push(static_cast<float>(ifaceSnap.txBytesPerSec));
        }

        m_Timestamps.push(nowSeconds);

        for (std::size_t i = 0; i < snap.cpuPerCore.size() && i < m_PerCoreHistory.size(); ++i)
        {
            m_PerCoreHistory[i].push(Numeric::clampPercentToFloat(snap.cpuPerCore[i].totalPercent));
        }

        trimHistory(nowSeconds);
    }

    // Update previous timestamp for next iteration
    m_PrevTimestamp = nowSeconds;
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

PowerStatus SystemModel::computePowerStatus(const Platform::PowerCounters& counters) const
{
    PowerStatus status;

    status.hasBattery = m_PowerCapabilities.hasBattery;

    if (!status.hasBattery)
    {
        return status;
    }

    // Basic state
    status.isOnAc = counters.isOnAc;
    status.isCharging = (counters.state == Platform::BatteryState::Charging);
    status.isDischarging = (counters.state == Platform::BatteryState::Discharging);
    status.isFull = (counters.state == Platform::BatteryState::Full);

    // Charge percentage
    status.chargePercent = counters.chargePercent;

    // Power consumption
    status.powerWatts = counters.powerNowW;

    // Health percentage
    status.healthPercent = counters.healthPercent;

    // Time estimates
    status.timeToEmptySec = counters.timeToEmptySec;
    status.timeToFullSec = counters.timeToFullSec;

    // Battery details
    status.technology = counters.technology;
    status.model = counters.model;

    return status;
}

const Platform::SystemCounters::InterfaceCounters* SystemModel::findPreviousInterface(const std::string& name) const
{
    auto it = std::ranges::find_if(m_PrevCounters.networkInterfaces, [&name](const auto& iface) { return iface.name == name; });
    return (it != m_PrevCounters.networkInterfaces.end()) ? &(*it) : nullptr;
}

} // namespace Domain
