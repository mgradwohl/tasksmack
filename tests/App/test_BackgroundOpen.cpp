/// @file test_BackgroundOpen.cpp
/// @brief App::BackgroundOpen (#3155), the worker About's and Help's links, Settings' buttons and the
/// dump result's "Open folder" open with the system handler on, never the UI thread: a job runs on its
/// worker and start() returns at once (pending), its result is taken in once by poll() (completion),
/// a job that fails or throws is a false result (error), and a second start() while one runs is
/// refused (the double click). Every job here is a fake: no browser or file manager is started. Also
/// Platform::SingleThreadedApartment, the STA ShellExecuteW is called in on that worker.

#include "App/BackgroundOpen.h"
#include "Platform/ThreadApartment.h"

#include <gtest/gtest.h>

#if defined(_WIN32)
// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h> // CoGetApartmentType: which apartment the opener's call runs in
// clang-format on
#endif

#include <atomic>
#include <chrono>
#include <future>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <thread>

namespace App
{
namespace
{

constexpr std::string_view THREAD_NAME = "ts-test-open";

/// A job that waits until the test releases it, as a slow shell extension would, and counts its runs.
class HeldJob
{
  public:
    HeldJob() : m_Released(m_Release.get_future().share())
    {}

    ~HeldJob()
    {
        release(); // a test that failed before releasing it still tears down
    }

    HeldJob(const HeldJob&) = delete;
    HeldJob& operator=(const HeldJob&) = delete;
    HeldJob(HeldJob&&) = delete;
    HeldJob& operator=(HeldJob&&) = delete;

    /// The job: true (the handler started) once released; runs on the open's worker.
    [[nodiscard]] BackgroundOpen::Job job(bool result = true)
    {
        return [this, result]
        {
            m_Runs.fetch_add(1);
            m_RunningOn.store(std::this_thread::get_id());
            static_cast<void>(m_Released.wait_for(std::chrono::seconds(30)));
            return result;
        };
    }

    void release()
    {
        if (!m_IsReleased)
        {
            m_IsReleased = true;
            m_Release.set_value();
        }
    }

    [[nodiscard]] int runs() const
    {
        return m_Runs.load();
    }
    [[nodiscard]] std::thread::id runningOn() const
    {
        return m_RunningOn.load();
    }

  private:
    std::promise<void> m_Release;
    std::shared_future<void> m_Released;
    bool m_IsReleased = false; // the test thread's
    std::atomic<int> m_Runs{0};
    std::atomic<std::thread::id> m_RunningOn{};
};

TEST(BackgroundOpenTest, AJobRunsOnTheWorkerAndItsResultIsTakenInOnce)
{
    HeldJob held;
    BackgroundOpen open(THREAD_NAME);
    EXPECT_FALSE(open.busy());

    // Pending: start() returns while the job still waits; nothing to take in yet.
    ASSERT_TRUE(open.start(held.job()));
    EXPECT_TRUE(open.busy());
    EXPECT_FALSE(open.poll().has_value());
    EXPECT_TRUE(open.busy());

    // Completion: its result, once, and the slot is free.
    held.release();
    const std::optional<bool> result = open.poll(/*wait=*/true);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result.value_or(false));
    EXPECT_FALSE(open.busy());
    EXPECT_FALSE(open.poll(true).has_value()) << "taken in once";
    EXPECT_EQ(held.runs(), 1);
    EXPECT_NE(held.runningOn(), std::this_thread::get_id()) << "never on the calling (UI) thread";
}

TEST(BackgroundOpenTest, ASecondStartWhileOneRunsIsRefused)
{
    HeldJob held;
    BackgroundOpen open(THREAD_NAME);
    ASSERT_TRUE(open.start(held.job()));
    EXPECT_FALSE(open.start(held.job())) << "the double click's second";
    EXPECT_FALSE(open.start(held.job()));
    held.release();
    open.wait();
    EXPECT_EQ(held.runs(), 1);

    // Once it is done, start() takes it in itself and runs the next.
    ASSERT_TRUE(open.start(held.job()));
    ASSERT_TRUE(open.poll(true).has_value());
    EXPECT_EQ(held.runs(), 2);
}

TEST(BackgroundOpenTest, AFailedOpenIsAFalseResult)
{
    BackgroundOpen open(THREAD_NAME);
    ASSERT_TRUE(open.start([] { return false; })); // the opener found no handler, and logged why
    const std::optional<bool> result = open.poll(true);
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result.value_or(true));
}

TEST(BackgroundOpenTest, AJobThatThrowsIsAFalseResultNotAnException)
{
    BackgroundOpen open(THREAD_NAME);
    ASSERT_TRUE(open.start([]() -> bool { throw std::runtime_error("the shell extension broke"); }));
    std::optional<bool> result;
    EXPECT_NO_THROW(result = open.poll(true));
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result.value_or(true));

    ASSERT_TRUE(open.start([]() -> bool { throw 42; })); // NOLINT(hicpp-exception-baseclass) - not a std::exception, on purpose
    EXPECT_NO_THROW(result = open.poll(true));
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result.value_or(true));
}

TEST(BackgroundOpenTest, WaitLeavesTheResultAndTheDestructorWaitsForTheJob)
{
    std::atomic<bool> ran{false};
    {
        BackgroundOpen open(THREAD_NAME);
        ASSERT_TRUE(open.start(
            [&ran]
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                ran.store(true);
                return true;
            }));
        open.wait();
        EXPECT_TRUE(ran.load());
        EXPECT_TRUE(open.busy()) << "wait() leaves the result for poll()";
        EXPECT_TRUE(open.start([] { return true; })) << "start() takes the finished one in first";
    }
    ran.store(false);
    {
        BackgroundOpen open(THREAD_NAME);
        ASSERT_TRUE(open.start(
            [&ran]
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                ran.store(true);
                return true;
            }));
    } // the destructor waits: the job never outlives what it uses
    EXPECT_TRUE(ran.load());
}

#if defined(_WIN32)
/// The calling thread's apartment type, or -1 when it has none (COM not initialized on it).
[[nodiscard]] int currentApartment()
{
    APTTYPE type = APTTYPE_CURRENT;
    APTTYPEQUALIFIER qualifier = APTTYPEQUALIFIER_NONE;
    return SUCCEEDED(CoGetApartmentType(&type, &qualifier)) ? static_cast<int>(type) : -1;
}
#endif

// #3155: ShellExecuteW runs on the open's worker, in an STA of its own for the call, as it asks of its
// caller; one already in the MTA is left there, and the thread is left as it was afterwards.
TEST(BackgroundOpenTest, TheOpenersCallRunsInASingleThreadedApartment)
{
    std::thread(
        []
        {
#if defined(_WIN32)
            const int before = currentApartment();
            {
                const Platform::SingleThreadedApartment apartment;
                EXPECT_TRUE(apartment.entered());
                // The process's first STA is its main one (APTTYPE_MAINSTA): an STA all the same.
                const int type = currentApartment();
                EXPECT_TRUE(type == static_cast<int>(APTTYPE_STA) || type == static_cast<int>(APTTYPE_MAINSTA)) << type;
            }
            EXPECT_EQ(currentApartment(), before) << "left as it was";
            {
                const Platform::MultiThreadedApartment mta;
                ASSERT_TRUE(mta.entered());
                const Platform::SingleThreadedApartment apartment;
                EXPECT_FALSE(apartment.entered()) << "an MTA thread stays in the MTA";
                EXPECT_EQ(currentApartment(), static_cast<int>(APTTYPE_MTA));
            }
#else
            const Platform::SingleThreadedApartment apartment;
            EXPECT_FALSE(apartment.entered()); // no COM: nothing to enter
#endif
        })
        .join();
}

TEST(BackgroundOpenTest, TheDefaultWorkerHasItsOwnShortName)
{
    EXPECT_EQ(Platform::SYSTEM_OPEN_THREAD_NAME, "ts-sys-open");
    EXPECT_LE(Platform::SYSTEM_OPEN_THREAD_NAME.size(), Platform::MAX_LINUX_THREAD_NAME_LENGTH);
}

} // namespace
} // namespace App
