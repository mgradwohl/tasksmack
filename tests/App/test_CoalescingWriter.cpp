/// @file test_CoalescingWriter.cpp
/// @brief App::CoalescingWriter (#2916), the fire-and-forget writer UserConfig::saveInBackground()
/// hands the settings to: the write runs on its worker, never on the caller; while one is in flight
/// later snapshots replace each other, so two quick saves cost one more write, of the last; a run of
/// failed writes (false, or a throw) is reported once; flush() and the destructor write whatever is
/// still pending before they return.

#include "App/CoalescingWriter.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace App
{
namespace
{

constexpr std::string_view COALESCING_THREAD_NAME = "ts-test-write";
constexpr auto WAIT_LIMIT = std::chrono::seconds(10); // so a broken writer fails the test, not hangs it

/// What a fake sink wrote, and on which thread.
class Written
{
  public:
    void record(int value)
    {
        const std::scoped_lock lock(m_Mutex);
        m_Values.push_back(value);
        m_Threads.push_back(std::this_thread::get_id());
    }

    [[nodiscard]] std::vector<int> values() const
    {
        const std::scoped_lock lock(m_Mutex);
        return m_Values;
    }

    [[nodiscard]] std::vector<std::thread::id> threads() const
    {
        const std::scoped_lock lock(m_Mutex);
        return m_Threads;
    }

  private:
    mutable std::mutex m_Mutex;
    std::vector<int> m_Values;
    std::vector<std::thread::id> m_Threads;
};

/// Holds the sink's first write in flight until the test opens it.
class Gate
{
  public:
    /// The sink's side: counts the write in, and waits while the gate is closed.
    void enter()
    {
        std::unique_lock lock(m_Mutex);
        ++m_Entered;
        m_Changed.notify_all();
        static_cast<void>(m_Changed.wait_for(lock, WAIT_LIMIT, [this] { return m_Open; }));
    }

    /// Whether @p count writes have entered (waiting for them, within WAIT_LIMIT).
    [[nodiscard]] bool waitEntered(int count)
    {
        std::unique_lock lock(m_Mutex);
        return m_Changed.wait_for(lock, WAIT_LIMIT, [this, count] { return m_Entered >= count; });
    }

    void open()
    {
        const std::scoped_lock lock(m_Mutex);
        m_Open = true;
        m_Changed.notify_all();
    }

  private:
    std::mutex m_Mutex;
    std::condition_variable m_Changed;
    int m_Entered = 0;
    bool m_Open = false;
};

TEST(CoalescingWriterTest, TheWriteRunsOnTheWorkerNotTheCaller)
{
    Written written;
    int failures = 0;
    CoalescingWriter<int> writer(
        COALESCING_THREAD_NAME,
        [&written](const int& value)
        {
            written.record(value);
            return true;
        },
        [&failures] { ++failures; });

    writer.submit(1);
    writer.flush();

    EXPECT_EQ(written.values(), std::vector<int>{1});
    ASSERT_EQ(written.threads().size(), 1U);
    EXPECT_NE(written.threads().front(), std::this_thread::get_id());
    EXPECT_FALSE(writer.busy());
    EXPECT_EQ(failures, 0);
}

TEST(CoalescingWriterTest, TwoQuickSavesWhileAWriteIsInFlightProduceOneWriteOfTheLast)
{
    Gate gate;
    Written written;
    CoalescingWriter<int> writer(
        COALESCING_THREAD_NAME,
        [&gate, &written](const int& value)
        {
            gate.enter();
            written.record(value);
            return true;
        },
        [] {});

    writer.submit(1);
    const bool firstInFlight = gate.waitEntered(1);
    // Neither waits for the write in flight: each replaces the snapshot not written yet.
    writer.submit(2);
    writer.submit(3);
    EXPECT_TRUE(writer.busy());
    gate.open();
    writer.flush();

    EXPECT_TRUE(firstInFlight);
    EXPECT_EQ(written.values(), (std::vector<int>{1, 3})) << "2 was replaced before it was written";
    EXPECT_FALSE(writer.busy());
}

TEST(CoalescingWriterTest, ARunOfFailedWritesIsReportedOnce)
{
    bool succeed = false;
    int writes = 0;
    int failures = 0;
    CoalescingWriter<int> writer(
        COALESCING_THREAD_NAME,
        [&succeed, &writes](const int& /*value*/)
        {
            ++writes;
            return succeed;
        },
        [&failures] { ++failures; });

    writer.submit(1);
    writer.flush();
    writer.submit(2);
    writer.flush();
    EXPECT_EQ(writes, 2);
    EXPECT_EQ(failures, 1) << "a second failure in a row isn't reported again";

    succeed = true; // flush() joined the worker, so the next one sees this
    writer.submit(3);
    writer.flush();
    succeed = false;
    writer.submit(4);
    writer.flush();
    EXPECT_EQ(writes, 4);
    EXPECT_EQ(failures, 2) << "a failure after a success is reported";
}

TEST(CoalescingWriterTest, ASinkThatThrowsIsAFailureAndTheWriterGoesOn)
{
    Written written;
    int failures = 0;
    CoalescingWriter<int> writer(
        COALESCING_THREAD_NAME,
        [&written](const int& value) -> bool
        {
            if (value == 1)
            {
                throw std::runtime_error("disk on fire");
            }
            written.record(value);
            return true;
        },
        [&failures] { ++failures; });

    writer.submit(1);
    writer.flush();
    writer.submit(2);
    writer.flush();

    EXPECT_EQ(failures, 1);
    EXPECT_EQ(written.values(), std::vector<int>{2});
}

TEST(CoalescingWriterTest, FlushWritesThePendingSnapshotBeforeItReturns)
{
    Gate gate;
    Written written;
    CoalescingWriter<int> writer(
        COALESCING_THREAD_NAME,
        [&gate, &written](const int& value)
        {
            gate.enter();
            written.record(value);
            return true;
        },
        [] {});

    writer.submit(1);
    const bool firstInFlight = gate.waitEntered(1);
    writer.submit(2); // pending behind the write in flight, as a click just before exit would be
    gate.open();
    writer.flush();

    EXPECT_TRUE(firstInFlight);
    EXPECT_EQ(written.values(), (std::vector<int>{1, 2}));
    EXPECT_FALSE(writer.busy());
}

TEST(CoalescingWriterTest, DestructionWritesThePendingSnapshot)
{
    Gate gate;
    Written written;
    bool firstInFlight = false;
    {
        CoalescingWriter<int> writer(
            COALESCING_THREAD_NAME,
            [&gate, &written](const int& value)
            {
                gate.enter();
                written.record(value);
                return true;
            },
            [] {});
        writer.submit(1);
        firstInFlight = gate.waitEntered(1);
        writer.submit(2);
        gate.open();
    }

    EXPECT_TRUE(firstInFlight);
    EXPECT_EQ(written.values(), (std::vector<int>{1, 2}));
}

TEST(CoalescingWriterTest, ASaveAfterTheWorkerEndedStartsAnother)
{
    Written written;
    CoalescingWriter<int> writer(
        COALESCING_THREAD_NAME,
        [&written](const int& value)
        {
            written.record(value);
            return true;
        },
        [] {});

    writer.submit(1);
    writer.flush();
    writer.submit(2);
    writer.flush();

    EXPECT_EQ(written.values(), (std::vector<int>{1, 2}));
}

/// A thread start that fails the first @c failures times with @c Failure, then starts a real thread.
template<typename Failure> struct FailingStart
{
    static inline std::atomic<int> failures{0}; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables) - the seam is static

    template<typename Body> [[nodiscard]] static std::jthread start(Body&& body)
    {
        if (failures.fetch_sub(1) > 0)
        {
            throw Failure{};
        }
        return StartJthread::start(std::forward<Body>(body));
    }
};

struct NoThread : std::system_error
{
    NoThread() : std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again))
    {}
};

template<typename Failure> void expectAFailedStartLeavesTheNextSubmitFree()
{
    using Starter = FailingStart<Failure>;
    Starter::failures = 1;
    Written written;
    CoalescingWriter<int, Starter> writer(
        COALESCING_THREAD_NAME,
        [&written](const int& value)
        {
            written.record(value);
            return true;
        },
        [] {});

    EXPECT_NO_THROW(writer.submit(1));
    EXPECT_TRUE(written.values().empty()) << "no thread started, so nothing is written yet";
    EXPECT_TRUE(writer.busy()) << "the snapshot is still pending";

    // The next submit() starts a worker (m_Running was reset) and it writes the newest snapshot, with no flush().
    writer.submit(2);
    const auto deadline = std::chrono::steady_clock::now() + WAIT_LIMIT;
    while (writer.busy() && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::yield();
    }
    EXPECT_FALSE(writer.busy());
    EXPECT_EQ(written.values(), std::vector<int>{2});
}

// #4411: std::jthread's constructor also allocates, so a start can fail with bad_alloc, not only system_error.
TEST(CoalescingWriterTest, AStartThatRunsOutOfMemoryLeavesTheWriterAbleToStartAnother)
{
    expectAFailedStartLeavesTheNextSubmitFree<std::bad_alloc>();
}

TEST(CoalescingWriterTest, AStartWithNoThreadLeavesTheWriterAbleToStartAnother)
{
    expectAFailedStartLeavesTheNextSubmitFree<NoThread>();
}

TEST(CoalescingWriterTest, AFlushWritesWhatAFailedStartLeftPending)
{
    using Starter = FailingStart<std::bad_alloc>;
    Starter::failures = 1;
    Written written;
    CoalescingWriter<int, Starter> writer(
        COALESCING_THREAD_NAME,
        [&written](const int& value)
        {
            written.record(value);
            return true;
        },
        [] {});

    writer.submit(7);
    writer.flush();

    EXPECT_EQ(written.values(), std::vector<int>{7});
    EXPECT_FALSE(writer.busy());
}

} // namespace
} // namespace App
