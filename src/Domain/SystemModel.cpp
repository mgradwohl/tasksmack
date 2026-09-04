#include "SystemModel.h"

#include "History.h"
#include "Numeric.h"
#include "Platform/IPowerProbe.h"
#include "Platform/ISystemProbe.h"
#include "Platform/PowerTypes.h"
#include "Platform/SystemTypes.h"
#include "SamplingConfig.h"
#include "SystemSnapshot.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
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

    applyHistoryCapacity();
}

void SystemModel::applyHistoryCapacity()
{
    // Size every ring so the configured time window fits even at the fastest
    // supported refresh cadence; time-based trimming governs actual retention.
    const std::size_t capacity = Sampling::historyCapacityForSeconds(m_MaxHistorySeconds);
    m_Timestamps.setCapacity(capacity);
    m_CpuHistory.setCapacity(capacity);
    m_CpuUserHistory.setCapacity(capacity);
    m_CpuSystemHistory.setCapacity(capacity);
    m_CpuIowaitHistory.setCapacity(capacity);
    m_CpuIdleHistory.setCapacity(capacity);
    m_MemoryHistory.setCapacity(capacity);
    m_MemoryCachedHistory.setCapacity(capacity);
    m_SwapHistory.setCapacity(capacity);
    m_PowerHistory.setCapacity(capacity);
    m_BatteryChargeHistory.setCapacity(capacity);
    m_NetRxHistory.setCapacity(capacity);
    m_NetTxHistory.setCapacity(capacity);
    for (auto& [name, history] : m_PerInterfaceRxHistory)
    {
        history.setCapacity(capacity);
    }
    for (auto& [name, history] : m_PerInterfaceTxHistory)
    {
        history.setCapacity(capacity);
    }
    for (auto& coreHistory : m_PerCoreHistory)
    {
        coreHistory.setCapacity(capacity);
    }
}

void SystemModel::trimHistory(double nowSeconds)
{
    // Drop entries older than the configured time window. All rings are pushed
    // in lockstep with m_Timestamps, so a single discard count keeps them
    // aligned. discardFront is O(1): no copies, rebuilds, or allocations.
    const double cutoff = nowSeconds - m_MaxHistorySeconds;
    const std::size_t removeCount = HistoryUtils::discardBefore(m_Timestamps,
                                                                cutoff,
                                                                m_CpuHistory,
                                                                m_CpuUserHistory,
                                                                m_CpuSystemHistory,
                                                                m_CpuIowaitHistory,
                                                                m_CpuIdleHistory,
                                                                m_MemoryHistory,
                                                                m_MemoryCachedHistory,
                                                                m_SwapHistory,
                                                                m_PowerHistory,
                                                                m_BatteryChargeHistory,
                                                                m_NetRxHistory,
                                                                m_NetTxHistory);

    for (auto& [name, history] : m_PerInterfaceRxHistory)
    {
        history.discardFront(removeCount);
    }
    for (auto& [name, history] : m_PerInterfaceTxHistory)
    {
        history.discardFront(removeCount);
    }
    for (auto& coreHistory : m_PerCoreHistory)
    {
        coreHistory.discardFront(removeCount);
    }
}

void SystemModel::setMaxHistorySeconds(double seconds)
{
    std::unique_lock lock(m_Mutex); // NOLINT(misc-const-correctness) - lock guard pattern
    m_MaxHistorySeconds = Domain::Sampling::clampHistorySeconds(seconds);
    applyHistoryCapacity();

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

    // Also read power data if probe is available (outside mutex - it's I/O). Applied to
    // the snapshot inside updateFromCountersLocked() below, under the same lock as the
    // counter-derived fields, so a reader never observes this cycle's power paired with
    // the previous cycle's CPU/memory/network data.
    std::optional<PowerStatus> powerStatus;
    if (m_PowerProbe)
    {
        powerStatus = computePowerStatus(m_PowerProbe->read());
    }

    const double nowSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    updateFromCountersLocked(counters, nowSeconds, powerStatus);
}

void SystemModel::updateFromCounters(const Platform::SystemCounters& counters)
{
    const double nowSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    updateFromCounters(counters, nowSeconds);
}

void SystemModel::updateFromCounters(const Platform::SystemCounters& counters, double nowSeconds)
{
    updateFromCountersLocked(counters, nowSeconds, std::nullopt);
}

void SystemModel::updateFromCountersLocked(const Platform::SystemCounters& counters,
                                           double nowSeconds,
                                           const std::optional<PowerStatus>& powerStatus)
{
    std::unique_lock lock(m_Mutex); // NOLINT(misc-const-correctness) - lock guard pattern
    if (powerStatus.has_value())
    {
        m_Snapshot.power = *powerStatus;
    }
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
    // Assign the version from a local candidate rather than mutating m_PublicationVersion
    // directly here: the history copies below can throw (std::bad_alloc), and if they do,
    // committing m_PublicationVersion/m_Publication/m_PublishedPublicationVersion only at
    // the end (see below) keeps all three mutually consistent instead of silently advancing
    // the version past what was actually published.
    publication->version = m_PublicationVersion + 1;
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
    m_PublicationVersion = publication->version;
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

        // Resize per-core history if needed (new cores get zero backfill so all
        // rings stay in lockstep with m_Timestamps)
        if (m_PerCoreHistory.size() < numCores)
        {
            const std::size_t capacity = Sampling::historyCapacityForSeconds(m_MaxHistorySeconds);
            const std::size_t backfillCount = std::min(m_Timestamps.size(), capacity - 1);
            const std::size_t oldSize = m_PerCoreHistory.size();
            m_PerCoreHistory.resize(numCores);
            for (std::size_t i = oldSize; i < numCores; ++i)
            {
                m_PerCoreHistory[i].setCapacity(capacity);
                for (std::size_t j = 0; j < backfillCount; ++j)
                {
                    m_PerCoreHistory[i].push(0.0F);
                }
            }
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

        // Per-interface network history. New interfaces get zero backfill (clamped to
        // ring capacity) so they align with m_Timestamps. Known interfaces absent from
        // this sample get a 0.0F placeholder so every series stays index-aligned.
        // Avoid allocating a hash-set on the hot path: the interface list is small
        // (typically < 10 entries), so a linear scan is cheaper than hashing.
        auto ifacePresent = [&snap](const std::string& name) -> bool
        {
            return std::ranges::any_of(snap.networkInterfaces, [&name](const auto& ifaceSnap) { return ifaceSnap.name == name; });
        };
        ++m_CurrentGeneration;
        for (const auto& ifaceSnap : snap.networkInterfaces)
        {
            const auto& name = ifaceSnap.name;
            auto ensureAligned = [this](auto& map, const std::string& ifName) -> auto&
            {
                auto [it, inserted] = map.try_emplace(ifName);
                if (inserted)
                {
                    const std::size_t capacity = Sampling::historyCapacityForSeconds(m_MaxHistorySeconds);
                    it->second.setCapacity(capacity);
                    const std::size_t backfillCount = std::min(m_Timestamps.size(), capacity - 1);
                    for (std::size_t j = 0; j < backfillCount; ++j)
                    {
                        it->second.push(0.0F);
                    }
                }
                return it->second;
            };
            ensureAligned(m_PerInterfaceRxHistory, name).push(static_cast<float>(ifaceSnap.rxBytesPerSec));
            ensureAligned(m_PerInterfaceTxHistory, name).push(static_cast<float>(ifaceSnap.txBytesPerSec));
            m_InterfaceLastSeenGeneration[name] = m_CurrentGeneration;
        }
        // Push 0.0F placeholder for known interfaces absent from this sample.
        // Iterating m_PerInterfaceRxHistory and mutating only the mapped values
        // (not inserting/erasing keys) does not invalidate the iterator, so no
        // scratch vector is needed.  m_PerInterfaceTxHistory always has the same
        // key set (both maps are always updated together), so .at() is safe.
        for (auto& [name, rxBuf] : m_PerInterfaceRxHistory)
        {
            if (!ifacePresent(name))
            {
                rxBuf.push(0.0F);
                m_PerInterfaceTxHistory.at(name).push(0.0F);
            }
        }

        // Prune interfaces absent for longer than the whole history window: by that point
        // their buffers hold nothing but the 0.0F padding just pushed above, so removing the
        // entry changes nothing observable (a fully zero-padded buffer and a missing key both
        // present as "no recent data" via netRxHistoryForInterface()/netTxHistoryForInterface()),
        // but retaining it forever would grow these maps without bound on a machine with
        // churning interfaces (#776).
        {
            const std::size_t historyCapacity = Sampling::historyCapacityForSeconds(m_MaxHistorySeconds);
            std::vector<std::string> staleInterfaces;
            for (const auto& [name, lastSeen] : m_InterfaceLastSeenGeneration)
            {
                if ((m_CurrentGeneration - lastSeen) > historyCapacity)
                {
                    staleInterfaces.push_back(name);
                }
            }
            for (const auto& name : staleInterfaces)
            {
                m_PerInterfaceRxHistory.erase(name);
                m_PerInterfaceTxHistory.erase(name);
                m_InterfaceLastSeenGeneration.erase(name);
            }
        }

        m_Timestamps.push(nowSeconds);

        // Advance rings for present cores; push 0.0F for any retained rings beyond
        // the reported core count so every core series stays aligned with m_Timestamps.
        for (std::size_t i = 0; i < m_PerCoreHistory.size(); ++i)
        {
            if (i < snap.cpuPerCore.size())
            {
                m_PerCoreHistory[i].push(Numeric::clampPercentToFloat(snap.cpuPerCore[i].totalPercent));
            }
            else
            {
                m_PerCoreHistory[i].push(0.0F);
            }
        }

        trimHistory(nowSeconds);
    }

    // Update previous timestamp for next iteration
    m_PrevTimestamp = nowSeconds;
}

CpuUsage SystemModel::computeCpuUsage(const Platform::CpuCounters& current, const Platform::CpuCounters& previous)
{
    CpuUsage usage;

    // counterDelta() clamps to 0 instead of wrapping if a field regresses (per-core CPU
    // hotplug/offline-online reindexing, a transiently stale counter, etc.) - without it, an
    // unsigned underflow here would silently pin the reported percentage at 100%.
    const std::uint64_t totalDelta = Numeric::counterDelta(current.total(), previous.total());
    if (totalDelta == 0)
    {
        return usage; // Avoid division by zero
    }

    const double totalDeltaDouble = Numeric::toDouble(totalDelta);

    auto percent = [totalDeltaDouble](std::uint64_t curr, std::uint64_t prev) -> double
    {
        const std::uint64_t delta = Numeric::counterDelta(curr, prev);
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
