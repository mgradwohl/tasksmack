/// @file test_UpdateChecker.cpp
/// @brief The opt-in update check's worker (#3093), with a fake HTTPS client and an injected clock, so no
/// test touches the network: nothing is sent while the setting is off or without a client, the first
/// check when it is on, at most one a day after that (a failed one counting), Check now ahead of the limit,
/// a restart that keeps the limit and the notice, and Dismiss. Also what the title bar's update button
/// shows for each of those (#3373).

#include "App/TitleBarButtons.h"
#include "App/UpdateChecker.h"
#include "Domain/SamplingConfig.h"
#include "Domain/UpdateCheck.h"
#include "Platform/IHttpsClient.h"

#include <gtest/gtest.h>

#include <chrono>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace App
{
namespace
{

/// Answers each get() with the reply it was given, and records the requests (on the worker's thread).
class FakeHttpsClient final : public Platform::IHttpsClient
{
  public:
    explicit FakeHttpsClient(std::expected<Platform::HttpsResponse, std::string> reply) : m_Reply(std::move(reply))
    {}

    [[nodiscard]] std::expected<Platform::HttpsResponse, std::string> get(const Platform::HttpsRequest& request,
                                                                          const std::stop_token& /*stop*/) override
    {
        const std::scoped_lock lock(m_Mutex);
        m_Requests.push_back(request);
        return m_Reply;
    }

    [[nodiscard]] std::vector<Platform::HttpsRequest> requests() const
    {
        const std::scoped_lock lock(m_Mutex);
        return m_Requests;
    }

    void setReply(std::expected<Platform::HttpsResponse, std::string> reply)
    {
        const std::scoped_lock lock(m_Mutex);
        m_Reply = std::move(reply);
    }

  private:
    mutable std::mutex m_Mutex;
    std::expected<Platform::HttpsResponse, std::string> m_Reply;
    std::vector<Platform::HttpsRequest> m_Requests;
};

using namespace std::chrono_literals;
using Domain::UpdateCheck::Persisted;

constexpr const char* RUNNING = "0.9.0";
constexpr const char* RELEASE_PAGE = "https://github.com/mgradwohl/tasksmack/releases/tag/v0.10.0";

Platform::HttpsResponse newerRelease()
{
    return Platform::HttpsResponse{
        .body = R"({"html_url":"https://github.com/mgradwohl/tasksmack/releases/tag/v0.10.0","tag_name":"v0.10.0",)"
                R"("draft":false,"prerelease":false,"author":{"html_url":"https://github.com/mgradwohl"}})",
        .status = 200,
    };
}

class UpdateCheckerTest : public ::testing::Test
{
  protected:
    /// A checker on the fake client and the test's clock.
    UpdateChecker makeChecker(std::shared_ptr<Platform::IHttpsClient> client, const char* version = RUNNING)
    {
        return {std::move(client), version, [this] { return m_Now; }};
    }

    /// update(), then waits for a check it started and takes that in with a second update().
    bool runUpdate(UpdateChecker& checker)
    {
        const bool changed = checker.update(m_State);
        checker.waitForCheck();
        return checker.update(m_State) || changed;
    }

    std::shared_ptr<FakeHttpsClient> m_Client = std::make_shared<FakeHttpsClient>(newerRelease());
    std::chrono::system_clock::time_point m_Now = std::chrono::system_clock::time_point(std::chrono::seconds(1'761'652'800));
    Persisted m_State;
};

TEST_F(UpdateCheckerTest, NothingIsSentWhileTheSettingIsOff)
{
    UpdateChecker checker = makeChecker(m_Client);
    ASSERT_TRUE(checker.offered());
    ASSERT_FALSE(m_State.enabled); // the default
    for (int day = 0; day < 30; ++day)
    {
        EXPECT_FALSE(runUpdate(checker));
        m_Now += 25h;
    }
    EXPECT_TRUE(m_Client->requests().empty());
    EXPECT_FALSE(checker.checking());
    EXPECT_EQ(m_State.lastCheckUnixSeconds, 0);
    EXPECT_TRUE(checker.summary().empty());
}

TEST_F(UpdateCheckerTest, WithoutAClientNothingIsOfferedOrSent)
{
    UpdateChecker checker = makeChecker(nullptr);
    EXPECT_FALSE(checker.offered());
    m_State.enabled = true;
    checker.checkNow();
    EXPECT_FALSE(checker.checking());
    EXPECT_FALSE(runUpdate(checker));
    EXPECT_EQ(m_State.lastCheckUnixSeconds, 0);
    // What another run saw is no notice here.
    m_State.latestVersion = "0.10.0";
    m_State.latestUrl = RELEASE_PAGE;
    EXPECT_FALSE(checker.notice(m_State).has_value());

    // A build version the check can't compare turns it off too.
    UpdateChecker unversioned = makeChecker(m_Client, "dev");
    EXPECT_FALSE(unversioned.offered());
    EXPECT_FALSE(runUpdate(unversioned));
    EXPECT_TRUE(m_Client->requests().empty());
}

TEST_F(UpdateCheckerTest, TheFirstCheckSendsOneRequestAndRecordsANewerRelease)
{
    UpdateChecker checker = makeChecker(m_Client);
    m_State.enabled = true;

    EXPECT_TRUE(checker.update(m_State)); // started: stamped
    EXPECT_EQ(m_State.lastCheckUnixSeconds, Domain::UpdateCheck::toUnixSeconds(m_Now));
    EXPECT_TRUE(checker.checking());
    EXPECT_EQ(checker.summary(), "Checking for updates\xE2\x80\xA6");
    checker.waitForCheck();
    EXPECT_TRUE(checker.update(m_State)); // taken in
    EXPECT_FALSE(checker.checking());

    const std::vector<Platform::HttpsRequest> requests = m_Client->requests();
    ASSERT_EQ(requests.size(), 1U);
    EXPECT_EQ(requests[0].host, "api.github.com");
    EXPECT_EQ(requests[0].path, "/repos/mgradwohl/tasksmack/releases/latest");
    EXPECT_EQ(requests[0].userAgent, "TaskSmack/0.9.0");
    EXPECT_EQ(requests[0].accept, "application/vnd.github+json");
    EXPECT_EQ(requests[0].timeout, std::chrono::milliseconds(Domain::Sampling::UPDATE_CHECK_TIMEOUT_MS));
    EXPECT_EQ(requests[0].maxBodyBytes, Domain::UpdateCheck::MAX_RESPONSE_BYTES);

    EXPECT_EQ(m_State.latestVersion, "0.10.0");
    EXPECT_EQ(m_State.latestUrl, RELEASE_PAGE);
    const std::optional<Domain::UpdateCheck::Notice> noticeRead = checker.notice(m_State);
    ASSERT_TRUE(noticeRead.has_value());
    const Domain::UpdateCheck::Notice notice = noticeRead.value_or(Domain::UpdateCheck::Notice{});
    EXPECT_EQ(notice.version, "0.10.0");
    EXPECT_EQ(notice.url, RELEASE_PAGE);
    EXPECT_EQ(checker.summary(), "TaskSmack v0.10.0 is available");
}

TEST_F(UpdateCheckerTest, AtMostOneCheckADay)
{
    UpdateChecker checker = makeChecker(m_Client);
    m_State.enabled = true;
    EXPECT_TRUE(runUpdate(checker));
    ASSERT_EQ(m_Client->requests().size(), 1U);

    // Frame after frame, an hour apart, for a day less a second: nothing more.
    const auto first = m_Now;
    while (m_Now + 1h < first + 24h)
    {
        m_Now += 1h;
        EXPECT_FALSE(runUpdate(checker));
    }
    m_Now = first + 24h - 1s;
    EXPECT_FALSE(runUpdate(checker));
    EXPECT_EQ(m_Client->requests().size(), 1U);

    m_Now = first + 24h;
    EXPECT_TRUE(runUpdate(checker));
    EXPECT_EQ(m_Client->requests().size(), 2U);
}

TEST_F(UpdateCheckerTest, AFailedCheckIsSilentAndCountsForTheDailyLimit)
{
    m_Client->setReply(std::unexpected(std::string("WinHttpSendRequest: the server's name couldn't be resolved (12007)")));
    UpdateChecker checker = makeChecker(m_Client);
    m_State.enabled = true;
    m_State.latestVersion = "0.9.5"; // what an earlier check saw stays
    EXPECT_TRUE(runUpdate(checker));
    EXPECT_EQ(m_State.latestVersion, "0.9.5");
    EXPECT_EQ(m_State.lastCheckUnixSeconds, Domain::UpdateCheck::toUnixSeconds(m_Now));
    EXPECT_EQ(checker.summary(), "Couldn't reach GitHub to check for updates");

    m_Now += 12h;
    EXPECT_FALSE(runUpdate(checker));
    EXPECT_EQ(m_Client->requests().size(), 1U); // no retry loop

    // A rate-limited answer is a failure too.
    m_Client->setReply(Platform::HttpsResponse{.body = R"({"message":"API rate limit exceeded"})", .status = 403});
    m_Now += 12h;
    EXPECT_TRUE(runUpdate(checker));
    EXPECT_EQ(m_Client->requests().size(), 2U);
    EXPECT_EQ(m_State.latestVersion, "0.9.5");
}

TEST_F(UpdateCheckerTest, UpToDateRecordsTheLatestReleaseAndOffersNothing)
{
    UpdateChecker checker = makeChecker(m_Client, "0.10.0");
    m_State.enabled = true;
    EXPECT_TRUE(runUpdate(checker));
    EXPECT_EQ(m_State.latestVersion, "0.10.0");
    EXPECT_FALSE(checker.notice(m_State).has_value());
    EXPECT_EQ(checker.summary(), "Up to date: the latest release is v0.10.0");
}

TEST_F(UpdateCheckerTest, ADevBuildSendsItsFullVersionAndIsNotOfferedAnOlderRelease)
{
    // A build from main between releases (#3371): tasksmack::Version::FULL reads "0.11.0-dev".
    UpdateChecker checker = makeChecker(m_Client, "0.11.0-dev");
    ASSERT_TRUE(checker.offered());
    m_State.enabled = true;
    EXPECT_TRUE(runUpdate(checker));
    const std::vector<Platform::HttpsRequest> requests = m_Client->requests();
    ASSERT_EQ(requests.size(), 1U);
    EXPECT_EQ(requests[0].userAgent, "TaskSmack/0.11.0-dev");
    EXPECT_EQ(m_State.latestVersion, "0.10.0");
    EXPECT_FALSE(checker.notice(m_State).has_value());
}

TEST_F(UpdateCheckerTest, CheckNowGoesAheadOfTheDailyLimit)
{
    UpdateChecker checker = makeChecker(m_Client);
    m_State.enabled = true;
    EXPECT_TRUE(runUpdate(checker));
    m_Now += 1h;
    checker.checkNow();
    EXPECT_TRUE(checker.checking()); // Settings' Check now is disabled from now
    EXPECT_TRUE(runUpdate(checker));
    EXPECT_EQ(m_Client->requests().size(), 2U);
    EXPECT_EQ(m_State.lastCheckUnixSeconds, Domain::UpdateCheck::toUnixSeconds(m_Now));
}

TEST_F(UpdateCheckerTest, ARestartWithinADayDoesntCheckButStillShowsTheNotice)
{
    m_State.enabled = true;
    {
        UpdateChecker before = makeChecker(m_Client);
        EXPECT_TRUE(runUpdate(before));
    }
    // What config.toml kept, read by a new run an hour later.
    m_Now += 1h;
    UpdateChecker after = makeChecker(m_Client);
    EXPECT_FALSE(runUpdate(after));
    EXPECT_EQ(m_Client->requests().size(), 1U);
    EXPECT_TRUE(after.notice(m_State).has_value());
    EXPECT_TRUE(after.summary().empty()); // nothing checked this session
}

TEST_F(UpdateCheckerTest, TurningTheSettingOffHidesTheNotice)
{
    UpdateChecker checker = makeChecker(m_Client);
    m_State.enabled = true;
    EXPECT_TRUE(runUpdate(checker));
    ASSERT_TRUE(checker.notice(m_State).has_value());
    m_State.enabled = false;
    EXPECT_FALSE(checker.notice(m_State).has_value());
    m_Now += 48h;
    EXPECT_FALSE(runUpdate(checker));
    EXPECT_EQ(m_Client->requests().size(), 1U);
}

TEST_F(UpdateCheckerTest, DismissHidesThatVersionOnly)
{
    UpdateChecker checker = makeChecker(m_Client);
    m_State.enabled = true;
    EXPECT_TRUE(runUpdate(checker));
    ASSERT_TRUE(checker.notice(m_State).has_value());

    EXPECT_TRUE(UpdateChecker::dismiss(m_State));
    EXPECT_EQ(m_State.dismissedVersion, "0.10.0");
    EXPECT_FALSE(checker.notice(m_State).has_value());
    EXPECT_FALSE(UpdateChecker::dismiss(m_State)); // nothing more to change

    // Tomorrow's check finds a newer one: offered again.
    m_Client->setReply(Platform::HttpsResponse{.body = R"({"tag_name":"v0.11.0"})", .status = 200});
    m_Now += 24h;
    EXPECT_TRUE(runUpdate(checker));
    const std::optional<Domain::UpdateCheck::Notice> noticeRead = checker.notice(m_State);
    ASSERT_TRUE(noticeRead.has_value());
    const Domain::UpdateCheck::Notice notice = noticeRead.value_or(Domain::UpdateCheck::Notice{});
    EXPECT_EQ(notice.version, "0.11.0");
    EXPECT_EQ(notice.url, Domain::UpdateCheck::LATEST_RELEASE_PAGE);

    Persisted none;
    EXPECT_FALSE(UpdateChecker::dismiss(none)); // no release seen: nothing to dismiss
}

TEST_F(UpdateCheckerTest, ACheckStillRunningWhenTheCheckerGoesIsWaitedFor)
{
    m_State.enabled = true;
    {
        UpdateChecker checker = makeChecker(m_Client);
        EXPECT_TRUE(checker.update(m_State));
    } // destroyed with the request in flight: stopped and waited for, not left running
    EXPECT_EQ(m_Client->requests().size(), 1U);
}

// ========== The title bar's update button (#3373) ==========

TEST_F(UpdateCheckerTest, TheUpdateButtonIsHiddenWhereTheCheckIsntOffered)
{
    m_State.enabled = true;
    m_State.latestVersion = "0.10.0"; // what another run saw shows nothing here either
    m_State.latestUrl = RELEASE_PAGE;
    EXPECT_EQ(makeChecker(nullptr).titleBarButton(m_State), TitleBarButtons::UpdateButton{});
    EXPECT_FALSE(makeChecker(m_Client, "dev").titleBarButton(m_State).shown);
}

TEST_F(UpdateCheckerTest, TheUpdateButtonIsInactiveAndSaysSoWhileTheSettingIsOff)
{
    const UpdateChecker checker = makeChecker(m_Client);
    m_State.latestVersion = "0.10.0"; // seen before the setting was turned off: not offered now
    m_State.latestUrl = RELEASE_PAGE;
    const TitleBarButtons::UpdateButton button = checker.titleBarButton(m_State);
    EXPECT_TRUE(button.shown);
    EXPECT_FALSE(button.active);
    EXPECT_EQ(button.tooltip, TitleBarButtons::UPDATE_OFF_TOOLTIP);
    EXPECT_TRUE(button.url.empty());
    EXPECT_TRUE(m_Client->requests().empty()); // asking sends nothing
}

TEST_F(UpdateCheckerTest, TheLaunchCheckLightsTheUpdateButtonWithTheNewVersion)
{
    UpdateChecker checker = makeChecker(m_Client);
    m_State.enabled = true;
    // Before the first update(): nothing seen yet.
    EXPECT_EQ(checker.titleBarButton(m_State).tooltip, TitleBarButtons::UPDATE_UNKNOWN_TOOLTIP);

    // The first update() starts the check, as TaskSmack starts; until it ends the button says so.
    ASSERT_TRUE(checker.update(m_State));
    TitleBarButtons::UpdateButton button = checker.titleBarButton(m_State);
    EXPECT_FALSE(button.active);
    EXPECT_EQ(button.tooltip, TitleBarButtons::UPDATE_CHECKING_TOOLTIP);

    checker.waitForCheck();
    ASSERT_TRUE(checker.update(m_State));
    button = checker.titleBarButton(m_State);
    EXPECT_TRUE(button.shown);
    EXPECT_TRUE(button.active);
    EXPECT_EQ(button.tooltip, "TaskSmack v0.10.0 is available");
    EXPECT_EQ(button.url, RELEASE_PAGE);
    EXPECT_EQ(m_Client->requests().size(), 1U);
}

TEST_F(UpdateCheckerTest, DismissingTheNoticeLeavesTheUpdateButtonLit)
{
    const UpdateChecker checker = makeChecker(m_Client);
    m_State.enabled = true;
    m_State.latestVersion = "0.10.0";
    m_State.latestUrl = RELEASE_PAGE;
    ASSERT_TRUE(UpdateChecker::dismiss(m_State));
    ASSERT_FALSE(checker.notice(m_State).has_value());
    const TitleBarButtons::UpdateButton button = checker.titleBarButton(m_State);
    EXPECT_TRUE(button.active);
    EXPECT_EQ(button.tooltip, "TaskSmack v0.10.0 is available");
    EXPECT_EQ(button.url, RELEASE_PAGE);
}

TEST_F(UpdateCheckerTest, TheUpdateButtonIsInactiveAndUpToDateOnTheLatestRelease)
{
    m_Client->setReply(newerRelease());
    UpdateChecker checker = makeChecker(m_Client, "0.10.0");
    m_State.enabled = true;
    ASSERT_TRUE(runUpdate(checker));
    const TitleBarButtons::UpdateButton button = checker.titleBarButton(m_State);
    EXPECT_TRUE(button.shown);
    EXPECT_FALSE(button.active);
    EXPECT_EQ(button.tooltip, TitleBarButtons::UPDATE_UP_TO_DATE_TOOLTIP);
    EXPECT_TRUE(button.url.empty());

    // A dev build of the next version is up to date against the last release too.
    const UpdateChecker dev = makeChecker(m_Client, "0.11.0-dev");
    EXPECT_EQ(dev.titleBarButton(m_State).tooltip, TitleBarButtons::UPDATE_UP_TO_DATE_TOOLTIP);
}

TEST_F(UpdateCheckerTest, AFailedCheckLeavesTheUpdateButtonOnWhatTheLastOneSaw)
{
    m_Client->setReply(std::unexpected(std::string("offline")));
    UpdateChecker checker = makeChecker(m_Client);
    m_State.enabled = true;
    ASSERT_TRUE(runUpdate(checker));
    // No check has ever seen a release: the button doesn't claim TaskSmack is up to date.
    TitleBarButtons::UpdateButton button = checker.titleBarButton(m_State);
    EXPECT_FALSE(button.active);
    EXPECT_EQ(button.tooltip, TitleBarButtons::UPDATE_UNKNOWN_TOOLTIP);

    // One that did, a day before: the failure changes nothing it shows.
    m_State.latestVersion = "0.10.0";
    m_State.latestUrl = RELEASE_PAGE;
    button = checker.titleBarButton(m_State);
    EXPECT_TRUE(button.active);
    EXPECT_EQ(button.tooltip, "TaskSmack v0.10.0 is available");
}

TEST_F(UpdateCheckerTest, ARestartWithinADayLightsTheUpdateButtonWithoutAnotherCheck)
{
    m_State.enabled = true;
    {
        UpdateChecker first = makeChecker(m_Client);
        ASSERT_TRUE(runUpdate(first));
    }
    m_Now += 2h;
    UpdateChecker after = makeChecker(m_Client);
    EXPECT_FALSE(runUpdate(after)); // not due
    const TitleBarButtons::UpdateButton button = after.titleBarButton(m_State);
    EXPECT_TRUE(button.active);
    EXPECT_EQ(button.tooltip, "TaskSmack v0.10.0 is available");
    EXPECT_EQ(m_Client->requests().size(), 1U);
}

} // namespace
} // namespace App
