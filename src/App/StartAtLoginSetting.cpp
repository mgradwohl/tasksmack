#include "App/StartAtLoginSetting.h"

#include "App/SessionAccountCheck.h"
#include "Domain/StartAtLogin.h"
#include "Platform/IStartAtLogin.h"
#include "Platform/SessionAccount.h"
#include "Platform/ThreadName.h"
#include "version.h"

#include <spdlog/spdlog.h>

#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace App
{

namespace StartAtLogin = Domain::StartAtLogin;

namespace
{

/// On the worker, before the entry is touched (#3671): the status to end with when this TaskSmack runs as another
/// account than the session's signed-in user (@p account says), else nullopt. Waits for the check's answer.
[[nodiscard]] std::optional<StartAtLogin::Status> otherAccountStatus(SessionAccountCheck* account)
{
    if (account == nullptr)
    {
        return std::nullopt;
    }
    Platform::SessionAccount session = account->wait();
    if (!session.otherAccount)
    {
        return std::nullopt;
    }
    return StartAtLogin::otherAccount(std::move(session.account));
}

/// The entry as it stands, for this copy.
[[nodiscard]] StartAtLogin::Status readStatus(Platform::IStartAtLogin& entry)
{
    auto facts = entry.read();
    if (!facts.has_value())
    {
        return StartAtLogin::failed(std::move(facts.error()));
    }
    return StartAtLogin::classify(facts->command, facts->executable, facts->installed, facts->caseInsensitivePaths);
}

/// Turns the entry on (this copy, minimized) or off, then reads it back.
[[nodiscard]] StartAtLogin::Status change(Platform::IStartAtLogin& entry, bool enabled)
{
    if (enabled)
    {
        // This copy's path comes from the read: the entry starts the TaskSmack that wrote it.
        auto facts = entry.read();
        if (!facts.has_value())
        {
            return StartAtLogin::failed(std::move(facts.error()));
        }
        if (auto written = entry.write(StartAtLogin::runCommand(facts->executable)); !written.has_value())
        {
            return StartAtLogin::failed(std::move(written.error()));
        }
    }
    else if (auto removed = entry.remove(); !removed.has_value())
    {
        return StartAtLogin::failed(std::move(removed.error()));
    }
    return readStatus(entry);
}

/// The startup check (#3276): the uninstaller's mark taken, then the entry as it stands, written back first
/// when an install (not an uninstall) removed it. The result says whether the mark was there.
[[nodiscard]] StartAtLogin::Status
checkAtStartup(Platform::IStartAtLogin& entry, bool intended, const std::string& seenBy, const std::string& thisBuild)
{
    // Taken first and once: a later failure still reports it, so what the user turned on is forgotten.
    const std::expected<bool, std::string> mark = entry.takeUninstallMark();
    if (!mark.has_value())
    {
        spdlog::warn("Start at sign-in: {}; not writing it back this time", mark.error());
    }
    const bool uninstalled = mark.value_or(false);
    StartAtLogin::Status status = [&]
    {
        auto facts = entry.read();
        if (!facts.has_value())
        {
            return StartAtLogin::failed(std::move(facts.error()));
        }
        const StartAtLogin::StartupAction action = StartAtLogin::startupAction(
            intended, uninstalled, seenBy, thisBuild, entry.restoresMissingEntry(), facts->installed, facts->command.has_value());
        // Without the mark's answer, an uninstall can't be ruled out: nothing is written back.
        if (action != StartAtLogin::StartupAction::Restore || !mark.has_value())
        {
            return StartAtLogin::classify(facts->command, facts->executable, facts->installed, facts->caseInsensitivePaths);
        }
        spdlog::info("Start at sign-in was on, and an install since removed it: turning it back on for {}", facts->executable);
        if (auto written = entry.write(StartAtLogin::runCommand(facts->executable)); !written.has_value())
        {
            return StartAtLogin::failed(std::move(written.error()));
        }
        return readStatus(entry);
    }();
    status.uninstalled = uninstalled;
    return status;
}

} // namespace

std::string buildIdentity()
{
    std::string identity = tasksmack::Version::FULL;
    identity += '+';
    identity += tasksmack::Version::GIT_COMMIT;
    return identity;
}

StartAtLoginSetting::StartAtLoginSetting(std::shared_ptr<Platform::IStartAtLogin> entry, std::shared_ptr<SessionAccountCheck> account)
    : m_Entry(std::move(entry)), m_Account(std::move(account)), m_Worker(Platform::START_AT_LOGIN_THREAD_NAME, &StartAtLogin::failed)
{}

void StartAtLoginSetting::refresh()
{
    if (offered() && !busy())
    {
        start(std::nullopt);
    }
}

void StartAtLoginSetting::startupCheck(bool intended, std::string seenBy, std::string thisBuild)
{
    if (!offered() || busy())
    {
        return;
    }
    m_Changing = false; // a read, that may write: the line under the box is the status's
    // NOLINTBEGIN(clang-analyzer-cplusplus.NewDeleteLeaks) - the job, which BackgroundTask owns, owns its captures
    if (std::optional<StartAtLogin::Status> notStarted = m_Worker.start(
            [entry = m_Entry, account = m_Account, intended, seenBy = std::move(seenBy), thisBuild = std::move(thisBuild)]
            {
                // Another account's entry: no mark taken, nothing written back (#3671).
                if (std::optional<StartAtLogin::Status> other = otherAccountStatus(account.get()); other.has_value())
                {
                    return std::move(*other);
                }
                return checkAtStartup(*entry, intended, seenBy, thisBuild);
            });
        notStarted.has_value())
    {
        m_Status = std::move(*notStarted);
    }
    // NOLINTEND(clang-analyzer-cplusplus.NewDeleteLeaks)
}

void StartAtLoginSetting::setEnabled(bool enabled)
{
    if (!offered())
    {
        return;
    }
    if (m_Worker.busy())
    {
        m_Pending = enabled; // the last one asked for wins
        return;
    }
    start(enabled);
}

void StartAtLoginSetting::start(std::optional<bool> enabled)
{
    m_Changing = enabled.has_value();
    if (enabled.has_value())
    {
        spdlog::info("Settings: Start TaskSmack when I sign in changed to {}", *enabled);
    }
    // The job owns what it uses: the entry and the account check, shared.
    // NOLINTBEGIN(clang-analyzer-cplusplus.NewDeleteLeaks) - the job, which BackgroundTask owns, owns its captures
    if (std::optional<StartAtLogin::Status> notStarted = m_Worker.start(
            [entry = m_Entry, account = m_Account, enabled]
            {
                if (std::optional<StartAtLogin::Status> other = otherAccountStatus(account.get()); other.has_value())
                {
                    return std::move(*other); // another account's entry: not read or changed (#3671)
                }
                return enabled.has_value() ? change(*entry, *enabled) : readStatus(*entry);
            });
        notStarted.has_value())
    {
        m_Status = std::move(*notStarted); // no thread for it: a failed read
    }
    // NOLINTEND(clang-analyzer-cplusplus.NewDeleteLeaks)
}

bool StartAtLoginSetting::poll()
{
    bool changed = false;
    if (std::optional<StartAtLogin::Status> status = m_Worker.poll(); status.has_value())
    {
        if (status->state == StartAtLogin::State::Failed)
        {
            spdlog::warn("Start at sign-in: {}", status->error);
        }
        changed = (*status != m_Status);
        m_Status = std::move(*status);
    }
    if (!m_Worker.busy() && m_Pending.has_value())
    {
        start(std::exchange(m_Pending, std::nullopt));
    }
    return changed;
}

void StartAtLoginSetting::waitForTesting()
{
    for (int round = 0; round < 3; ++round)
    {
        m_Worker.wait();
        static_cast<void>(poll());
    }
}

} // namespace App
