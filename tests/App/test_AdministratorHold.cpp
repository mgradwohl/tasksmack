/// @file test_AdministratorHold.cpp
/// @brief The Access notice's Administrator hold on Domain::Worker (#2728, #3343), with a scripted broker, so
/// no helper or password prompt is involved: the answer comes back once, a second start while one is under way
/// finishes "isn't installed", the layer's end cancels a hold still waiting for its prompt, a hold that throws
/// is reported rather than waited for, and a start that can't be queued says so at once.

#include "App/AdministratorHold.h"
#include "Domain/AllocationFailureHook.h"
#include "Platform/BrokerProtocol.h"
#include "Platform/IPrivilegedBroker.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace App
{

// Outside the anonymous namespace: it's made with std::make_shared (.github/copilot-instructions.md, Testing).
namespace AdministratorHoldFakes
{

/// holdForSession() answers as scripted: at once, or once released (or cancelled), or by throwing.
// NOLINTNEXTLINE(misc-use-internal-linkage) - outside the anonymous namespace on purpose, as said above
class ScriptedBroker final : public Platform::IPrivilegedBroker
{
  public:
    enum class Mode : std::uint8_t
    {
        Answer,         ///< Returns m_Answer at once
        WaitForRelease, ///< Waits until release() or the stop token, as a password prompt does
        Throw,          ///< Throws std::runtime_error
    };

    explicit ScriptedBroker(Mode mode, std::expected<void, std::string> answer = {}) : m_Mode(mode), m_Answer(std::move(answer))
    {}

    [[nodiscard]] bool connected() const noexcept override
    {
        return false;
    }
    [[nodiscard]] bool lastSeenConnected() const noexcept override
    {
        return false;
    }
    [[nodiscard]] std::expected<void, std::string> connect(const std::stop_token& /*stopToken*/) override
    {
        return std::unexpected(std::string("not used"));
    }
    [[nodiscard]] std::expected<Platform::Broker::Response, std::string> call(const Platform::Broker::Request& /*request*/,
                                                                              const std::stop_token& /*stopToken*/) override
    {
        return std::unexpected(std::string("not used"));
    }

    [[nodiscard]] std::expected<void, std::string> holdForSession(const std::stop_token& stopToken) override
    {
        m_Calls.fetch_add(1);
        switch (m_Mode)
        {
        case Mode::Answer:
            return m_Answer;
        case Mode::Throw:
            throw std::runtime_error("helper exploded");
        case Mode::WaitForRelease:
        {
            std::unique_lock lock(m_Mutex);
            m_Waiting = true;
            m_Changed.notify_all();
            m_Changed.wait(lock, stopToken, [this] { return m_Released; });
            m_SawStop = stopToken.stop_requested();
            return stopToken.stop_requested() ? std::unexpected(std::string("cancelled")) : m_Answer;
        }
        }
        return m_Answer;
    }

    /// Waits until holdForSession() is waiting for its release.
    [[nodiscard]] bool waitUntilWaiting()
    {
        std::unique_lock lock(m_Mutex);
        return m_Changed.wait_for(lock, std::chrono::seconds(10), [this] { return m_Waiting; });
    }

    void release()
    {
        const std::scoped_lock lock(m_Mutex);
        m_Released = true;
        m_Changed.notify_all();
    }

    [[nodiscard]] bool sawStop() const
    {
        const std::scoped_lock lock(m_Mutex);
        return m_SawStop;
    }

    [[nodiscard]] int calls() const noexcept
    {
        return m_Calls.load();
    }

  private:
    Mode m_Mode;
    std::expected<void, std::string> m_Answer;
    std::atomic<int> m_Calls{0};
    mutable std::mutex m_Mutex;
    std::condition_variable_any m_Changed;
    bool m_Waiting = false;
    bool m_Released = false;
    bool m_SawStop = false;
};

} // namespace AdministratorHoldFakes

namespace
{

using AdministratorHoldFakes::ScriptedBroker;

constexpr auto WAIT = std::chrono::seconds(10);

constexpr std::string_view NO_ANSWER = "no answer in time";

/// take() until it has an answer, as the layer's onUpdate() does each frame; NO_ANSWER if none comes. A plain
/// Result keeps clang-tidy's optional dataflow satisfied (bugprone-unchecked-optional-access doesn't model gtest's
/// ASSERT_TRUE).
[[nodiscard]] AdministratorHold::Result takeWithin(AdministratorHold& hold)
{
    const auto deadline = std::chrono::steady_clock::now() + WAIT;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (auto answer = hold.take(); answer.has_value())
        {
            return *answer;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return std::unexpected(std::string(NO_ANSWER));
}

TEST(AdministratorHoldTest, WithoutAHelperItFinishesNotInstalledAndStartsNothing)
{
    AdministratorHold hold;
    const auto started = hold.start(nullptr);
    ASSERT_FALSE(started.has_value());
    EXPECT_EQ(started.error(), AdministratorHold::NOT_INSTALLED);
    EXPECT_FALSE(hold.pending());
    EXPECT_FALSE(hold.take().has_value());
}

TEST(AdministratorHoldTest, AHeldSessionIsAnsweredOnce)
{
    AdministratorHold hold;
    const auto broker = std::make_shared<ScriptedBroker>(ScriptedBroker::Mode::Answer);
    ASSERT_TRUE(hold.start(broker).has_value());
    EXPECT_TRUE(hold.pending());
    const auto answer = takeWithin(hold);
    EXPECT_TRUE(answer.has_value()) << answer.error_or("");
    EXPECT_FALSE(hold.pending());
    EXPECT_FALSE(hold.take().has_value()) << "an answer is handed over once";
    EXPECT_EQ(broker->calls(), 1);
}

TEST(AdministratorHoldTest, ARefusedPromptIsAnsweredWithItsReasonAndCanBeTriedAgain)
{
    AdministratorHold hold;
    const auto refused = std::make_shared<ScriptedBroker>(ScriptedBroker::Mode::Answer, std::unexpected(std::string("dismissed")));
    ASSERT_TRUE(hold.start(refused).has_value());
    const auto answer = takeWithin(hold);
    ASSERT_FALSE(answer.has_value());
    EXPECT_EQ(answer.error(), "dismissed");

    // The notice's Administrator button can be pressed again, and the next hold runs.
    const auto held = std::make_shared<ScriptedBroker>(ScriptedBroker::Mode::Answer);
    ASSERT_TRUE(hold.start(held).has_value());
    const auto again = takeWithin(hold);
    EXPECT_TRUE(again.has_value()) << again.error_or("");
    EXPECT_EQ(held->calls(), 1);
}

TEST(AdministratorHoldTest, ASecondStartWhileOneIsUnderWayFinishesNotInstalled)
{
    AdministratorHold hold;
    const auto broker = std::make_shared<ScriptedBroker>(ScriptedBroker::Mode::WaitForRelease);
    ASSERT_TRUE(hold.start(broker).has_value());
    ASSERT_TRUE(broker->waitUntilWaiting());

    const auto second = hold.start(broker);
    ASSERT_FALSE(second.has_value());
    EXPECT_EQ(second.error(), AdministratorHold::NOT_INSTALLED);
    EXPECT_FALSE(hold.take().has_value()) << "the first hold is still waiting for its prompt";

    broker->release();
    const auto answer = takeWithin(hold);
    EXPECT_TRUE(answer.has_value()) << answer.error_or("");
    EXPECT_EQ(broker->calls(), 1) << "the refused start ran nothing";
}

TEST(AdministratorHoldTest, ItsEndCancelsAHoldStillWaitingForItsPrompt)
{
    const auto broker = std::make_shared<ScriptedBroker>(ScriptedBroker::Mode::WaitForRelease);
    {
        AdministratorHold hold;
        ASSERT_TRUE(hold.start(broker).has_value());
        ASSERT_TRUE(broker->waitUntilWaiting());
    } // the layer goes: its hold's stop token is signalled, and the hold waited for
    EXPECT_TRUE(broker->sawStop());
    EXPECT_EQ(broker->calls(), 1);
}

TEST(AdministratorHoldTest, AHoldThatThrowsIsReportedNotWaitedFor)
{
    AdministratorHold hold;
    const auto broker = std::make_shared<ScriptedBroker>(ScriptedBroker::Mode::Throw);
    ASSERT_TRUE(hold.start(broker).has_value());
    const auto answer = takeWithin(hold);
    ASSERT_FALSE(answer.has_value());
    EXPECT_EQ(answer.error(), "helper exploded") << "a hold that threw still finishes the notice, with its reason";
    EXPECT_FALSE(hold.pending());

    // The worker is Failed for good, so a later start says why at once rather than queueing for nothing.
    const auto later = hold.start(std::make_shared<ScriptedBroker>(ScriptedBroker::Mode::Answer));
    ASSERT_FALSE(later.has_value());
    EXPECT_EQ(later.error(), "helper exploded");
    EXPECT_FALSE(hold.pending());
}

// A start whose job can't be queued (no memory for its stop state, #3472) says so at once, so the notice
// never waits for a hold that won't run. Skipped where the hook is compiled out (Windows, sanitizer builds:
// AllocationFailureHook.h); the Linux debug and release builds run it.
TEST(AdministratorHoldTest, AStartThatCannotBeQueuedSaysSoAtOnce)
{
#ifdef TASKSMACK_NO_ALLOCATOR_HOOK
    GTEST_SKIP() << TASKSMACK_NO_ALLOCATOR_HOOK;
#else
    AdministratorHold hold;
    const auto first = std::make_shared<ScriptedBroker>(ScriptedBroker::Mode::Answer);
    ASSERT_TRUE(hold.start(first).has_value()); // starts the worker's thread, which allocates
    ASSERT_TRUE(takeWithin(hold).has_value());

    const auto broker = std::make_shared<ScriptedBroker>(ScriptedBroker::Mode::Answer); // made before the arming
    AdministratorHold::Result started; // assigned under the arming: its own storage is made before
    bool stillPending = true;
    {
        const TestSupport::FailAllocationAfter failNext(0);
        started = hold.start(broker);
        stillPending = TestSupport::allocationFailurePending();
    }
    EXPECT_FALSE(stillPending) << "start() reached the job's allocation";
    ASSERT_FALSE(started.has_value());
    EXPECT_EQ(started.error(), AdministratorHold::NOT_QUEUED);
    EXPECT_FALSE(hold.pending());
    EXPECT_EQ(broker->calls(), 0);

    // The worker goes on: the next start runs.
    ASSERT_TRUE(hold.start(broker).has_value());
    const auto answer = takeWithin(hold);
    EXPECT_TRUE(answer.has_value()) << answer.error_or("");
#endif
}

} // namespace
} // namespace App
