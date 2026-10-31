#include "App/StartAtLoginSetting.h"

#include "Domain/StartAtLogin.h"
#include "Platform/IStartAtLogin.h"
#include "Platform/ThreadName.h"

#include <spdlog/spdlog.h>

#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace App
{

namespace StartAtLogin = Domain::StartAtLogin;

namespace
{

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

} // namespace

StartAtLoginSetting::StartAtLoginSetting(std::shared_ptr<Platform::IStartAtLogin> entry)
    : m_Entry(std::move(entry)), m_Worker(Platform::START_AT_LOGIN_THREAD_NAME, &StartAtLogin::failed)
{}

void StartAtLoginSetting::refresh()
{
    if (offered() && !busy())
    {
        start(std::nullopt);
    }
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
    // The job owns what it uses: the entry, shared.
    // NOLINTBEGIN(clang-analyzer-cplusplus.NewDeleteLeaks) - the job, which BackgroundTask owns, owns its captures
    if (std::optional<StartAtLogin::Status> notStarted =
            m_Worker.start([entry = m_Entry, enabled] { return enabled.has_value() ? change(*entry, *enabled) : readStatus(*entry); });
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
