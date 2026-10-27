#include "ServiceModel.h"

#include "Platform/AsciiCase.h"
#include "Platform/IServiceProbe.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace Domain
{

ServicesByPid indexServicesByPid(std::span<const Platform::ServiceInfo> services)
{
    ServicesByPid byPid;
    std::size_t index = 0;
    while (index < services.size())
    {
        if (const std::uint32_t pid = services[index].pid; pid != 0)
        {
            byPid[pid].push_back(index);
        }
        ++index;
    }
    return byPid;
}

std::span<const std::size_t> servicesHostedBy(const ServicePublication& publication, std::uint32_t pid) noexcept
{
    const auto found = publication.servicesByPid.find(pid);
    return found != publication.servicesByPid.end() ? std::span<const std::size_t>(found->second) : std::span<const std::size_t>{};
}

const Platform::ServiceInfo* findService(const ServicePublication& publication, std::string_view name) noexcept
{
    const auto found = std::ranges::find(publication.services, name, &Platform::ServiceInfo::name);
    return found != publication.services.end() ? &*found : nullptr;
}

ServiceModel::ServiceModel(std::unique_ptr<Platform::IServiceProbe> probe, NowFunction now)
    : m_Probe(std::move(probe)), m_Now(std::move(now))
{
    if (!m_Probe)
    {
        throw std::invalid_argument("ServiceModel requires a probe");
    }
    m_Capabilities = m_Probe->capabilities();
}

void ServiceModel::sample()
{
    const std::scoped_lock lock(m_SampleMutex);

    if (m_ConfigRereadRequested.exchange(false, std::memory_order_acq_rel))
    {
        m_Probe->forgetCachedConfig();
    }
    auto next = std::make_shared<ServicePublication>();
    const Clock::time_point captureTime = m_Now();
    Platform::ServiceEnumeration result = m_Probe->enumerate();
    if (!result.ok)
    {
        // Never an empty list passed off as current: the last good one, flagged with why, and still
        // stamped with when it was read.
        const std::shared_ptr<const ServicePublication> previous = m_Slot.load();
        next->services = previous->services;
        next->servicesByPid = previous->servicesByPid;
        next->captureTime = previous->captureTime;
        next->stale = true;
        next->failureReason = std::move(result.failureReason);
        next->version = ++m_LastVersion;
        m_Slot.commit(std::move(next));
        return;
    }
    next->services = std::move(result.services);
    // The table's default order, done once per sample here rather than per frame in the UI. ASCII
    // letters fold, every other byte sorts as itself whatever the locale (#2566).
    const auto lessIgnoringCase = [](const Platform::ServiceInfo& a, const Platform::ServiceInfo& b)
    {
        return Platform::compareIgnoreCase(a.name, b.name) < 0;
    };
    std::ranges::sort(next->services, lessIgnoringCase);
    // The PID index too (#1880), built here on the sampler thread rather than by each reader.
    next->servicesByPid = indexServicesByPid(next->services);
    next->captureTime = captureTime;
    next->version = ++m_LastVersion;
    m_Slot.commit(std::move(next));
}

} // namespace Domain
