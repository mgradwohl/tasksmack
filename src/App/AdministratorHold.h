#pragma once

// The Access notice's Administrator choice off the UI thread (#2728): IPrivilegedBroker::holdForSession()
// waits for the password prompt, so it runs on a Domain::Worker (#3343), ts-admin-hold, and the UI thread
// only starts it and takes its answer.
//
//  - One hold at a time: start() while one is still to be taken is refused, as a second click on
//    Administrator always was.
//  - The answer is the worker's final publication; take() hands it over once. A hold whose body threw
//    (no memory) ends the worker Failed, and take() reports that as the hold's error, so the notice never
//    waits for an answer that can't come. A failed worker is terminal: a later start() answers its reason.
//  - Cancellation: the destructor (the layer's) stops the worker, which signals the hold's stop token, so a
//    prompt still up is abandoned, and waits for it.

#include "Domain/Worker.h"
#include "Platform/IPrivilegedBroker.h"

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace App
{

class AdministratorHold
{
  public:
    using Result = std::expected<void, std::string>;

    /// start()'s error for a missing helper, or for a hold already under way (#2728).
    static constexpr std::string_view NOT_INSTALLED = "the administrator helper isn't installed";
    /// start()'s error when the hold couldn't be queued (no memory).
    static constexpr std::string_view NOT_QUEUED = "the administrator helper couldn't be started (no memory)";

    AdministratorHold();
    /// Stops the worker: a hold still waiting for its prompt is cancelled through its stop token, and waited for.
    ~AdministratorHold() = default;

    AdministratorHold(const AdministratorHold&) = delete;
    AdministratorHold& operator=(const AdministratorHold&) = delete;
    AdministratorHold(AdministratorHold&&) = delete;
    AdministratorHold& operator=(AdministratorHold&&) = delete;

    /// Has the worker hold @p broker's helper for the session. Never blocks. The error, for the notice,
    /// says why nothing was started: no helper, a hold already under way, or a worker that has failed.
    [[nodiscard]] Result start(std::shared_ptr<Platform::IPrivilegedBroker> broker);

    /// The hold's answer, once, when there is one; nullopt while it is under way, or when none was started.
    /// UI thread; never blocks.
    [[nodiscard]] std::optional<Result> take();

    /// Whether a hold was started and its answer not yet taken.
    [[nodiscard]] bool pending() const noexcept
    {
        return m_Pending;
    }

  private:
    /// One hold: the broker whose helper to hold.
    struct HoldRequest
    {
        std::shared_ptr<Platform::IPrivilegedBroker> broker;
    };
    /// What the worker publishes: holdForSession()'s answer.
    struct HoldAnswer
    {
        Result result;
    };
    using HoldWorker = Domain::Worker<HoldRequest, HoldAnswer>;

    // UI thread only
    std::uint64_t m_TakenVersion = 0; ///< The last publication take() handed over
    bool m_Pending = false;           ///< A hold was queued and its answer not yet taken

    HoldWorker m_Worker; // Last: stopped and joined first
};

} // namespace App
