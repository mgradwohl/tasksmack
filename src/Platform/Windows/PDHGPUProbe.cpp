#include "Platform/Windows/PDHGPUProbe.h"

#include "Platform/GPUTypes.h"
#include "Platform/Windows/PDHGPUProbeImpl.h"

#include <spdlog/spdlog.h>

#include <memory>
#include <utility>

// Windows headers
// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>  // PDH_MORE_DATA, PDH_CSTATUS_NEW_DATA, etc.
// clang-format on

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <ranges>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Platform
{

using PDHGPUProbeImplDetail::adapterUtilizationFromEngines;
using PDHGPUProbeImplDetail::addEngineUtilization;
using PDHGPUProbeImplDetail::busiestEngineUtilization;

PDHGPUProbe::PDHGPUProbe(Role role) : m_Impl(std::make_unique<Impl>())
{
    m_Impl->role = role;
    m_Impl->initialize();
}

PDHGPUProbe::PDHGPUProbe(std::unique_ptr<Impl> impl) : m_Impl(std::move(impl))
{}

// Impl's own destructor calls shutdown(); the unique_ptr<Impl> member destructor below
// (implicit) is the single teardown point, so no explicit shutdown() call is needed here.
PDHGPUProbe::~PDHGPUProbe() = default;

PDHGPUProbe::PDHGPUProbe(PDHGPUProbe&&) noexcept = default;
PDHGPUProbe& PDHGPUProbe::operator=(PDHGPUProbe&&) noexcept = default;

bool PDHGPUProbe::isAvailable() const
{
    return m_Impl && m_Impl->initialized;
}

std::vector<ProcessGPUCounters> PDHGPUProbe::readProcessGPUCounters()
{
    if (!m_Impl || !m_Impl->initialized)
    {
        return {};
    }

    // Retry any counters that failed to add previously; bail out only if none
    // are active even after the retry.
    if (!m_Impl->ensureCounters())
    {
        // Nothing was read, so the adapter figures are unread: a gap, not the last reading repeated
        // as if fresh (#1166).
        m_Impl->clearAdapterReadings();
        // Cached results stand in only while recent; after that, no data rather than a frozen
        // repeat of an old reading (#1034).
        spdlog::debug("PDHGPUProbe: No counters active; returning recent cached results if any");
        return m_Impl->freshCachedResults();
    }

    // Collect query data BEFORE checking warm-up to avoid race with refreshCounters()
    // This ensures we always have fresh data when warmedUp is checked
    PDH_STATUS status = m_Impl->pdhCollectQueryData(m_Impl->query);
    if (status != ERROR_SUCCESS)
    {
        spdlog::debug("PDHGPUProbe: PdhCollectQueryData failed: 0x{:x}", static_cast<unsigned>(status));
        // The adapter figures are unread on every failed collect, however recent the per-process
        // cache: the previous reading published again would look fresh (#1166).
        m_Impl->clearAdapterReadings();
        // Cached results stand in only while recent; after that, no data rather than a frozen
        // repeat of an old reading (#1034).
        return m_Impl->freshCachedResults();
    }

    DWORD itemCount = 0;
    if (m_Impl->role == Role::Adapter)
    {
        readAdapterMemory();
    }

    // Handle warm-up: the first collected sample cannot produce utilization values
    // because PDH needs two samples to compute deltas
    if (!m_Impl->warmedUp)
    {
        m_Impl->warmedUp = true;
        // No rates yet, so no adapter utilization: a gap, not 0% or a reading from before a
        // counter re-add (#1166).
        m_Impl->lastAdapterUtilization.clear();
        m_Impl->adapterUtilizationCurrent = false;
        spdlog::debug("PDHGPUProbe: Warm-up sample collected, returning recent cached results if any");
        // Cached results stand in during warm-up only while recent. Refreshing their timestamp here
        // made results from before a counter re-add count as fresh for another staleness window.
        return m_Impl->freshCachedResults();
    }

    if (m_Impl->role == Role::Adapter)
    {
        // The adapter query needs only the per-engine totals, not the per-process aggregation the
        // process query builds (#1175).
        readAdapterUtilization();
        return {};
    }

    std::vector<ProcessGPUCounters> result;

    // Aggregate data per PID per GPU
    // Key: (pid, gpuLuid) -> aggregated data
    struct AggKey
    {
        std::int32_t pid;
        std::string gpuLuid;

        bool operator==(const AggKey& other) const
        {
            return pid == other.pid && gpuLuid == other.gpuLuid;
        }
    };
    struct AggKeyHash
    {
        std::size_t operator()(const AggKey& k) const
        {
            // Efficient hash combining that avoids intermediate hashes
            // Hash the string and combine with PID hash using bit mixing
            constexpr std::size_t PRIME = 0x9e3779b9;
            std::size_t h1 = std::hash<std::int32_t>{}(k.pid);
            const std::size_t h2 = std::hash<std::string>{}(k.gpuLuid);
            // Mix h1 and h2 using seed-xor pattern for better distribution
            h1 ^= h2 + PRIME + (h1 << 6) + (h1 >> 2);
            return h1;
        }
    };
    struct AggData
    {
        // Utilization per engine of this process (engineKey -> percent). A process's GPU % is
        // its busiest engine, as Task Manager reports it: engines run in parallel, so their sum
        // is not a utilization and overstated mixed loads (decode + 3D + copy) (#1033). A
        // process touches a handful of engines, so a linear scan beats hashing here.
        std::vector<std::pair<std::string, double>> utilizationByEngine;
        std::uint64_t dedicatedMemory = 0;
        std::uint64_t sharedMemory = 0;
        std::vector<std::string> engines;
    };
    std::unordered_map<AggKey, AggData, AggKeyHash> aggregated;

    // Read counter values from the three wildcard counter arrays.
    // The scratch buffer is reused across reads, so each array must be fully
    // processed before the next read.
    if (auto* items = m_Impl->readCounterArray(m_Impl->utilizationCounter, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, itemCount))
    {
        const std::span itemSpan{items, itemCount};
        for (std::size_t idx = 0; idx < itemSpan.size(); ++idx)
        {
            const auto& item = itemSpan[idx];
            if (item.FmtValue.CStatus != ERROR_SUCCESS && item.FmtValue.CStatus != PDH_CSTATUS_NEW_DATA)
            {
                continue;
            }
            const auto& inst = m_Impl->instanceForAt(item.szName, idx, m_Impl->utilizationPositional);
            if (!inst.valid || inst.pid <= 0)
            {
                continue;
            }

            auto& agg = aggregated[AggKey{.pid = inst.pid, .gpuLuid = inst.gpuLuid}];
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-union-access) - PDH_FMT_DOUBLE selects doubleValue
            addEngineUtilization(agg.utilizationByEngine, inst.engineKey, item.FmtValue.doubleValue);

            // Add engine type if not already present
            if (!inst.engineType.empty() && std::ranges::find(agg.engines, inst.engineType) == agg.engines.end())
            {
                agg.engines.push_back(inst.engineType);
            }
        }
    }

    const auto accumulateMemory = [&](PDH_HCOUNTER counter, auto& posCache, auto memberSelector)
    {
        auto* items = m_Impl->readCounterArray(counter, PDH_FMT_LARGE, itemCount);
        if (items == nullptr)
        {
            return;
        }
        const std::span itemSpan{items, itemCount};
        for (std::size_t idx = 0; idx < itemSpan.size(); ++idx)
        {
            const auto& item = itemSpan[idx];
            if (item.FmtValue.CStatus != ERROR_SUCCESS && item.FmtValue.CStatus != PDH_CSTATUS_NEW_DATA)
            {
                continue;
            }
            const auto& inst = m_Impl->instanceForAt(item.szName, idx, posCache);
            if (!inst.valid || inst.pid <= 0)
            {
                continue;
            }
            auto& agg = aggregated[AggKey{.pid = inst.pid, .gpuLuid = inst.gpuLuid}];
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-union-access) - PDH_FMT_LARGE selects largeValue
            memberSelector(agg) += static_cast<std::uint64_t>(item.FmtValue.largeValue);
        }
    };

    accumulateMemory(m_Impl->dedicatedMemoryCounter,
                     m_Impl->dedicatedMemoryPositional,
                     [](AggData& agg) -> std::uint64_t& { return agg.dedicatedMemory; });
    accumulateMemory(
        m_Impl->sharedMemoryCounter, m_Impl->sharedMemoryPositional, [](AggData& agg) -> std::uint64_t& { return agg.sharedMemory; });

    // Convert to ProcessGPUCounters
    for (const auto& [key, agg] : aggregated)
    {
        const double busiestEngine = busiestEngineUtilization(agg.utilizationByEngine);
        // Include processes with either GPU utilization or GPU memory usage
        if (busiestEngine <= 0.0 && agg.dedicatedMemory == 0 && agg.sharedMemory == 0)
        {
            continue; // Skip processes with no GPU activity
        }

        ProcessGPUCounters counter;
        counter.pid = key.pid;
        // gpuLuid already contains format like "0x00000000_0x0000F78E" from instance name.
        // Prepend "GPU_" to match DXGI's luidId format ("GPU_0x00000000_0x0000F78E").
        // ProcessModel will match this against the gpuIdToName map (which includes both gpuId and luidId).
        counter.gpuId = "GPU_" + key.gpuLuid;
        counter.gpuUtilPercent = busiestEngine;
        counter.gpuMemoryBytes = agg.dedicatedMemory + agg.sharedMemory;
        counter.activeEngines = agg.engines;

        result.push_back(std::move(counter));
    }

    if (!result.empty())
    {
        spdlog::debug("PDHGPUProbe: Got {} per-process GPU entries (util + memory)", result.size());
        // Cache valid results for use during warm-up periods and for staleness checks
        m_Impl->lastValidResults = result;
        m_Impl->lastValidTimestamp = std::chrono::steady_clock::now();
    }

    return result;
}

void PDHGPUProbe::readAdapterMemory()
{
    // Adapter-wide memory in use (#1029). These are gauges, not rates, so they are read on the
    // warm-up collect too -- otherwise the first GPU refresh published every non-NVML adapter with
    // 0 bytes in use. One instance per adapter and physical node, so names are parsed directly
    // rather than through the per-process instance caches.
    DWORD itemCount = 0;
    std::unordered_map<std::string, AdapterMemoryUsage> adapterMemory;
    // Every good item counts, 0 bytes included, and marks its segment read for the adapter: an
    // idle adapter's real 0 B used to be dropped and so showed as N/A, indistinguishable from a
    // segment whose counter array failed (#1246).
    const auto accumulateAdapterMemory =
        [&](PDH_HCOUNTER counter, std::uint64_t AdapterMemoryUsage::* bytesMember, bool AdapterMemoryUsage::* readMember)
    {
        auto* items = m_Impl->readCounterArray(counter, PDH_FMT_LARGE, itemCount);
        if (items == nullptr)
        {
            return; // This segment is unread for every adapter
        }
        for (const auto& item : std::span{items, itemCount})
        {
            if (item.FmtValue.CStatus != ERROR_SUCCESS && item.FmtValue.CStatus != PDH_CSTATUS_NEW_DATA)
            {
                continue;
            }
            const std::string luid =
                PDHGPUProbeImplDetail::parseAdapterInstanceLuid(PDHGPUProbeImplDetail::wideToUtf8Fallback(std::wstring(item.szName)));
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-union-access) - PDH_FMT_LARGE selects largeValue
            const LONGLONG bytes = item.FmtValue.largeValue;
            if (luid.empty() || bytes < 0)
            {
                continue;
            }
            auto& usage = adapterMemory["GPU_" + luid];
            usage.*bytesMember += static_cast<std::uint64_t>(bytes);
            usage.*readMember = true;
        }
    };
    accumulateAdapterMemory(m_Impl->adapterDedicatedCounter, &AdapterMemoryUsage::dedicatedBytes, &AdapterMemoryUsage::dedicatedRead);
    accumulateAdapterMemory(m_Impl->adapterSharedCounter, &AdapterMemoryUsage::sharedBytes, &AdapterMemoryUsage::sharedRead);
    m_Impl->lastAdapterMemory = std::move(adapterMemory);
}

void PDHGPUProbe::readAdapterUtilization()
{
    // For each engine of each adapter the sum over processes, then the busiest engine
    // (gpuLuid -> engineKey -> percent) -- Task Manager's definition (#1033).
    std::unordered_map<std::string, std::unordered_map<std::string, double>> adapterEngines;
    std::unordered_set<std::string> unreadLuids; // Adapters with engine items, none of them readable
    DWORD itemCount = 0;
    auto* items = m_Impl->readCounterArray(m_Impl->utilizationCounter, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, itemCount);
    if (!m_Impl->lastArrayReadOk)
    {
        // The array itself was not read: nothing is known about any adapter -- unread, a gap, not an
        // idle 0% (#1166).
        m_Impl->lastAdapterUtilization.clear();
        m_Impl->adapterUtilizationUnread.clear();
        m_Impl->adapterUtilizationCurrent = false;
        return;
    }
    if (items != nullptr)
    {
        const std::span itemSpan{items, itemCount};
        for (std::size_t idx = 0; idx < itemSpan.size(); ++idx)
        {
            const auto& item = itemSpan[idx];
            const auto& inst = m_Impl->instanceForAt(item.szName, idx, m_Impl->utilizationPositional);
            if (!inst.valid || inst.pid <= 0)
            {
                continue;
            }
            if (item.FmtValue.CStatus != ERROR_SUCCESS && item.FmtValue.CStatus != PDH_CSTATUS_NEW_DATA)
            {
                unreadLuids.insert("GPU_" + inst.gpuLuid);
                continue;
            }
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-union-access) - PDH_FMT_DOUBLE selects doubleValue
            adapterEngines[inst.gpuLuid][inst.engineKey] += item.FmtValue.doubleValue;
        }
    }

    m_Impl->lastAdapterUtilization.clear();
    for (const auto& [gpuLuid, engines] : adapterEngines)
    {
        m_Impl->lastAdapterUtilization["GPU_" + gpuLuid] = adapterUtilizationFromEngines(engines);
        unreadLuids.erase("GPU_" + gpuLuid); // Some of its engines were read: it has a reading
    }
    m_Impl->adapterUtilizationUnread = std::move(unreadLuids);
    m_Impl->adapterUtilizationCurrent = true;
}

std::unordered_map<std::string, double> PDHGPUProbe::adapterUtilization() const
{
    return m_Impl ? m_Impl->lastAdapterUtilization : std::unordered_map<std::string, double>{};
}

std::unordered_set<std::string> PDHGPUProbe::adapterUtilizationUnread() const
{
    return m_Impl ? m_Impl->adapterUtilizationUnread : std::unordered_set<std::string>{};
}

bool PDHGPUProbe::adapterUtilizationCurrent() const
{
    return m_Impl && m_Impl->adapterUtilizationCurrent;
}

std::unordered_map<std::string, AdapterMemoryUsage> PDHGPUProbe::adapterMemory() const
{
    return m_Impl ? m_Impl->lastAdapterMemory : std::unordered_map<std::string, AdapterMemoryUsage>{};
}

PDHGPUProbe::CacheStats PDHGPUProbe::instanceCacheStats() const
{
    if (!m_Impl)
    {
        return {};
    }
    return {.hits = m_Impl->instanceCacheHits,
            .misses = m_Impl->instanceCacheMisses,
            .clears = m_Impl->instanceCacheClears,
            .cachedEntries = m_Impl->instanceCache.size(),
            .positionalHits = m_Impl->positionalHits,
            .positionalMisses = m_Impl->positionalMisses};
}

GPUCapabilities PDHGPUProbe::capabilities() const
{
    GPUCapabilities caps{};

    if (m_Impl && m_Impl->initialized)
    {
        caps.hasPerProcessMetrics = true;
        caps.hasEngineUtilization = true;
        caps.supportsMultiGPU = true;
    }

    // PDH provides utilization but not system-level GPU metrics
    caps.hasTemperature = false;
    caps.hasPowerMetrics = false;
    caps.hasClockSpeeds = false;
    caps.hasFanSpeed = false;

    return caps;
}

} // namespace Platform
