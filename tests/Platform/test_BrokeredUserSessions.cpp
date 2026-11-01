/// @file test_BrokeredUserSessions.cpp
/// @brief The Users tab's session actions with the administrator helper behind them (#3076): a Disconnect or
/// Sign out refused for want of an administrator goes to a running helper; Authenticate and retry starts the
/// helper (its UAC prompt) and acts through it; without a helper the refusal says how to get the rights, and
/// isn't offered the retry. Everything else is the inner sessions'.

#include "Mocks/FakeBroker.h"
#include "Platform/BrokerProtocol.h"
#include "Platform/BrokeredUserSessions.h"
#include "Platform/IServiceActions.h"
#include "Platform/IUserSessions.h"

#include <gtest/gtest.h>

#include <expected>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace Platform
{
namespace
{

/// TaskSmack's own sessions: session "1" is its own and acts; any other is refused for an administrator.
class RecordingSessions final : public IUserSessions
{
  public:
    std::vector<std::string> calls;

    [[nodiscard]] bool hasSessions() const override
    {
        return true;
    }
    [[nodiscard]] UserSessionsRead read() override
    {
        UserSession mine;
        mine.id = "1";
        mine.user = "alice";
        return {.status = UserSessionsStatus::Ok, .sessions = {mine}, .detail = {}};
    }
    [[nodiscard]] bool canAct(SessionAction action) const override
    {
        return action != SessionAction::Lock;
    }
    [[nodiscard]] ServiceActionResult
    act(std::string_view id, SessionAction /*action*/, bool allowInteractive, const std::stop_token& /*stopToken*/) override
    {
        calls.push_back(std::string(id) + (allowInteractive ? " interactive" : ""));
        if (id == "1")
        {
            return ServiceActionResult::succeeded();
        }
        ServiceActionResult refused = ServiceActionResult::failed("Requires administrator");
        refused.needsAuthorization = true;
        return refused;
    }
};

struct Fixture
{
    explicit Fixture(bool connected = true, bool withBroker = true)
        : broker(withBroker ? std::make_shared<Mocks::FakeBroker>(connected) : nullptr)
    {
        auto owned = std::make_unique<RecordingSessions>();
        inner = owned.get();
        sessions = std::make_unique<BrokeredUserSessions>(std::move(owned), broker);
        if (broker != nullptr)
        {
            broker->setAnswer([](const Broker::Request& /*request*/) -> std::expected<Broker::Response, std::string>
                              { return Broker::ServiceActionReply{.result = ServiceActionResult::succeeded()}; });
        }
    }

    [[nodiscard]] std::vector<Broker::SessionActionRequest> sent() const
    {
        std::vector<Broker::SessionActionRequest> requests;
        for (const Broker::Request& request : broker->requests())
        {
            if (const auto* session = std::get_if<Broker::SessionActionRequest>(&request))
            {
                requests.push_back(*session);
            }
        }
        return requests;
    }

    std::shared_ptr<Mocks::FakeBroker> broker;
    RecordingSessions* inner = nullptr;
    std::unique_ptr<BrokeredUserSessions> sessions;
};

TEST(BrokeredUserSessionsTest, ReadsAndCapabilitiesAreTheInnerSessions)
{
    const Fixture fixture;
    EXPECT_TRUE(fixture.sessions->hasSessions());
    EXPECT_EQ(fixture.sessions->read().sessions.size(), 1U);
    EXPECT_TRUE(fixture.sessions->canAct(SessionAction::Disconnect));
    EXPECT_FALSE(fixture.sessions->canAct(SessionAction::Lock));
}

TEST(BrokeredUserSessionsTest, OnesOwnSessionNeverReachesTheHelper)
{
    const Fixture fixture;
    EXPECT_TRUE(fixture.sessions->act("1", SessionAction::Disconnect, false, {}).ok);
    EXPECT_TRUE(fixture.broker->requests().empty());
    EXPECT_EQ(fixture.inner->calls, (std::vector<std::string>{"1"}));
}

TEST(BrokeredUserSessionsTest, ARunningHelperFinishesARefusalAtOnce)
{
    const Fixture fixture;
    EXPECT_TRUE(fixture.sessions->act("3", SessionAction::SignOut, false, {}).ok);
    ASSERT_EQ(fixture.sent().size(), 1U);
    EXPECT_EQ(fixture.sent()[0], (Broker::SessionActionRequest{.action = SessionAction::SignOut, .sessionId = 3}));
    EXPECT_EQ(fixture.broker->connectCount(), 0) << "no prompt: the helper was running";

    // The helper's own refusal is the answer.
    fixture.broker->setAnswer([](const Broker::Request& /*request*/) -> std::expected<Broker::Response, std::string>
                              { return Broker::ServiceActionReply{.result = ServiceActionResult::failed("Session 3 has ended")}; });
    EXPECT_EQ(fixture.sessions->act("3", SessionAction::Disconnect, false, {}).message, "Session 3 has ended");

    // A helper that doesn't do session actions leaves TaskSmack's refusal, retry and all.
    fixture.broker->setAnswer([](const Broker::Request& /*request*/) -> std::expected<Broker::Response, std::string>
                              { return Broker::ErrorReply{.message = "This helper doesn't act on sessions"}; });
    const ServiceActionResult unhelped = fixture.sessions->act("3", SessionAction::SignOut, false, {});
    EXPECT_FALSE(unhelped.ok);
    EXPECT_TRUE(unhelped.needsAuthorization);

    // A helper that broke since connected(): the refusal stands, saying why, with the retry still offered.
    fixture.broker->setAnswer([](const Broker::Request& /*request*/) -> std::expected<Broker::Response, std::string>
                              { return std::unexpected(std::string("the pipe broke")); });
    const ServiceActionResult broke = fixture.sessions->act("3", SessionAction::SignOut, false, {});
    EXPECT_TRUE(broke.needsAuthorization);
    EXPECT_EQ(broke.message, "Requires administrator (the administrator helper couldn't be asked: the pipe broke)");

    std::stop_source stop;
    stop.request_stop();
    EXPECT_TRUE(fixture.sessions->act("3", SessionAction::SignOut, false, stop.get_token()).cancelled);
}

TEST(BrokeredUserSessionsTest, WithoutARunningHelperTheRefusalOffersTheRetryWhichStartsIt)
{
    const Fixture fixture(false);
    const ServiceActionResult refused = fixture.sessions->act("3", SessionAction::Disconnect, false, {});
    EXPECT_FALSE(refused.ok);
    EXPECT_TRUE(refused.needsAuthorization);
    EXPECT_TRUE(fixture.sent().empty());

    // Authenticate and retry: the helper starts (its UAC prompt), then acts.
    EXPECT_TRUE(fixture.sessions->act("3", SessionAction::Disconnect, true, {}).ok);
    EXPECT_EQ(fixture.broker->connectCount(), 1);
    ASSERT_EQ(fixture.sent().size(), 1U);
    EXPECT_EQ(fixture.sent()[0].action, SessionAction::Disconnect);
    EXPECT_EQ(fixture.inner->calls, (std::vector<std::string>{"3"})) << "the retry goes straight to the helper";

    // Running now, a second retry doesn't prompt again.
    EXPECT_TRUE(fixture.sessions->act("4", SessionAction::SignOut, true, {}).ok);
    EXPECT_EQ(fixture.broker->connectCount(), 1);
}

TEST(BrokeredUserSessionsTest, ARetryThatCantReachTheHelperSaysWhy)
{
    const Fixture fixture(false);
    fixture.broker->setConnectFailure("The UAC prompt was dismissed");
    const ServiceActionResult dismissed = fixture.sessions->act("3", SessionAction::SignOut, true, {});
    EXPECT_FALSE(dismissed.ok);
    EXPECT_TRUE(dismissed.authorizationUnavailable);
    EXPECT_EQ(dismissed.message, "Couldn't act as an administrator: The UAC prompt was dismissed");
    std::stop_source stop;
    stop.request_stop();
    EXPECT_TRUE(fixture.sessions->act("3", SessionAction::SignOut, true, stop.get_token()).cancelled);

    const Fixture running;
    running.broker->setAnswer([](const Broker::Request& /*request*/) -> std::expected<Broker::Response, std::string>
                              { return std::unexpected(std::string("timed out")); });
    EXPECT_EQ(running.sessions->act("3", SessionAction::SignOut, true, {}).message, "Couldn't ask the administrator helper: timed out");
    EXPECT_TRUE(running.sessions->act("3", SessionAction::SignOut, true, stop.get_token()).cancelled);
    running.broker->setAnswer([](const Broker::Request& /*request*/) -> std::expected<Broker::Response, std::string>
                              { return Broker::ErrorReply{.message = "no"}; });
    EXPECT_EQ(running.sessions->act("3", SessionAction::SignOut, true, {}).message, "The administrator helper doesn't act on sessions");
    EXPECT_EQ(running.sessions->act("c1", SessionAction::SignOut, true, {}).message, "Not a session: c1");
    EXPECT_EQ(running.sessions->act("c1", SessionAction::SignOut, false, {}).message, "Not a session: c1");
}

TEST(BrokeredUserSessionsTest, WithoutAHelperARefusalSaysHowToGetTheRights)
{
    const Fixture fixture(false, false);
    const ServiceActionResult refused = fixture.sessions->act("3", SessionAction::SignOut, false, {});
    EXPECT_FALSE(refused.ok);
    EXPECT_FALSE(refused.needsAuthorization) << "nothing could finish it, so no retry is offered";
    EXPECT_EQ(refused.message, std::string("Requires administrator") + std::string(BrokeredUserSessions::NO_HELPER));
    EXPECT_FALSE(fixture.sessions->act("3", SessionAction::SignOut, true, {}).needsAuthorization);
    EXPECT_TRUE(fixture.sessions->act("1", SessionAction::SignOut, false, {}).ok);
}

} // namespace
} // namespace Platform
