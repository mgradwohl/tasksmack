#pragma once

// Per-process energy from a package energy counter such as RAPL (#1093). Lives in Domain because it
// keeps state between samples: probes read raw counters only (IProcessProbe::readPackageEnergy),
// and ProcessModel calls this under its sampling lock, so samples are applied in order.
//
// RAPL reports one package-wide energy counter. Each interval's energy is shared out by each
// process's share of the CPU time used in that interval, and credited to a per-process running
// total. ProcessModel turns that total into watts with its usual counter-rate maths. Sharing the
// lifetime counter by lifetime CPU share instead charged idle long-lived processes for current
// package power, spiked every survivor whenever a busy process exited, and zeroed everything when
// the counter wrapped.

#include "Platform/ProcessTypes.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace Domain::ProcessEnergy
{

/// Energy used between two reads of a counter that wraps back to 0 after `maxRangeUj`
/// (max_energy_range_uj; 0 when unknown). A decrease with no known range yields 0.
[[nodiscard]] constexpr std::uint64_t energyDeltaUj(std::uint64_t previousUj, std::uint64_t currentUj, std::uint64_t maxRangeUj) noexcept
{
    if (currentUj >= previousUj)
    {
        return currentUj - previousUj;
    }
    if (maxRangeUj > 0 && previousUj <= maxRangeUj)
    {
        return (maxRangeUj - previousUj) + currentUj;
    }
    return 0;
}

/// Shares package energy out to processes interval by interval. Not thread-safe; ProcessModel
/// serialises calls under its sampling lock.
class Attributor
{
  public:
    /// Credits the energy used since the previous call to `processes` by their share of the CPU
    /// time used since then, and writes each process's running total to energyMicrojoules.
    /// `systemEnergyUj` is the package counter now, or nullopt if it couldn't be read: totals are
    /// then reported unchanged, so the counters stay monotonic and read as 0 W for the interval.
    /// A process seen for the first time is credited nothing (its CPU time so far may predate the
    /// interval); processes that have exited are forgotten.
    void attribute(std::span<Platform::ProcessCounters> processes, std::optional<std::uint64_t> systemEnergyUj, std::uint64_t maxRangeUj)
    {
        const std::uint64_t intervalEnergyUj = (systemEnergyUj.has_value() && m_PreviousSystemEnergyUj.has_value())
                                                 ? energyDeltaUj(*m_PreviousSystemEnergyUj, *systemEnergyUj, maxRangeUj)
                                                 : 0;

        m_CpuDeltas.assign(processes.size(), 0);
        std::uint64_t totalCpuDelta = 0;
        for (std::size_t i = 0; i < processes.size(); ++i)
        {
            const auto it = m_State.find(keyOf(processes[i]));
            const std::uint64_t cpuTime = processes[i].userTime + processes[i].systemTime;
            if (it != m_State.end() && cpuTime >= it->second.cpuTime)
            {
                m_CpuDeltas[i] = cpuTime - it->second.cpuTime;
                totalCpuDelta += m_CpuDeltas[i];
            }
        }

        std::unordered_map<Key, State, KeyHash> next;
        next.reserve(processes.size());
        for (std::size_t i = 0; i < processes.size(); ++i)
        {
            const Key key = keyOf(processes[i]);
            const auto it = m_State.find(key);
            double accumulatedUj = (it != m_State.end()) ? it->second.accumulatedUj : 0.0;
            if (totalCpuDelta > 0 && m_CpuDeltas[i] > 0)
            {
                accumulatedUj +=
                    static_cast<double>(intervalEnergyUj) * static_cast<double>(m_CpuDeltas[i]) / static_cast<double>(totalCpuDelta);
            }
            processes[i].energyMicrojoules = static_cast<std::uint64_t>(std::floor(accumulatedUj));
            next.insert_or_assign(key, State{.cpuTime = processes[i].userTime + processes[i].systemTime, .accumulatedUj = accumulatedUj});
        }
        m_State = std::move(next);

        if (systemEnergyUj.has_value())
        {
            m_PreviousSystemEnergyUj = systemEnergyUj;
        }
    }

  private:
    // A PID alone is not an identity: start time distinguishes a reused PID.
    struct Key
    {
        std::int32_t pid = 0;
        std::uint64_t startTimeTicks = 0;
        bool operator==(const Key&) const = default;
    };

    struct KeyHash
    {
        [[nodiscard]] std::size_t operator()(const Key& key) const noexcept
        {
            return std::hash<std::uint64_t>{}((static_cast<std::uint64_t>(static_cast<std::uint32_t>(key.pid)) << 32U) ^
                                              key.startTimeTicks);
        }
    };

    struct State
    {
        std::uint64_t cpuTime = 0;
        double accumulatedUj = 0.0;
    };

    [[nodiscard]] static Key keyOf(const Platform::ProcessCounters& process) noexcept
    {
        return Key{.pid = process.pid, .startTimeTicks = process.startTimeTicks};
    }

    std::optional<std::uint64_t> m_PreviousSystemEnergyUj;
    std::unordered_map<Key, State, KeyHash> m_State;
    std::vector<std::uint64_t> m_CpuDeltas; // reused scratch
};

} // namespace Domain::ProcessEnergy
