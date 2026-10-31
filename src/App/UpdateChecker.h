#pragma once

// The opt-in update check, run (#3093): the decisions are Domain::UpdateCheck's, the request
// Platform::IHttpsClient's; this owns the worker the request runs on and what this session saw, and is
// driven from the UI thread. ShellLayer calls update() each frame with the [updates] settings: it takes in
// a finished check and starts the daily one when it is due, never on the UI thread itself (#2731). The
// result comes back through the worker's future, an Outcome the UI thread then owns, and is written into
// the settings, which the caller saves in the background.
//
// Nothing is sent while "Check for updates daily" is off: update() starts a check only when
// Domain::UpdateCheck::isCheckDue() says so, which is never while it is off, or for checkNow(), which
// Settings asks for only while its box is ticked. Without an HTTPS client (Linux, for now; a synthetic or
// test-hook run) nothing is offered or sent at all.
//
// Free of ImGui; tested in tests/App/test_UpdateChecker.cpp with a fake client and clock.

#include "App/Panels/BackgroundTask.h"
#include "Domain/UpdateCheck.h"
#include "Platform/IHttpsClient.h"

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace App
{

class UpdateChecker
{
  public:
    /// The wall clock the daily limit is measured on.
    using Clock = std::function<std::chrono::system_clock::time_point()>;

    /// @param client          The HTTPS client (Platform::makeHttpsClient()), or null: nothing is offered or sent.
    /// @param currentVersion  The running build's version, tasksmack::Version::STRING ("0.10.0").
    /// @param clock           The wall clock; std::chrono::system_clock::now when empty (tests pass their own).
    UpdateChecker(std::shared_ptr<Platform::IHttpsClient> client, std::string_view currentVersion, Clock clock = {});

    /// Whether the check is offered: there is a client, and the running version is one it can compare.
    [[nodiscard]] bool offered() const noexcept
    {
        return m_Client != nullptr && m_Current.has_value();
    }

    /// Whether a check is running, or Check now has asked for one the next update() starts.
    [[nodiscard]] bool checking() const noexcept
    {
        return m_Worker.busy() || m_CheckNowRequested;
    }

    /// On the UI thread, each frame: takes in a finished check, then starts one when it is due, or when
    /// checkNow() asked for one. A check that starts stamps @p state's last check (so a failed one counts
    /// for the daily limit); one that ends records the latest release it saw, or logs why it failed at debug.
    /// @return Whether @p state changed, for the caller to save (UserConfig::saveInBackground()).
    [[nodiscard]] bool update(Domain::UpdateCheck::Persisted& state);

    /// Settings' Check now: the next update() starts a check, whatever the daily limit. Ignored while one
    /// runs, or when the check isn't offered.
    void checkNow() noexcept;

    /// What the notice offers for @p state: Domain::UpdateCheck::noticeFor(), against the running version;
    /// nothing where the check isn't offered.
    [[nodiscard]] std::optional<Domain::UpdateCheck::Notice> notice(const Domain::UpdateCheck::Persisted& state) const;

    /// The notice's Dismiss: its version is not offered again, only a newer one. @return Whether @p state
    /// changed.
    [[nodiscard]] static bool dismiss(Domain::UpdateCheck::Persisted& state);

    /// A line about the last check this session, for Settings: "Checking...", "Up to date: ...",
    /// "TaskSmack v0.10.0 is available", "Couldn't check: ..."; empty before the first.
    [[nodiscard]] std::string summary() const;

    /// Tests: waits for a check still running; the next update() takes it in.
    void waitForCheck() noexcept
    {
        m_Worker.wait();
    }

  private:
    void start(Domain::UpdateCheck::Persisted& state, std::chrono::system_clock::time_point now);
    void takeIn(Domain::UpdateCheck::Outcome outcome, Domain::UpdateCheck::Persisted& state);

    std::shared_ptr<Platform::IHttpsClient> m_Client;
    std::optional<Domain::UpdateCheck::Version> m_Current;
    std::string m_CurrentText;
    Clock m_Clock;
    std::optional<Domain::UpdateCheck::Outcome> m_LastOutcome; ///< This session's last check, taken in
    bool m_CheckNowRequested = false;
    // Last: destroyed first, stopping and waiting for a request still running, whose job owns all it uses.
    BackgroundTask<Domain::UpdateCheck::Outcome> m_Worker;
};

} // namespace App
