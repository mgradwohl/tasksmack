#include "StartupModel.h"

#include "Platform/IStartupProbe.h"

#include <algorithm>
#include <cctype>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace Domain
{

StartupModel::StartupModel(std::unique_ptr<Platform::IStartupProbe> probe) : m_Probe(std::move(probe))
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
    const auto lessIgnoringCase = [](const Platform::StartupEntry& a, const Platform::StartupEntry& b)
    {
        return std::ranges::lexicographical_compare(
            a.name, b.name, [](unsigned char x, unsigned char y) { return std::tolower(x) < std::tolower(y); });
    };
    std::ranges::stable_sort(next->entries, lessIgnoringCase);
    next->version = ++m_LastVersion;
    m_Slot.commit(std::move(next));
}

} // namespace Domain
