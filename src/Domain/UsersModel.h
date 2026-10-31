#pragma once

#include "Domain/ISamplable.h"
#include "Domain/PublicationSlot.h"
#include "Platform/IUserSessions.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace Domain
{

/// One immutable generation of the machine's signed-in sessions (#3076).
struct UsersPublication
{
    std::uint64_t version = 0; ///< 0 until the first sample.
    Platform::UserSessionsStatus status = Platform::UserSessionsStatus::Unsupported;
    std::string detail;                          ///< For Failed, what failed.
    std::vector<Platform::UserSession> sessions; ///< By user name (ignoring case), then session ID.
};

/// Samples the signed-in sessions off the UI thread (from the Users tab's BackgroundSampler) and publishes
/// each reading as an immutable UsersPublication, so the UI reads it without locking or copying.
class UsersModel : public ISamplable
{
  public:
    /// @p sessions is shared with the tab, which locks and signs out through it on its own worker; the
    /// platform's implementations are safe to call from both threads.
    explicit UsersModel(std::shared_ptr<Platform::IUserSessions> sessions);

    /// Reads the sessions and publishes them. Thread-safe; samples are serialised.
    void sample() override;

    /// The newest generation (an empty version-0 one before the first sample).
    [[nodiscard]] std::shared_ptr<const UsersPublication> publication() const noexcept
    {
        return m_Slot.load();
    }

    [[nodiscard]] std::uint64_t version() const noexcept
    {
        return m_Slot.version();
    }

    /// Whether the platform can list sessions, fixed at construction.
    [[nodiscard]] bool hasSessions() const noexcept
    {
        return m_HasSessions;
    }

  private:
    std::shared_ptr<Platform::IUserSessions> m_Sessions; // read() under m_SampleMutex only
    bool m_HasSessions = false;
    std::mutex m_SampleMutex;
    std::uint64_t m_LastVersion = 0; // guarded by m_SampleMutex
    PublicationSlot<UsersPublication> m_Slot;
};

} // namespace Domain
