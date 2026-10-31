#include "UsersModel.h"

#include "Platform/AsciiCase.h"
#include "Platform/IUserSessions.h"

#include <algorithm>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace Domain
{

UsersModel::UsersModel(std::shared_ptr<Platform::IUserSessions> sessions) : m_Sessions(std::move(sessions))
{
    if (!m_Sessions)
    {
        throw std::invalid_argument("UsersModel requires a sessions source");
    }
    m_HasSessions = m_Sessions->hasSessions();
}

void UsersModel::sample()
{
    const std::scoped_lock lock(m_SampleMutex);

    Platform::UserSessionsRead read = m_Sessions->read();
    auto next = std::make_shared<UsersPublication>();
    next->status = read.status;
    next->detail = std::move(read.detail);
    next->sessions = std::move(read.sessions);
    // The tab's order, once per sample here rather than per frame: by user, then session.
    std::ranges::stable_sort(next->sessions,
                             [](const Platform::UserSession& a, const Platform::UserSession& b)
                             {
                                 if (const auto byUser = Platform::compareIgnoreCase(a.user, b.user); byUser != 0)
                                 {
                                     return byUser < 0;
                                 }
                                 return a.id < b.id;
                             });
    next->version = ++m_LastVersion;
    m_Slot.commit(std::move(next));
}

} // namespace Domain
