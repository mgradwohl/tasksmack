#pragma once

// Which services each process hosts (#1880), merged into the process snapshots by ProcessModel from the
// latest service list (Windows: the PID each service reports) and the process's own systemd unit
// (Linux: ProcessCounters::services, from its cgroup, #1886). Pure: no locking, no probes, so the PID-reuse
// and staleness rules are tested on their own.

#include "Domain/ProcessSnapshot.h"
#include "Domain/SamplingConfig.h"
#include "Domain/ServiceModel.h"
#include "Platform/ProcessTypes.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Domain::ProcessServices
{

/// Between the names in ProcessSnapshot::services.
inline constexpr std::string_view SEPARATOR = ", ";

/// Whether @p publication says which services the processes listed at @p now host: it has been read,
/// and no longer ago than Sampling::SERVICES_DATA_MAX_AGE_MS. An older one is no longer being refreshed
/// (nothing needs it) and is left out rather than passed off as current. A capture time after @p now
/// (the two read from different clocks in a test) counts as current.
[[nodiscard]] inline bool isCurrent(const ServicePublication& publication, std::chrono::steady_clock::time_point now) noexcept
{
    return publication.version != 0 && (now - publication.captureTime) <= std::chrono::milliseconds(Sampling::SERVICES_DATA_MAX_AGE_MS);
}

/// The services a process hosts, by name: its own systemd unit (@p serviceUnit, Linux; empty
/// elsewhere) and those @p publication gives its PID, without repeats. @p listedAtCapture says
/// whether the process -- its PID and start time -- was already listed when the service list was
/// read: one first listed since may have reused the PID of a process the list saw, and its entry is
/// not this process's. @p publication may be null (no current list).
[[nodiscard]] inline std::vector<std::string>
hostedServiceNames(std::string_view serviceUnit, const ServicePublication* publication, std::int32_t pid, bool listedAtCapture)
{
    std::vector<std::string> names;
    if (!serviceUnit.empty())
    {
        names.emplace_back(serviceUnit);
    }
    if (publication == nullptr || !listedAtCapture || pid <= 0)
    {
        return names;
    }
    const std::span<const std::size_t> hosted = servicesHostedBy(*publication, static_cast<std::uint32_t>(pid));
    std::size_t position = 0;
    while (position < hosted.size())
    {
        const std::string& name = publication->services[hosted[position]].name;
        if (std::ranges::find(names, name) == names.end())
        {
            names.push_back(name);
        }
        ++position;
    }
    return names;
}

/// @p names joined with SEPARATOR: the Services column's text.
[[nodiscard]] inline std::string joinNames(std::span<const std::string> names)
{
    std::string joined;
    std::size_t position = 0;
    while (position < names.size())
    {
        if (position != 0)
        {
            joined += SEPARATOR;
        }
        joined += names[position];
        ++position;
    }
    return joined;
}

/// Fills every snapshot's serviceNames and services (their joined text). @p snapshots, @p counters (the probe's, for the
/// Linux unit) and @p firstSeen (when the model first listed each process) are parallel. @p publication
/// is the current service list, or null for none (isCurrent()).
inline void mergeInto(std::span<ProcessSnapshot> snapshots,
                      std::span<const Platform::ProcessCounters> counters,
                      std::span<const std::chrono::steady_clock::time_point> firstSeen,
                      const ServicePublication* publication)
{
    std::size_t index = 0;
    while (index < snapshots.size())
    {
        ProcessSnapshot& snapshot = snapshots[index];
        const std::string_view unit = index < counters.size() ? std::string_view(counters[index].services) : std::string_view{};
        const bool listedAtCapture = publication != nullptr && index < firstSeen.size() && firstSeen[index] <= publication->captureTime;
        snapshot.serviceNames = hostedServiceNames(unit, publication, snapshot.pid, listedAtCapture);
        snapshot.services = joinNames(snapshot.serviceNames);
        ++index;
    }
}

} // namespace Domain::ProcessServices
