#include "StartupModel.h"

#include "Domain/SamplingConfig.h"
#include "Domain/StartupImpact.h"
#include "Platform/AsciiCase.h"
#include "Platform/IStartupProbe.h"

#include <algorithm>
#include <chrono>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace Domain
{

StartupModel::StartupModel(std::unique_ptr<Platform::IStartupProbe> probe, NowFunction now)
    : m_Probe(std::move(probe)), m_Now(std::move(now))
{
    if (!m_Probe)
    {
        throw std::invalid_argument("StartupModel requires a probe");
    }
    m_Capabilities = m_Probe->capabilities();
}

void StartupModel::sample()
{
    const std::scoped_lock lock(m_SampleMutex);

    auto next = std::make_shared<StartupPublication>();
    next->entries = m_Probe->enumerate();
    // The table's default order, done once per sample here rather than per frame in the UI (#580).
    // ASCII letters fold, every other byte sorts as itself whatever the locale (#2566).
    const auto lessIgnoringCase = [](const Platform::StartupEntry& a, const Platform::StartupEntry& b)
    {
        return Platform::compareIgnoreCase(a.name, b.name) < 0;
    };
    std::ranges::stable_sort(next->entries, lessIgnoringCase);
    if (m_Capabilities.hasImpact)
    {
        // The sign-in measurement changes once per sign-in: read once, then only every so often (#3073).
        const Clock::time_point now = m_Now();
        if (!m_ImpactRead.has_value() || now - *m_ImpactRead >= std::chrono::seconds(Sampling::STARTUP_IMPACT_REFRESH_SECONDS))
        {
            m_Impact = m_Probe->readImpact();
            m_ImpactRead = now;
        }
        next->impacts.reserve(next->entries.size());
        for (const auto& entry : next->entries)
        {
            next->impacts.push_back(startupEntryImpact(entry, m_Impact));
        }
        next->impactSignInUnixSeconds = m_Impact.signInUnixSeconds;
        next->impactUnavailableReason = m_Impact.unavailableReason;
    }
    next->version = ++m_LastVersion;
    m_Slot.commit(std::move(next));
}

} // namespace Domain
