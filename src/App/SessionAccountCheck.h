#pragma once

// Whether this TaskSmack runs elevated as another account than the session's signed-in user (#3671), read
// once at startup on a worker of its own, and shared by what it changes: Settings' Start TaskSmack when I
// sign in (StartAtLoginSetting) and the Startup tab's per-user entries (StartupActionsView). In such a copy
// both leave the user's sign-in startup alone: TaskSmack never acts as another user.
//
// The UI thread asks result(), which never waits. A worker about to write asks wait(), which does, so a
// change asked for before the answer came still follows it. The read is Platform::readSessionAccount()
// (a test's function instead); one that fails, or doesn't answer within WAIT_LIMIT, is the same account, as
// the read itself answers when it can't tell.
//
// Free of ImGui; tested in tests/App/test_SessionAccountCheck.cpp.

#include "Domain/Worker.h"
#include "Platform/SessionAccount.h"

#include <chrono>
#include <functional>
#include <memory>
#include <optional>

namespace App
{

class SessionAccountCheck
{
    /// known()'s passkey: only it makes a check with its answer given.
    struct KnownTag
    {};

  public:
    using ReadFn = std::function<Platform::SessionAccount()>;

    /// How long wait() waits for the read before it counts the copy as the same account.
    static constexpr std::chrono::seconds WAIT_LIMIT{30};

    /// Starts @p read on the check's worker (Platform::SESSION_ACCOUNT_THREAD_NAME).
    explicit SessionAccountCheck(ReadFn read);
    /// known()'s: @p account is the answer.
    SessionAccountCheck(KnownTag tag, Platform::SessionAccount account);

    /// A check whose answer is @p account already: no worker, nothing read (tests).
    [[nodiscard]] static std::shared_ptr<SessionAccountCheck> known(Platform::SessionAccount account);

    ~SessionAccountCheck() = default;
    SessionAccountCheck(const SessionAccountCheck&) = delete;
    SessionAccountCheck& operator=(const SessionAccountCheck&) = delete;
    SessionAccountCheck(SessionAccountCheck&&) = delete;
    SessionAccountCheck& operator=(SessionAccountCheck&&) = delete;

    /// The answer, once the read has ended; nullopt while it runs. Never waits: the UI thread's.
    [[nodiscard]] std::optional<Platform::SessionAccount> result() const;

    /// The answer, waiting for the read up to WAIT_LIMIT: a worker's, before it changes anything per-user.
    /// Never on the UI thread.
    [[nodiscard]] Platform::SessionAccount wait();

  private:
    /// The worker's one job: the read. It carries nothing.
    struct Read
    {};
    using ReadWorker = Domain::Worker<Read, std::optional<Platform::SessionAccount>>;

    std::optional<Platform::SessionAccount> m_Known; ///< known(): the answer, with no worker
    std::unique_ptr<ReadWorker> m_Worker;            ///< Null for known()
};

} // namespace App
