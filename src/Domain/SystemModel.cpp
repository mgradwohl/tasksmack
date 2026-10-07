#include "SystemModel.h"

#include "History.h"
#include "Numeric.h"
#include "Platform/IPowerProbe.h"
#include "Platform/ISystemProbe.h"
#include "Platform/PowerTypes.h"
#include "Platform/SystemTypes.h"
#include "PublicationSlot.h"
#include "SamplingConfig.h"
#include "SystemSnapshot.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <utility>
#include <vector>

namespace Domain
{

namespace
{

/// Per-core slots are indexed by core id, so an id bounds the vectors' size. Linux's largest
/// NR_CPUS (MAXSMP) is 8192 and Windows tops out at 2048 logical processors: a larger id is
/// malformed input, not a core, and is dropped rather than sizing every per-core vector to it.
constexpr std::size_t MAX_CORE_SLOTS = 8192;

/// Slots needed to index these cores by id: the highest plausible core id plus one.
[[nodiscard]] std::size_t coreSlotCount(const std::vector<Platform::CpuCounters>& cores) noexcept
{
    std::size_t slots = 0;
    for (const auto& core : cores)
    {
        if (core.coreId < MAX_CORE_SLOTS)
        {
            slots = std::max(slots, core.coreId + 1);
        }
    }
    return slots;
}

/// Fill `index` with the positions of `interfaces`, sorted by name (ties by position, so a lookup
/// finds the first of a repeated name, as a linear scan would). Reuses the vector's capacity, and
/// std::ranges::sort, unlike stable_sort, needs no buffer: no allocation per sample once warm (#1415).
void buildInterfaceIndex(const std::vector<Platform::SystemCounters::InterfaceCounters>& interfaces, std::vector<std::size_t>& index)
{
    index.resize(interfaces.size());
    for (std::size_t i = 0; i < index.size(); ++i)
    {
        index[i] = i;
    }
    std::ranges::sort(index,
                      [&interfaces](std::size_t lhs, std::size_t rhs)
                      {
                          const int order = interfaces[lhs].name.compare(interfaces[rhs].name);
                          return (order != 0) ? (order < 0) : (lhs < rhs);
                      });
}

/// The first interface named `name`, by binary search of an index from buildInterfaceIndex().
[[nodiscard]] const Platform::SystemCounters::InterfaceCounters*
findInterface(const std::vector<Platform::SystemCounters::InterfaceCounters>& interfaces,
              const std::vector<std::size_t>& index,
              const std::string& name)
{
    const auto it = std::ranges::lower_bound(
        index, name, std::less<>{}, [&interfaces](std::size_t position) -> const std::string& { return interfaces[position].name; });
    return (it != index.end() && interfaces[*it].name == name) ? &interfaces[*it] : nullptr;
}

/// A per-core slot with no reading this sample: NaN, drawn as a gap and shown as N/A (#1146).
[[nodiscard]] CpuUsage noCpuReading() noexcept
{
    constexpr double NO_READING = std::numeric_limits<double>::quiet_NaN();
    return CpuUsage{.totalPercent = NO_READING,
                    .userPercent = NO_READING,
                    .systemPercent = NO_READING,
                    .idlePercent = NO_READING,
                    .iowaitPercent = NO_READING,
                    .stealPercent = NO_READING};
}

} // namespace

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
    // Drop entries older than the configured time window, except the newest of them while a newer
    // sample remains: the anchor that lets a chart's line run off the window's left edge (see
    // HistoryUtils::discardBefore, #1016). All rings are pushed in lockstep with m_Timestamps, so a
    // single discard count keeps them aligned. discardFront is O(1): no copies, rebuilds, or allocations.
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

double SystemModel::maxHistorySeconds() const
{
    const std::shared_lock lock(m_Mutex);
    return m_MaxHistorySeconds;
}

void SystemModel::setMaxHistorySeconds(double seconds)
{
    const std::scoped_lock writerLock(m_WriterMutex);
    {
        const std::unique_lock lock(m_Mutex);
        m_MaxHistorySeconds = Domain::Sampling::clampHistorySeconds(seconds);
        applyHistoryCapacity();

        if (!m_Timestamps.empty())
        {
            trimHistory(m_Timestamps.latest());
        }
    }
    // Republish the trimmed history now rather than at the next sample, which can be several
    // seconds away while sampling is throttled: until then the charts kept the old window's data,
    // scale and peaks (#1145). Nothing is published before the first sample.
    if (m_PublicationVersion != 0)
    {
        publish();
    }
}

void SystemModel::setMaxSaneNetworkRate(double bytesPerSecond) noexcept
{
    m_MaxSaneNetworkRateBps.store(Sampling::clampMaxSaneRateBps(bytesPerSecond), std::memory_order_relaxed);
}

void SystemModel::refresh()
{
    if (!m_Probe)
    {
        return;
    }

    auto counters = m_Probe->read();
    // Stamped as soon as the counters are read, as StorageModel does: the power read below has
    // its own, variable latency (sysfs, WMI), which would otherwise jitter the rate interval (#1144).
    const double nowSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();

    // Also read power data if probe is available (outside mutex - it's I/O). Applied to
    // the snapshot inside updateFromCountersLocked() below, under the same lock as the
    // counter-derived fields, so a reader never observes this cycle's power paired with
    // the previous cycle's CPU/memory/network data.
    std::optional<PowerStatus> powerStatus;
    if (m_PowerProbe)
    {
        powerStatus = computePowerStatus(m_PowerProbe->read());
    }

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
    const std::scoped_lock writerLock(m_WriterMutex);
    {
        const std::unique_lock lock(m_Mutex);
        if (powerStatus.has_value())
        {
            m_Snapshot.power = *powerStatus;
        }
        computeSnapshot(counters, nowSeconds);
        m_PrevCounters = counters;
        // computeSnapshot() indexed these counters' interfaces; they are the previous ones now.
        std::swap(m_PrevInterfaceIndex, m_InterfaceIndex);
        m_HasPrevious = true;
    }
    // Outside the exclusive lock: the history copies take a shared lock only (#868).
    publish();
}

SystemSnapshot SystemModel::snapshot() const
{
    std::shared_lock lock(m_Mutex); // NOLINT(misc-const-correctness) - lock guard pattern
    return m_Snapshot;
}

std::shared_ptr<const SystemPublication> SystemModel::publication() const noexcept
{
    return m_Publication.load();
}

std::uint64_t SystemModel::publicationVersion() const noexcept
{
    return m_Publication.version();
}

void SystemModel::publish()
{
    // Build contents first, commit validity keys last: the version comes from a local candidate and
    // m_PublicationVersion only advances once the generation is committed, so a throw from the copies
    // below (std::bad_alloc) leaves the published generation, its version and m_PublicationVersion
    // consistent. The copies -- up to every history ring at the longest window -- run under a shared
    // lock: snapshot() and the per-field accessors still read alongside, and publication() doesn't
    // take m_Mutex at all, so no reader waits for them (#868). Nothing else can write this state
    // meanwhile; the caller holds m_WriterMutex.
    auto publication = std::make_shared<SystemPublication>();
    {
        const std::shared_lock stateLock(m_Mutex);
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
    }
    const std::uint64_t version = publication->version;
    m_Publication.commit(std::move(publication));
    m_PublicationVersion = version;
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

    // Core count: the cores reported this sample, not counting an implausible id coreSlotCount()
    // drops, so the UI never shows a core that has no slot (#1229).
    snap.coreCount = static_cast<int>(
        std::ranges::count_if(counters.cpuPerCore, [](const Platform::CpuCounters& core) { return core.coreId < MAX_CORE_SLOTS; }));

    // The core ids seen this session: only these get a chart, not every slot up to the highest id
    // (#1262). Never pruned, so a CPU that goes offline keeps its chart, with a gap (#1229).
    for (const auto& core : counters.cpuPerCore)
    {
        if (core.coreId >= MAX_CORE_SLOTS)
        {
            continue;
        }
        if (const auto at = std::ranges::lower_bound(m_SeenCoreIds, core.coreId); at == m_SeenCoreIds.end() || *at != core.coreId)
        {
            m_SeenCoreIds.insert(at, core.coreId);
        }
    }
    snap.seenCoreIds = m_SeenCoreIds;

    // Memory (always available)
    snap.memoryTotalBytes = counters.memory.totalBytes;
    snap.memoryAvailableBytes = counters.memory.availableBytes;
    snap.memoryCachedBytes = counters.memory.cachedBytes;
    snap.memoryBuffersBytes = counters.memory.buffersBytes;

    // Used = total - available (MemAvailable accounts for cache/buffers that can be freed). A
    // MemAvailable of 0 is memory exhausted, not missing, so only a kernel without it takes the
    // legacy formula. Both subtractions saturate at 0: a container (LXCFS) can report available
    // above total, which wrapped to about 16 EiB used (#1143).
    const auto saturatingSub = [](std::uint64_t a, std::uint64_t b) -> std::uint64_t
    {
        return (a > b) ? a - b : 0;
    };
    if (counters.memory.hasAvailableBytes)
    {
        snap.memoryUsedBytes = saturatingSub(counters.memory.totalBytes, counters.memory.availableBytes);
    }
    else
    {
        // Subtract each part in turn: summing them first could wrap (#1232 review).
        snap.memoryUsedBytes =
            saturatingSub(saturatingSub(saturatingSub(counters.memory.totalBytes, counters.memory.freeBytes), counters.memory.cachedBytes),
                          counters.memory.buffersBytes);
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
    // Guarded: a probe reporting more free than total must read as 0 used, not wrap to ~2^64
    // (the Windows probe once did exactly that, pinning swap at 100 %, #1026).
    snap.swapUsedBytes = (counters.memory.swapFreeBytes < counters.memory.swapTotalBytes)
                           ? (counters.memory.swapTotalBytes - counters.memory.swapFreeBytes)
                           : 0;
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

    // A rate above the configured ceiling ([metrics] max_sane_rate_bps) is a counter glitch -- a
    // driver reset, a reinitialised or re-registered counter -- not traffic: it reads 0 here and is
    // a gap in the history, so one bogus sample can't blow out the chart's scale (#1291).
    const double maxSaneRate = m_MaxSaneNetworkRateBps.load(std::memory_order_relaxed);
    struct RateGap
    {
        bool rx = false;
        bool tx = false;
    };
    std::vector<RateGap> interfaceGaps(counters.networkInterfaces.size());
    RateGap totalGap;
    const auto saneRate = [maxSaneRate](double rate, bool& gap)
    {
        gap = rate > maxSaneRate;
        return gap ? 0.0 : rate;
    };

    // Name lookups into this sample's interfaces and the previous sample's go through sorted
    // indexes, not linear scans: container hosts run hundreds of veth/bridge interfaces, and a scan
    // per interface made each sample quadratic in their number (#1415). The previous sample's index
    // is this one's, kept from the last call (see updateFromCountersLocked()).
    buildInterfaceIndex(counters.networkInterfaces, m_InterfaceIndex);

    snap.networkInterfaces.reserve(counters.networkInterfaces.size());
    for (std::size_t ifaceIndex = 0; ifaceIndex < counters.networkInterfaces.size(); ++ifaceIndex)
    {
        const auto& iface = counters.networkInterfaces[ifaceIndex];
        auto& gap = interfaceGaps[ifaceIndex];
        SystemSnapshot::InterfaceSnapshot ifaceSnap;
        ifaceSnap.name = iface.name;
        ifaceSnap.displayName = iface.displayName;
        ifaceSnap.isUp = iface.isUp;
        ifaceSnap.linkSpeedMbps = iface.linkSpeedMbps;
        ifaceSnap.isVirtual = iface.isVirtual;
        ifaceSnap.isVirtualKnown = iface.isVirtualKnown;

        // Compute rates only if we have previous data and positive time delta
        if (m_HasPrevious && timeDelta > 0.0)
        {
            const auto* prevIface = findPreviousInterface(iface.name);
            if (prevIface != nullptr)
            {
                if (iface.rxBytes >= prevIface->rxBytes)
                {
                    ifaceSnap.rxBytesPerSec = saneRate(Numeric::counterRate(iface.rxBytes, prevIface->rxBytes, timeDelta), gap.rx);
                }
                if (iface.txBytes >= prevIface->txBytes)
                {
                    ifaceSnap.txBytesPerSec = saneRate(Numeric::counterRate(iface.txBytes, prevIface->txBytes, timeDelta), gap.tx);
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

        // Per-core CPU, matched by core id (the Linux cpuN), never by list position. The kernel
        // lists online CPUs only, so with cpu2 offline cpu3 is third in the list: matching by
        // position diffed cpu3 against the previous sample's cpu2 and charted every later core
        // under the wrong label (#1229). Slots are indexed by core id; a core missing from either
        // sample has no delta and keeps the NaN "no reading" slot.
        const std::size_t slotCount = std::max(coreSlotCount(counters.cpuPerCore), coreSlotCount(m_PrevCounters.cpuPerCore));

        std::vector<const Platform::CpuCounters*> previousById(slotCount, nullptr);
        for (const auto& core : m_PrevCounters.cpuPerCore)
        {
            if (core.coreId < slotCount && previousById[core.coreId] == nullptr)
            {
                previousById[core.coreId] = &core;
            }
        }

        snap.cpuPerCore.assign(slotCount, noCpuReading());
        for (const auto& core : counters.cpuPerCore)
        {
            if (core.coreId >= slotCount)
            {
                continue;
            }
            const Platform::CpuCounters* previous = previousById[core.coreId];
            // Skip a core with no previous sample, and a repeated id (the first entry wins).
            if (previous == nullptr || !std::isnan(snap.cpuPerCore[core.coreId].totalPercent))
            {
                continue;
            }
            snap.cpuPerCore[core.coreId] = computeCpuUsage(core, *previous);
        }

        // Grow per-core history to cover every core id seen. A new core's ring is backfilled with
        // NaN -- no reading, drawn as a gap (#1146) -- so all rings stay in lockstep with m_Timestamps.
        if (m_PerCoreHistory.size() < slotCount)
        {
            const std::size_t capacity = Sampling::historyCapacityForSeconds(m_MaxHistorySeconds);
            const std::size_t backfillCount = std::min(m_Timestamps.size(), capacity - 1);
            const std::size_t oldSize = m_PerCoreHistory.size();
            m_PerCoreHistory.resize(slotCount);
            for (std::size_t i = oldSize; i < slotCount; ++i)
            {
                m_PerCoreHistory[i].setCapacity(capacity);
                for (std::size_t j = 0; j < backfillCount; ++j)
                {
                    m_PerCoreHistory[i].push(std::numeric_limits<float>::quiet_NaN());
                }
            }
        }

        // Total network rate is the sum of the per-interface rates computed above, not the change in
        // the summed lifetime counters. With the summed counters, an interface appearing (a VPN
        // connecting, WSL starting a vEthernet adapter) delivered its whole lifetime byte count in
        // one sample -- single-sample spikes of 50-110 MB/s on an idle Wi-Fi link that then pinned
        // the axis for the history window -- and one disappearing read as a counter rollback, 0
        // (#1030). Per interface, a new one has no rate until its second sample. The aggregate
        // counters remain the fallback for a probe that reports no per-interface data.
        // Virtual interfaces (bridges, veth, VPN tunnels) are left out: their traffic also crosses a
        // hardware interface, and counting both doubled the Total on machines running Docker, WSL or
        // a VPN (#1106). If every interface is virtual (inside a container) they all count.
        if (timeDelta > 0.0 && !snap.networkInterfaces.empty())
        {
            const bool anyHardware =
                std::ranges::any_of(snap.networkInterfaces, [](const auto& ifaceSnap) { return !ifaceSnap.isVirtual; });
            for (std::size_t ifaceIndex = 0; ifaceIndex < snap.networkInterfaces.size(); ++ifaceIndex)
            {
                const auto& ifaceSnap = snap.networkInterfaces[ifaceIndex];
                if (anyHardware && ifaceSnap.isVirtual)
                {
                    continue;
                }
                snap.netRxBytesPerSec += ifaceSnap.rxBytesPerSec;
                snap.netTxBytesPerSec += ifaceSnap.txBytesPerSec;
                // A Total missing a counted interface's glitched sample isn't a measurement either.
                totalGap.rx = totalGap.rx || interfaceGaps[ifaceIndex].rx;
                totalGap.tx = totalGap.tx || interfaceGaps[ifaceIndex].tx;
            }
        }
        else if (timeDelta > 0.0)
        {
            // Only compute if counters increased (handle overflow/restart)
            if (counters.netRxBytes >= m_PrevCounters.netRxBytes)
            {
                snap.netRxBytesPerSec =
                    saneRate(Numeric::counterRate(counters.netRxBytes, m_PrevCounters.netRxBytes, timeDelta), totalGap.rx);
            }
            if (counters.netTxBytes >= m_PrevCounters.netTxBytes)
            {
                snap.netTxBytesPerSec =
                    saneRate(Numeric::counterRate(counters.netTxBytes, m_PrevCounters.netTxBytes, timeDelta), totalGap.tx);
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
        // A rate dropped as a glitch is a gap (NaN), not the 0 the snapshot shows (#1291).
        constexpr float NO_READING = std::numeric_limits<float>::quiet_NaN();
        const auto historyRate = [](double rate, bool gap)
        {
            return gap ? NO_READING : static_cast<float>(rate);
        };
        m_NetRxHistory.push(historyRate(snap.netRxBytesPerSec, totalGap.rx));
        m_NetTxHistory.push(historyRate(snap.netTxBytesPerSec, totalGap.tx));

        // Per-interface network history. New interfaces are backfilled (clamped to ring
        // capacity) so they align with m_Timestamps, and known interfaces absent from this
        // sample get a placeholder, so every series stays index-aligned. Both are NaN, not 0:
        // nothing was measured, and a chart must show a gap there rather than a false zero (#1015).
        // snap.networkInterfaces mirrors counters.networkInterfaces one for one, so the sorted
        // index built above answers "present this sample?" in O(log n), without allocating (#1415).
        auto ifacePresent = [this, &counters](const std::string& name) -> bool
        {
            return findInterface(counters.networkInterfaces, m_InterfaceIndex, name) != nullptr;
        };
        for (std::size_t ifaceIndex = 0; ifaceIndex < snap.networkInterfaces.size(); ++ifaceIndex)
        {
            const auto& ifaceSnap = snap.networkInterfaces[ifaceIndex];
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
                        it->second.push(std::numeric_limits<float>::quiet_NaN());
                    }
                }
                return it->second;
            };
            ensureAligned(m_PerInterfaceRxHistory, name).push(historyRate(ifaceSnap.rxBytesPerSec, interfaceGaps[ifaceIndex].rx));
            ensureAligned(m_PerInterfaceTxHistory, name).push(historyRate(ifaceSnap.txBytesPerSec, interfaceGaps[ifaceIndex].tx));
            m_InterfaceLastSeenSeconds[name] = nowSeconds;
        }
        // Push a NaN placeholder for known interfaces absent from this sample.
        // Iterating m_PerInterfaceRxHistory and mutating only the mapped values
        // (not inserting/erasing keys) does not invalidate the iterator, so no
        // scratch vector is needed.  m_PerInterfaceTxHistory always has the same
        // key set (both maps are always updated together), so .at() is safe.
        for (auto& [name, rxBuf] : m_PerInterfaceRxHistory)
        {
            if (!ifacePresent(name))
            {
                rxBuf.push(std::numeric_limits<float>::quiet_NaN());
                m_PerInterfaceTxHistory.at(name).push(std::numeric_limits<float>::quiet_NaN());
            }
        }

        // Prune interfaces absent for longer than the configured history window: by that point
        // their buffers hold nothing but the NaN padding just pushed above, so removing the
        // entry changes nothing observable (a fully NaN-padded buffer and a missing key both
        // present as "no recent data" via netRxHistoryForInterface()/netTxHistoryForInterface()),
        // but retaining it forever would grow these maps without bound on a machine with
        // churning interfaces (#776). Matches trimHistory()'s own wall-clock cutoff below.
        // Erases in place while iterating m_InterfaceLastSeenSeconds rather than collecting
        // stale names into a scratch vector first: that vector's own allocation could throw
        // right under the memory pressure this pruning exists to relieve, silently skipping
        // the whole pass for the one refresh cycle it matters most.
        for (auto it = m_InterfaceLastSeenSeconds.begin(); it != m_InterfaceLastSeenSeconds.end();)
        {
            if ((nowSeconds - it->second) > m_MaxHistorySeconds)
            {
                m_PerInterfaceRxHistory.erase(it->first);
                m_PerInterfaceTxHistory.erase(it->first);
                it = m_InterfaceLastSeenSeconds.erase(it);
            }
            else
            {
                ++it;
            }
        }

        m_Timestamps.push(nowSeconds);

        // Advance each core id's ring with its own reading; push NaN (a gap, not a fake 0%) for a
        // core id with no reading this sample, such as an offlined core -- interior or trailing --
        // so every core series stays aligned with m_Timestamps (#1146, #1229).
        for (std::size_t i = 0; i < m_PerCoreHistory.size(); ++i)
        {
            if (i < snap.cpuPerCore.size() && !std::isnan(snap.cpuPerCore[i].totalPercent))
            {
                m_PerCoreHistory[i].push(Numeric::clampPercentToFloat(snap.cpuPerCore[i].totalPercent));
            }
            else
            {
                m_PerCoreHistory[i].push(std::numeric_limits<float>::quiet_NaN());
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

    // counterDelta() clamps to 0 instead of wrapping if a field regresses (a transiently stale
    // counter, a probe restarting its counts, etc.) - without it, an unsigned underflow here would
    // silently pin the reported percentage at 100%. The denominator is the sum of the same guarded
    // per-field deltas (guest excluded, as in total()), so a regressed field counts as 0 in both the
    // numerators and the denominator, rather than also cancelling other fields' growth (#1157).
    const auto fieldDelta = [&current, &previous](std::uint64_t Platform::CpuCounters::* field)
    {
        return Numeric::counterDelta(current.*field, previous.*field);
    };
    const std::uint64_t totalDelta = fieldDelta(&Platform::CpuCounters::user) + fieldDelta(&Platform::CpuCounters::nice) +
                                     fieldDelta(&Platform::CpuCounters::system) + fieldDelta(&Platform::CpuCounters::idle) +
                                     fieldDelta(&Platform::CpuCounters::iowait) + fieldDelta(&Platform::CpuCounters::irq) +
                                     fieldDelta(&Platform::CpuCounters::softirq) + fieldDelta(&Platform::CpuCounters::steal);
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

    usage.userPercent =
        100.0 * (Numeric::toDouble(fieldDelta(&Platform::CpuCounters::user) + fieldDelta(&Platform::CpuCounters::nice)) / totalDeltaDouble);
    usage.systemPercent = percent(current.system, previous.system);
    usage.idlePercent = percent(current.idle, previous.idle);
    usage.iowaitPercent = percent(current.iowait, previous.iowait);
    usage.stealPercent = percent(current.steal, previous.steal);

    // Total = 100% - (idle + iowait). iowait is idle time spent waiting on I/O: shown as its own
    // breakdown band, but not busy, which also matches Windows, where that time is plain idle
    // (#1157). Built from the two rollback-guarded percentages, not from one delta of their sum: a
    // regressing iowait would otherwise cancel real idle growth and report the core 100% busy.
    usage.totalPercent = 100.0 - (usage.idlePercent + usage.iowaitPercent);

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
    // Before the no-battery return: on a battery-less machine isOnAc is still the adapter's report
    // (#1109), which consumers need.
    status.isOnAc = counters.isOnAc;

    if (!status.hasBattery)
    {
        return status;
    }

    // Basic state
    status.isCharging = (counters.state == Platform::BatteryState::Charging);
    status.isDischarging = (counters.state == Platform::BatteryState::Discharging);
    status.isFull = (counters.state == Platform::BatteryState::Full);
    status.isNotCharging = (counters.state == Platform::BatteryState::NotCharging);

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
    return findInterface(m_PrevCounters.networkInterfaces, m_PrevInterfaceIndex, name);
}

} // namespace Domain
