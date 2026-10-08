#include "ServiceModel.h"

#include "Platform/IServiceProbe.h"

#include <algorithm>
#include <cctype>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace Domain
{

ServiceModel::ServiceModel(std::unique_ptr<Platform::IServiceProbe> probe) : m_Probe(std::move(probe))
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

    auto next = std::make_shared<ServicePublication>();
    Platform::ServiceEnumeration result = m_Probe->enumerate();
    if (!result.ok)
    {
        // Never an empty list passed off as current: the last good one, flagged with why.
        next->services = m_Slot.load()->services;
        next->stale = true;
        next->failureReason = std::move(result.failureReason);
        next->version = ++m_LastVersion;
        m_Slot.commit(std::move(next));
        return;
    }
    next->services = std::move(result.services);
    // The table's default order, done once per sample here rather than per frame in the UI.
    const auto lessIgnoringCase = [](const Platform::ServiceInfo& a, const Platform::ServiceInfo& b)
    {
        return std::ranges::lexicographical_compare(
            a.name, b.name, [](unsigned char x, unsigned char y) { return std::tolower(x) < std::tolower(y); });
    };
    std::ranges::sort(next->services, lessIgnoringCase);
    next->version = ++m_LastVersion;
    m_Slot.commit(std::move(next));
}

} // namespace Domain
