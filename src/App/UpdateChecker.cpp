#include "App/UpdateChecker.h"

#include "Domain/SamplingConfig.h"
#include "Domain/UpdateCheck.h"
#include "Platform/IHttpsClient.h"
#include "Platform/ThreadName.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <format>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>

namespace App
{

namespace UpdateCheck = Domain::UpdateCheck;

UpdateChecker::UpdateChecker(std::shared_ptr<Platform::IHttpsClient> client, std::string_view currentVersion, Clock clock)
    : m_Client(std::move(client)),
      m_Current(UpdateCheck::parseVersion(currentVersion)),
      m_CurrentText(currentVersion),
      m_Clock(std::move(clock)),
      m_Worker(Platform::UPDATE_CHECK_THREAD_NAME, &UpdateCheck::failed)
{
    if (!m_Clock)
    {
        m_Clock = []
        {
            return std::chrono::system_clock::now();
        };
    }
    if (m_Client != nullptr && !m_Current.has_value())
    {
        spdlog::warn("Update check: this build's version, {}, isn't one it can compare; the check is off", currentVersion);
    }
}

bool UpdateChecker::update(UpdateCheck::Persisted& state)
{
    bool changed = false;
    if (std::optional<UpdateCheck::Outcome> outcome = m_Worker.poll(); outcome.has_value())
    {
        takeIn(std::move(*outcome), state);
        changed = true;
    }
    if (!offered() || m_Worker.busy())
    {
        m_CheckNowRequested = false;
        return changed;
    }
    const std::chrono::system_clock::time_point now = m_Clock();
    if (std::exchange(m_CheckNowRequested, false) ||
        UpdateCheck::isCheckDue(state.enabled, UpdateCheck::fromUnixSeconds(state.lastCheckUnixSeconds), now))
    {
        start(state, now);
        changed = true;
    }
    return changed;
}

void UpdateChecker::checkNow() noexcept
{
    if (offered() && !m_Worker.busy())
    {
        m_CheckNowRequested = true;
    }
}

void UpdateChecker::start(UpdateCheck::Persisted& state, std::chrono::system_clock::time_point now)
{
    if (!m_Current.has_value())
    {
        return; // never: update() starts a check only when offered()
    }
    // Stamped as it starts: a check that fails, or never ends because TaskSmack closed, still counts
    // for the daily limit.
    state.lastCheckUnixSeconds = UpdateCheck::toUnixSeconds(now);
    Platform::HttpsRequest request{
        .host = std::string(UpdateCheck::API_HOST),
        .path = std::string(UpdateCheck::LATEST_RELEASE_PATH),
        .userAgent = UpdateCheck::userAgent(m_CurrentText),
        .accept = std::string(UpdateCheck::ACCEPT_HEADER),
        .timeout = std::chrono::milliseconds(Domain::Sampling::UPDATE_CHECK_TIMEOUT_MS),
        .maxBodyBytes = UpdateCheck::MAX_RESPONSE_BYTES,
    };
    spdlog::debug("Update check: GET https://{}{}", request.host, request.path);
    // The job owns what it uses: the client, the request and the version to compare against.
    // NOLINTBEGIN(clang-analyzer-cplusplus.NewDeleteLeaks) - the job, which BackgroundTask owns, owns its captures
    if (std::optional<UpdateCheck::Outcome> notStarted = m_Worker.start(
            [client = m_Client, request = std::move(request), current = *m_Current](const std::stop_token& stop)
            {
                const auto response = client->get(request, stop);
                if (!response.has_value())
                {
                    return UpdateCheck::failed(response.error());
                }
                return UpdateCheck::evaluateResponse(response->status, response->body, current);
            });
        notStarted.has_value())
    {
        takeIn(std::move(*notStarted), state); // no thread for it: a failed check
    }
    // NOLINTEND(clang-analyzer-cplusplus.NewDeleteLeaks)
}

void UpdateChecker::takeIn(UpdateCheck::Outcome outcome, UpdateCheck::Persisted& state)
{
    switch (outcome.kind)
    {
    case UpdateCheck::Outcome::Kind::Failed:
        // Silent but for the log: offline, rate-limited or an unreadable reply; the next check is tomorrow's.
        spdlog::debug("Update check failed: {}", outcome.detail);
        break;
    case UpdateCheck::Outcome::Kind::UpToDate:
        spdlog::debug("Update check: up to date (the latest release is {})", outcome.latestVersion);
        state.latestVersion = outcome.latestVersion;
        state.latestUrl = outcome.releaseUrl;
        break;
    case UpdateCheck::Outcome::Kind::Available:
        spdlog::info("Update check: TaskSmack {} is available ({})", outcome.latestVersion, outcome.releaseUrl);
        state.latestVersion = outcome.latestVersion;
        state.latestUrl = outcome.releaseUrl;
        break;
    }
    m_LastOutcome = std::move(outcome);
}

std::optional<UpdateCheck::Notice> UpdateChecker::notice(const UpdateCheck::Persisted& state) const
{
    // Not offered here (no client: Linux, a synthetic or test-hook run): no notice either, whatever an
    // earlier run wrote to config.toml.
    if (m_Client == nullptr || !m_Current.has_value())
    {
        return std::nullopt;
    }
    return UpdateCheck::noticeFor(state, *m_Current);
}

bool UpdateChecker::dismiss(UpdateCheck::Persisted& state)
{
    if (state.latestVersion.empty() || state.dismissedVersion == state.latestVersion)
    {
        return false;
    }
    state.dismissedVersion = state.latestVersion;
    return true;
}

std::string UpdateChecker::summary() const
{
    if (checking())
    {
        return "Checking for updates\xE2\x80\xA6";
    }
    if (!m_LastOutcome.has_value())
    {
        return {};
    }
    switch (m_LastOutcome->kind)
    {
    case UpdateCheck::Outcome::Kind::UpToDate:
        return std::format("Up to date: the latest release is v{}", m_LastOutcome->latestVersion);
    case UpdateCheck::Outcome::Kind::Available:
        return UpdateCheck::noticeText(m_LastOutcome->latestVersion);
    case UpdateCheck::Outcome::Kind::Failed:
        break;
    }
    return "Couldn't reach GitHub to check for updates";
}

} // namespace App
