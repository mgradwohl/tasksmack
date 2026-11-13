/// @file test_SessionAccountCheck.cpp
/// @brief Whose TaskSmack this is (#3671), read once on the check's worker: result() never waits and has no
/// answer while the read runs, wait() has the read's answer, and a read that throws counts as the same account.
/// A fake read: never the real token or session.

#include "App/SessionAccountCheck.h"
#include "Platform/SessionAccount.h"

#include <gtest/gtest.h>

#include <atomic>
#include <future>
#include <memory>
#include <optional>
#include <stdexcept>
#include <thread>

namespace App
{
namespace
{

/// The answer for a TaskSmack elevated as "admin".
[[nodiscard]] Platform::SessionAccount admin()
{
    return {.otherAccount = true, .account = "admin"};
}

TEST(SessionAccountCheckTest, AKnownAnswerIsThereAtOnce)
{
    const std::shared_ptr<SessionAccountCheck> check = SessionAccountCheck::known(admin());
    EXPECT_EQ(check->result(), std::optional<Platform::SessionAccount>(admin()));
    EXPECT_EQ(check->wait(), admin());
}

TEST(SessionAccountCheckTest, TheReadRunsOnceOnTheWorkerAndResultNeverWaits)
{
    std::promise<void> release;
    std::shared_future<void> released = release.get_future().share();
    std::atomic<int> reads{0};
    std::atomic<std::thread::id> readOn{};
    SessionAccountCheck check(
        [&]
        {
            ++reads;
            readOn = std::this_thread::get_id();
            released.wait();
            return admin();
        });
    EXPECT_EQ(check.result(), std::nullopt) << "no answer while the read runs";
    release.set_value();
    EXPECT_EQ(check.wait(), admin());
    EXPECT_EQ(check.result(), std::optional<Platform::SessionAccount>(admin()));
    EXPECT_EQ(check.wait(), admin()) << "asked again, the answer stands";
    EXPECT_EQ(reads.load(), 1);
    EXPECT_NE(readOn.load(), std::this_thread::get_id());
}

TEST(SessionAccountCheckTest, TheSameAccountIsAnAnswerToo)
{
    SessionAccountCheck check([] { return Platform::SessionAccount{}; });
    EXPECT_EQ(check.wait(), Platform::SessionAccount{});
    EXPECT_EQ(check.result(), std::optional<Platform::SessionAccount>(Platform::SessionAccount{}));
}

TEST(SessionAccountCheckTest, AReadThatThrowsCountsAsTheSameAccount)
{
    SessionAccountCheck check([] -> Platform::SessionAccount { throw std::runtime_error("no token"); });
    EXPECT_EQ(check.wait(), Platform::SessionAccount{});
    EXPECT_EQ(check.result(), std::optional<Platform::SessionAccount>(Platform::SessionAccount{}));
}

} // namespace
} // namespace App
