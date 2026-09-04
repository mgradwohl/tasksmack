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
#include <vector>

namespace Platform
{

PDHGPUProbe::PDHGPUProbe() : m_Impl(std::make_unique<Impl>())
{
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
        // Return cached results if available
        if (m_Impl->lastValidTimestamp.time_since_epoch().count() > 0)
        {
            const auto ageMs =
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - m_Impl->lastValidTimestamp)
                    .count();
            spdlog::debug("PDHGPUProbe: Returning cached results (stale {} ms)", ageMs);
        }
        return m_Impl->lastValidResults;
    }

    // Collect query data BEFORE checking warm-up to avoid race with refreshCounters()
    // This ensures we always have fresh data when warmedUp is checked
    PDH_STATUS status = m_Impl->pdhCollectQueryData(m_Impl->query);
    if (status != ERROR_SUCCESS)
    {
        spdlog::debug("PDHGPUProbe: PdhCollectQueryData failed: 0x{:x}", static_cast<unsigned>(status));
        // Return cached results on failure
        if (m_Impl->lastValidTimestamp.time_since_epoch().count() > 0)
        {
            const auto ageMs =
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - m_Impl->lastValidTimestamp)
                    .count();
            spdlog::debug("PDHGPUProbe: Returning cached results (stale {} ms)", ageMs);
        }
        return m_Impl->lastValidResults;
    }

    // Handle warm-up: the first collected sample cannot produce utilization values
    // because PDH needs two samples to compute deltas
    if (!m_Impl->warmedUp)
    {
        m_Impl->warmedUp = true;
        spdlog::debug("PDHGPUProbe: Warm-up sample collected, returning cached results");
        // Return cached results during warm-up to avoid UI gaps
        m_Impl->lastValidTimestamp = std::chrono::steady_clock::now();
        return m_Impl->lastValidResults;
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
        double totalUtilization = 0.0;
        std::uint64_t dedicatedMemory = 0;
        std::uint64_t sharedMemory = 0;
        std::vector<std::string> engines;
    };
    std::unordered_map<AggKey, AggData, AggKeyHash> aggregated;

    // Read counter values from the three wildcard counter arrays.
    // The scratch buffer is reused across reads, so each array must be fully
    // processed before the next read.
    DWORD itemCount = 0;

    if (auto* items = m_Impl->readCounterArray(m_Impl->utilizationCounter, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, itemCount))
    {
        for (const auto& item : std::span{items, itemCount})
        {
            if (item.FmtValue.CStatus != ERROR_SUCCESS && item.FmtValue.CStatus != PDH_CSTATUS_NEW_DATA)
            {
                continue;
            }
            const auto& inst = m_Impl->instanceFor(item.szName);
            if (!inst.valid || inst.pid <= 0)
            {
                continue;
            }

            auto& agg = aggregated[AggKey{.pid = inst.pid, .gpuLuid = inst.gpuLuid}];
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-union-access) - PDH_FMT_DOUBLE selects doubleValue
            agg.totalUtilization += item.FmtValue.doubleValue;

            // Add engine type if not already present
            if (!inst.engineType.empty() && std::ranges::find(agg.engines, inst.engineType) == agg.engines.end())
            {
                agg.engines.push_back(inst.engineType);
            }
        }
    }

    const auto accumulateMemory = [&](PDH_HCOUNTER counter, auto memberSelector)
    {
        auto* items = m_Impl->readCounterArray(counter, PDH_FMT_LARGE, itemCount);
        if (items == nullptr)
        {
            return;
        }
        for (const auto& item : std::span{items, itemCount})
        {
            if (item.FmtValue.CStatus != ERROR_SUCCESS && item.FmtValue.CStatus != PDH_CSTATUS_NEW_DATA)
            {
                continue;
            }
            const auto& inst = m_Impl->instanceFor(item.szName);
            if (!inst.valid || inst.pid <= 0)
            {
                continue;
            }
            auto& agg = aggregated[AggKey{.pid = inst.pid, .gpuLuid = inst.gpuLuid}];
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-union-access) - PDH_FMT_LARGE selects largeValue
            memberSelector(agg) += static_cast<std::uint64_t>(item.FmtValue.largeValue);
        }
    };

    accumulateMemory(m_Impl->dedicatedMemoryCounter, [](AggData& agg) -> std::uint64_t& { return agg.dedicatedMemory; });
    accumulateMemory(m_Impl->sharedMemoryCounter, [](AggData& agg) -> std::uint64_t& { return agg.sharedMemory; });

    // Convert to ProcessGPUCounters
    for (const auto& [key, agg] : aggregated)
    {
        // Include processes with either GPU utilization or GPU memory usage
        if (agg.totalUtilization <= 0.0 && agg.dedicatedMemory == 0 && agg.sharedMemory == 0)
        {
            continue; // Skip processes with no GPU activity
        }

        ProcessGPUCounters counter;
        counter.pid = key.pid;
        // gpuLuid already contains format like "0x00000000_0x0000F78E" from instance name.
        // Prepend "GPU_" to match DXGI's luidId format ("GPU_0x00000000_0x0000F78E").
        // ProcessModel will match this against the gpuIdToName map (which includes both gpuId and luidId).
        counter.gpuId = "GPU_" + key.gpuLuid;
        counter.gpuUtilPercent = agg.totalUtilization;
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
