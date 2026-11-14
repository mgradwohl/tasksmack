/// @file test_IdleWaitTimer.cpp
/// @brief The frame loop's precise idle wake (#3712): the timer's due time for a wait, and, on Windows,
/// arming and disarming the high-resolution timer and its thread-pool wait against fakes, the posted
/// wake, a missing timer, and a short real wait that ends on time with the system timer at its default.

#include "Core/IdleWaitTimer.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>

#ifdef _WIN32
#include <chrono>
#include <thread>
#include <vector>

#include <windows.h>
#endif

namespace Core
{

TEST(IdleWaitTimerDueTimeTest, AWaitIsDueItsTimeoutFromNowIn100NsUnits)
{
    EXPECT_EQ(idleWaitTimerDueTime(1), std::optional<std::int64_t>{-10'000});
    EXPECT_EQ(idleWaitTimerDueTime(50), std::optional<std::int64_t>{-500'000});
    EXPECT_EQ(idleWaitTimerDueTime(250), std::optional<std::int64_t>{-2'500'000});
}

TEST(IdleWaitTimerDueTimeTest, APollOrAnEndlessWaitNeedsNoTimer)
{
    EXPECT_EQ(idleWaitTimerDueTime(0), std::nullopt);
    EXPECT_EQ(idleWaitTimerDueTime(-1), std::nullopt);
}

#ifdef _WIN32

namespace
{

/// What the fakes saw, and what they answer.
struct FakeCalls
{
    bool failTimer = false;
    bool failWait = false;
    bool failSet = false;
    DWORD createFlags = 0;
    PTP_WAIT_CALLBACK callback = nullptr;
    // Whether the thread pool was handed a context; not kept, since a fake that held it would outlive the
    // timer it points at. The test passes the timer itself, as that context is.
    bool hadContext = false;
    std::vector<std::int64_t> dueTimes;
    std::vector<HANDLE> waitRegistrations; // each SetThreadpoolWait's handle, null to stop waiting
    int cancels = 0;
    int closedHandles = 0;
    int waitedCallbacks = 0;
    int closedWaits = 0;
    std::vector<DWORD> postedTo;
    std::vector<UINT> postedMessages;
};

FakeCalls& fake()
{
    static FakeCalls calls;
    return calls;
}

// Fake handles: addresses of two statics, never dereferenced.
HANDLE fakeTimerHandle()
{
    static int timerObject = 0;
    return static_cast<HANDLE>(&timerObject);
}
PTP_WAIT fakeWaitHandle()
{
    static int waitObject = 0;
    return reinterpret_cast<PTP_WAIT>(&waitObject); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast) - an opaque handle
}

HANDLE WINAPI fakeCreateTimer(LPSECURITY_ATTRIBUTES /*attributes*/, LPCWSTR /*name*/, DWORD flags, DWORD /*access*/)
{
    fake().createFlags = flags;
    return fake().failTimer ? nullptr : fakeTimerHandle();
}

BOOL WINAPI fakeSetTimer(HANDLE /*timer*/,
                         const LARGE_INTEGER* due,
                         LONG /*period*/,
                         PTIMERAPCROUTINE /*apc*/,
                         LPVOID /*arg*/,
                         PREASON_CONTEXT /*reason*/,
                         ULONG /*delay*/)
{
    fake().dueTimes.push_back(due->QuadPart);
    return fake().failSet ? FALSE : TRUE;
}

BOOL WINAPI fakeCancel(HANDLE /*timer*/)
{
    ++fake().cancels;
    return TRUE;
}

BOOL WINAPI fakeCloseHandle(HANDLE /*handle*/)
{
    ++fake().closedHandles;
    return TRUE;
}

PTP_WAIT WINAPI fakeCreateWait(PTP_WAIT_CALLBACK callback, PVOID context, PTP_CALLBACK_ENVIRON /*environment*/)
{
    fake().callback = callback;
    fake().hadContext = context != nullptr;
    return fake().failWait ? nullptr : fakeWaitHandle();
}

VOID WINAPI fakeSetWait(PTP_WAIT /*wait*/, HANDLE handle, PFILETIME /*timeout*/)
{
    fake().waitRegistrations.push_back(handle);
}

VOID WINAPI fakeWaitCallbacks(PTP_WAIT /*wait*/, BOOL /*cancelPending*/)
{
    ++fake().waitedCallbacks;
}

VOID WINAPI fakeCloseWait(PTP_WAIT /*wait*/)
{
    ++fake().closedWaits;
}

BOOL WINAPI fakePost(DWORD threadId, UINT message, WPARAM /*wParam*/, LPARAM /*lParam*/)
{
    fake().postedTo.push_back(threadId);
    fake().postedMessages.push_back(message);
    return TRUE;
}

[[nodiscard]] IdleWaitTimerFunctions fakeFunctions()
{
    return {
        .createWaitableTimerExW = &fakeCreateTimer,
        .setWaitableTimerEx = &fakeSetTimer,
        .cancelWaitableTimer = &fakeCancel,
        .closeHandle = &fakeCloseHandle,
        .createThreadpoolWait = &fakeCreateWait,
        .setThreadpoolWait = &fakeSetWait,
        .waitForThreadpoolWaitCallbacks = &fakeWaitCallbacks,
        .closeThreadpoolWait = &fakeCloseWait,
        .postThreadMessageW = &fakePost,
    };
}

class IdleWaitTimerTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        fake() = FakeCalls{};
    }
    void TearDown() override
    {
        fake() = FakeCalls{};
    }
};

} // namespace

TEST_F(IdleWaitTimerTest, ArmSetsAHighResolutionTimerDueAtTheTimeout)
{
    IdleWaitTimer timer(fakeFunctions());
    ASSERT_TRUE(timer.active());
    EXPECT_NE(fake().createFlags & CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, 0U);

    timer.arm(50);
    EXPECT_EQ(fake().dueTimes, (std::vector<std::int64_t>{-500'000}));
    EXPECT_EQ(fake().waitRegistrations, (std::vector<HANDLE>{fakeTimerHandle()})) << "the one-shot wait registered for this wait";
}

TEST_F(IdleWaitTimerTest, DisarmCancelsTheTimerAndStopsTheWait)
{
    IdleWaitTimer timer(fakeFunctions());
    timer.arm(50);
    timer.disarm();
    EXPECT_EQ(fake().cancels, 1);
    EXPECT_EQ(fake().waitRegistrations, (std::vector<HANDLE>{fakeTimerHandle(), nullptr}));

    timer.disarm(); // Nothing armed: nothing to cancel
    EXPECT_EQ(fake().cancels, 1);
    EXPECT_EQ(fake().waitRegistrations.size(), 2U);
}

TEST_F(IdleWaitTimerTest, EachWaitArmsAgain)
{
    IdleWaitTimer timer(fakeFunctions());
    for (int wait = 0; wait < 3; ++wait)
    {
        timer.arm(20 + wait);
        timer.disarm();
    }
    EXPECT_EQ(fake().dueTimes, (std::vector<std::int64_t>{-200'000, -210'000, -220'000}));
    EXPECT_EQ(fake().cancels, 3);
}

TEST_F(IdleWaitTimerTest, APollOrAnEndlessWaitArmsNothing)
{
    IdleWaitTimer timer(fakeFunctions());
    timer.arm(0);
    timer.arm(-1);
    timer.disarm();
    EXPECT_TRUE(fake().dueTimes.empty());
    EXPECT_TRUE(fake().waitRegistrations.empty());
    EXPECT_EQ(fake().cancels, 0);
}

TEST_F(IdleWaitTimerTest, TheSignalPostsAWakeToTheLoopThread)
{
    IdleWaitTimer timer(fakeFunctions());
    timer.arm(50);
    ASSERT_NE(fake().callback, nullptr);
    ASSERT_TRUE(fake().hadContext);
    fake().callback(nullptr, &timer, fakeWaitHandle(), WAIT_OBJECT_0); // the thread pool's call, with the timer as its context
    EXPECT_EQ(fake().postedTo, (std::vector<DWORD>{GetCurrentThreadId()})) << "the thread that built it";
    EXPECT_EQ(fake().postedMessages, (std::vector<UINT>{WM_NULL}));
}

TEST_F(IdleWaitTimerTest, AFailedSetLeavesNothingRegistered)
{
    fake().failSet = true;
    IdleWaitTimer timer(fakeFunctions());
    timer.arm(50);
    EXPECT_EQ(fake().waitRegistrations, (std::vector<HANDLE>{fakeTimerHandle(), nullptr}));
    timer.disarm();
    EXPECT_EQ(fake().cancels, 0) << "never armed";
}

TEST_F(IdleWaitTimerTest, WithoutATimerItDoesNothing)
{
    fake().failTimer = true;
    {
        IdleWaitTimer timer(fakeFunctions());
        EXPECT_FALSE(timer.active());
        timer.arm(50);
        timer.disarm();
    }
    EXPECT_TRUE(fake().dueTimes.empty());
    EXPECT_EQ(fake().closedHandles, 0);
    EXPECT_EQ(fake().closedWaits, 0);
}

TEST_F(IdleWaitTimerTest, WithoutAThreadPoolWaitTheTimerIsClosed)
{
    fake().failWait = true;
    {
        IdleWaitTimer timer(fakeFunctions());
        EXPECT_FALSE(timer.active());
        timer.arm(50);
    }
    EXPECT_TRUE(fake().dueTimes.empty());
    EXPECT_EQ(fake().closedHandles, 1);
    EXPECT_EQ(fake().closedWaits, 0);
}

TEST_F(IdleWaitTimerTest, DestructionStopsTheWaitAndWaitsForItsCallbackFirst)
{
    {
        IdleWaitTimer timer(fakeFunctions());
        timer.arm(50);
    }
    EXPECT_EQ(fake().waitRegistrations.back(), nullptr);
    EXPECT_EQ(fake().waitedCallbacks, 1);
    EXPECT_EQ(fake().closedWaits, 1);
    EXPECT_EQ(fake().closedHandles, 1);
}

// The real timer: a message wait of 2 ticks' worth ends at its timeout plus the thread pool's latency,
// not on a system timer tick, whatever resolution the machine runs at.
TEST(IdleWaitTimerRealTest, AMessageWaitEndsWhenTheTimerIsDue)
{
    // On a thread of its own: the test runner's thread owns other tests' windows, whose messages
    // would wake the wait early. A thread with no windows gets only the timer's message.
    constexpr int WAIT_MS = 23; // not a multiple of the 15.625 ms default tick
    bool active = false;
    DWORD result = WAIT_FAILED;
    std::chrono::steady_clock::duration waited{};
    std::thread waiter(
        [&]
        {
            MSG message{};
            static_cast<void>(PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE)); // makes the thread's queue
            IdleWaitTimer timer;
            active = timer.active();
            if (!active)
            {
                return;
            }
            const auto start = std::chrono::steady_clock::now();
            timer.arm(WAIT_MS);
            // As SDL waits: no handles, every message wakes it, with a timeout well past the timer's
            result = MsgWaitForMultipleObjects(0, nullptr, FALSE, WAIT_MS * 10, QS_ALLINPUT);
            waited = std::chrono::steady_clock::now() - start;
            timer.disarm();
        });
    waiter.join();
    if (!active)
    {
        GTEST_SKIP() << "no high-resolution waitable timer (Windows 10 1803 on)";
    }
    EXPECT_EQ(result, WAIT_OBJECT_0) << "woken by the posted message, not the timeout";
    EXPECT_GE(waited, std::chrono::milliseconds(WAIT_MS - 1));
    EXPECT_LT(waited, std::chrono::milliseconds(WAIT_MS * 5)) << "a loaded runner may be late, but not by the full timeout";
}

#endif // _WIN32

} // namespace Core
