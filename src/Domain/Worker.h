#pragma once

// One background worker thread with the rules every TaskSmack worker needs, so no worker restates them
// (#3343). A raw std::jthread joins its thread and nothing more; each of these is a bug a hand-rolled
// worker has had:
//
//  - Exception containment (#3335): the body runs inside a no-throw boundary. An exception escaping it
//    ends the worker in the terminal Failed state, its reason copied on the worker thread (the owner never
//    reads an exception object thrown on another thread, #1685). Never std::terminate. A reason that can't
//    be copied (no memory) still reads as a fixed text.
//  - Bounded admission (#3340): the queue holds at most `capacity` jobs, and each worker states what a
//    full queue does (Admission). submit() says what it did, so a caller can show "not checked yet".
//  - Cancellation (#3333): every job gets a stop token of its own, signalled by cancelCurrent(), stop() and
//    the destructor. A resource that needs its own safe abort (a WinHTTP request) supplies the
//    cancelInFlight hook, which is only ever called while a job is in flight: the job can't finish, and the
//    next can't start, until the hook returns.
//  - Publication: the body publishes immutable snapshots, incremental (progress()) and final (finish()),
//    through a PublicationSlot; the owner reads latest() on the UI thread, which waits for a pointer swap
//    at most. Each publication, and a failure, wakes the frame loop (PublicationWake, #3156).
//  - Shutdown: stop() (and the destructor) cancels the job in flight, waits for it, and drops what is still
//    queued without running it.
//
// One thread per worker (#2731: each data domain on its own worker), started with the first submit(). No
// thread pool. Free of the OS but for the thread name; tested in tests/Domain/test_Worker.cpp.
//
// Typical use:
//
//     Domain::Worker<Request, Result> m_Worker{{.threadName = Platform::X_THREAD_NAME, .capacity = 64,
//                                               .admission = Domain::Admission::Refuse},
//                                              [](Request& job, const std::stop_token& stop, auto& out) {
//                                                  out.finish(read(job, stop));
//                                              }};
//     if (m_Worker.submit(request) == Domain::SubmitResult::Refused) showNotYet();
//     if (auto published = m_Worker.latest(); published->version != m_Seen) show(published->value);
//     if (m_Worker.state() == Domain::WorkerState::Failed) showFailed(m_Worker.failure());

#include "Domain/PublicationSlot.h"
#include "Domain/PublicationWake.h"
#include "Platform/ThreadName.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace Domain
{

/// What a full queue does with one more job.
enum class Admission : std::uint8_t
{
    Refuse,     ///< The new job isn't admitted: SubmitResult::Refused.
    DropOldest, ///< The oldest queued job is dropped, unrun, for the new one: SubmitResult::DroppedOldest.
    /// A queued job with the same key (Options::sameKey) is replaced in place, keeping its turn, whether or not
    /// the queue is full: SubmitResult::Merged. Otherwise a full queue refuses, as Refuse does.
    MergeByKey,
};

/// What submit() did with a job.
enum class SubmitResult : std::uint8_t
{
    Queued,        ///< Queued at the back.
    Merged,        ///< It replaced a queued job with the same key (MergeByKey).
    DroppedOldest, ///< Queued at the back; the oldest queued job was dropped for it (DropOldest).
    Refused,       ///< Not admitted: the queue is full, or there was no memory to queue it. Try again later.
    Closed,        ///< Not admitted: the worker has stopped or failed (state()), and takes no more work.
};

/// Where a worker is in its life. Failed and Stopped are terminal: a worker that should go on is replaced.
enum class WorkerState : std::uint8_t
{
    Running, ///< Taking work (its thread starts with the first submit()).
    Failed,  ///< A job's body threw, or no thread could be started: failure() says why.
    Stopped, ///< stop() or the destructor ended it.
};

/// One publication of a worker's snapshot.
template<typename Snapshot> struct WorkerPublication
{
    std::uint64_t version = 0; ///< 0 before the first publication; increases with every one after.
    bool isFinal = false;      ///< finish() published it: the job's result, not its progress.
    Snapshot value{};
};

/// @tparam Job       One unit of work, queued by value. Destroyed unrun when dropped.
/// @tparam Snapshot  What the body publishes for the owner; default-constructible (the value before any).
template<typename Job, typename Snapshot> class Worker
{
  public:
    using Publication = WorkerPublication<Snapshot>;

    /// How the body publishes. Only the worker thread holds one, so publications are in order.
    class Publisher
    {
      public:
        Publisher(const Publisher&) = delete;
        Publisher& operator=(const Publisher&) = delete;
        Publisher(Publisher&&) = delete;
        Publisher& operator=(Publisher&&) = delete;
        ~Publisher() = default;

        /// Publishes @p value as the job's progress so far. Throws std::bad_alloc, which fails the worker
        /// like any exception from the body.
        void progress(Snapshot value)
        {
            m_Owner->publish(std::move(value), false);
        }

        /// Publishes @p value as the job's result. The body may still publish after it.
        void finish(Snapshot value)
        {
            m_Owner->publish(std::move(value), true);
        }

      private:
        friend class Worker;
        explicit Publisher(Worker& owner) noexcept : m_Owner(&owner)
        {}
        Worker* m_Owner; // the worker this belongs to; outlives it
    };

    /// Runs one job on the worker thread. @p stop is the job's own: signalled by cancelCurrent(), stop() and
    /// the destructor. A stop_callback the body registers on it runs with the worker's lock held, so it must
    /// not call back into the worker.
    using Body = std::function<void(Job& job, const std::stop_token& stop, Publisher& out)>;

    struct Options
    {
        /// The thread's name (Platform/ThreadName.h, 15 bytes at most); must outlive the worker.
        std::string_view threadName;
        std::size_t capacity = 1; ///< The most jobs queued at once, not counting the one in flight; at least 1.
        Admission admission = Admission::Refuse;
        /// MergeByKey: whether @p queued and @p incoming are for the same thing. Without it nothing merges.
        // NOLINTNEXTLINE(readability-redundant-member-init) - lets designated initializers leave it out
        std::function<bool(const Job& queued, const Job& incoming)> sameKey{};
        /// Called by cancelCurrent(), stop() and the destructor while a job is in flight, after its stop token
        /// is signalled, on the cancelling thread and with the worker's lock held: the job can't finish until
        /// it returns. For a resource that needs its own abort; it must be quick, never throw, and never call
        /// back into the worker.
        // NOLINTNEXTLINE(readability-redundant-member-init) - lets designated initializers leave it out
        std::function<void()> cancelInFlight{};
    };

    /// The failure() text when the reason itself couldn't be copied (no memory).
    static constexpr std::string_view FAILURE_UNRECORDED = "The worker failed (no memory to record why)";
    /// The failure() text for a body that threw something other than a std::exception.
    static constexpr std::string_view FAILURE_UNKNOWN = "Unknown error";
    /// The failure() text when no thread could be started.
    static constexpr std::string_view FAILURE_NO_THREAD = "No thread could be started for the work";

    Worker(Options options, Body body) : m_Options(std::move(options)), m_Body(std::move(body))
    {
        m_Options.capacity = std::max<std::size_t>(m_Options.capacity, 1);
    }

    /// stop(): cancels the job in flight, waits for it, and drops the queue.
    ~Worker()
    {
        static_cast<void>(stop());
    }

    // Not copyable or movable: the thread holds `this`.
    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;
    Worker(Worker&&) = delete;
    Worker& operator=(Worker&&) = delete;

    /// Offers @p job to the queue under the worker's Admission policy; never waits for a job. Starts the
    /// thread on the first call: when it can't start, the worker is Failed and this answers Closed.
    [[nodiscard]] SubmitResult submit(Job job) noexcept
    {
        std::optional<Job> dropped; // destroyed after the lock is released
        const std::scoped_lock lock(m_Mutex);
        if (m_State.load(std::memory_order_relaxed) != WorkerState::Running)
        {
            return SubmitResult::Closed;
        }
        if (!m_Thread.joinable() && !startThreadLocked())
        {
            return SubmitResult::Closed;
        }
        try
        {
            if (m_Options.admission == Admission::MergeByKey && m_Options.sameKey)
            {
                const auto same = std::ranges::find_if(m_Queue, [this, &job](const Job& queued) { return m_Options.sameKey(queued, job); });
                if (same != m_Queue.end())
                {
                    *same = std::move(job);
                    return SubmitResult::Merged;
                }
            }
            const bool full = m_Queue.size() >= m_Options.capacity;
            if (full && m_Options.admission != Admission::DropOldest)
            {
                return SubmitResult::Refused;
            }
            // Queued first, so a push that throws leaves the queue as it was; then the oldest goes.
            m_Queue.push_back(std::move(job));
            if (full)
            {
                dropped.emplace(std::move(m_Queue.front()));
                m_Queue.pop_front();
            }
            m_Wake.notify_one();
            return full ? SubmitResult::DroppedOldest : SubmitResult::Queued;
        }
        catch (...)
        {
            // No memory to queue it (or to compare keys): not admitted, as a full queue's refusal is.
            return SubmitResult::Refused;
        }
    }

    /// Signals the job in flight's stop token and calls the cancelInFlight hook; the queue goes on. Nothing
    /// when no job is in flight.
    void cancelCurrent() noexcept
    {
        const std::scoped_lock lock(m_Mutex);
        cancelInFlightLocked();
    }

    /// Ends the worker: cancels the job in flight (cancelCurrent()), waits for it, and drops the queued jobs
    /// unrun. The state becomes Stopped, unless the worker had failed. Safe to call more than once, from the
    /// owner's thread; never from the body (it would wait for itself).
    /// @return How many queued jobs were dropped.
    std::size_t stop() noexcept
    {
        std::deque<Job> dropped; // destroyed after the lock is released
        {
            const std::scoped_lock lock(m_Mutex);
            WorkerState running = WorkerState::Running;
            static_cast<void>(m_State.compare_exchange_strong(running, WorkerState::Stopped, std::memory_order_acq_rel));
            dropped.swap(m_Queue);
            cancelInFlightLocked();
            static_cast<void>(m_Thread.request_stop());
        }
        if (m_Thread.joinable() && m_Thread.get_id() != std::this_thread::get_id())
        {
            m_Thread.join();
        }
        m_Idle.notify_all();
        return dropped.size();
    }

    /// The latest publication (version 0 before any); a shared_ptr copy under the slot's swap lock.
    [[nodiscard]] std::shared_ptr<const Publication> latest() const noexcept
    {
        return m_Slot.load();
    }

    /// The latest publication's version, without locking.
    [[nodiscard]] std::uint64_t publishedVersion() const noexcept
    {
        return m_Slot.version();
    }

    [[nodiscard]] WorkerState state() const noexcept
    {
        return m_State.load(std::memory_order_acquire);
    }

    /// Why the worker failed; empty unless state() is Failed.
    [[nodiscard]] std::string failure() const
    {
        const std::scoped_lock lock(m_Mutex);
        if (m_State.load(std::memory_order_relaxed) != WorkerState::Failed)
        {
            return {};
        }
        return m_FailureUnrecorded ? std::string(FAILURE_UNRECORDED) : m_Failure;
    }

    /// The jobs queued, not counting the one in flight.
    [[nodiscard]] std::size_t queued() const noexcept
    {
        const std::scoped_lock lock(m_Mutex);
        return m_Queue.size();
    }

    /// Whether a job is in flight.
    [[nodiscard]] bool busy() const noexcept
    {
        const std::scoped_lock lock(m_Mutex);
        return m_InFlight;
    }

    /// Waits up to @p timeout until no job is queued or in flight (or the worker ended). For tests and
    /// shutdown paths only: never on the UI thread. @return Whether it got there in time.
    [[nodiscard]] bool waitUntilIdle(std::chrono::milliseconds timeout)
    {
        std::unique_lock lock(m_Mutex);
        return m_Idle.wait_for(
            lock,
            timeout,
            [this] { return (m_Queue.empty() && !m_InFlight) || m_State.load(std::memory_order_relaxed) != WorkerState::Running; });
    }

  private:
    /// Starts the thread (under m_Mutex). When it can't, the worker is Failed. @return Whether it started.
    bool startThreadLocked() noexcept
    {
        try
        {
            m_Thread = std::jthread([this](const std::stop_token& stop) noexcept { run(stop); });
            return true;
        }
        catch (...)
        {
            // std::system_error (no thread), or std::bad_alloc for its state.
            failLocked(FAILURE_NO_THREAD);
            return false;
        }
    }

    /// cancelCurrent() under m_Mutex.
    void cancelInFlightLocked() noexcept
    {
        if (!m_InFlight)
        {
            return;
        }
        static_cast<void>(m_JobStop.request_stop());
        if (m_Options.cancelInFlight)
        {
            try
            {
                m_Options.cancelInFlight();
            }
            catch (...) // NOLINT(bugprone-empty-catch) - the hook must not throw; the stop token is signalled anyway
            {}
        }
    }

    /// Records @p reason and makes the worker Failed (under m_Mutex). Never throws: a reason that can't be
    /// copied reads as FAILURE_UNRECORDED.
    void failLocked(std::string_view reason) noexcept
    {
        try
        {
            m_Failure.assign(reason);
        }
        catch (...)
        {
            m_FailureUnrecorded = true;
        }
        // Stored after the reason, so a reader that sees Failed sees why. A worker stopped already stays Stopped.
        WorkerState running = WorkerState::Running;
        static_cast<void>(m_State.compare_exchange_strong(running, WorkerState::Failed, std::memory_order_acq_rel));
    }

    /// Publishes on the worker thread, the only writer, so the versions increase.
    void publish(Snapshot value, bool isFinal)
    {
        auto next =
            std::make_shared<const Publication>(Publication{.version = ++m_NextVersion, .isFinal = isFinal, .value = std::move(value)});
        m_Slot.commit(std::move(next));
        PublicationWake::notify();
    }

    /// The thread. Never throws: the body's exceptions end it in runJob(), with their reason; anything the
    /// loop's own bookkeeping throws (no memory for a job's stop state) ends it here.
    void run(const std::stop_token& stop) noexcept
    {
        static_cast<void>(Platform::setCurrentThreadName(m_Options.threadName));
        try
        {
            loop(stop);
        }
        catch (...)
        {
            {
                const std::scoped_lock lock(m_Mutex);
                failLocked(FAILURE_UNRECORDED);
                endLocked();
            }
            PublicationWake::notify();
        }
    }

    /// One job at a time until stopped, or until a body throws.
    void loop(const std::stop_token& stop)
    {
        Publisher out(*this);
        while (true)
        {
            std::optional<Job> job;
            std::stop_token jobStop;
            {
                std::unique_lock lock(m_Mutex);
                if (!m_Wake.wait(lock, stop, [this] { return !m_Queue.empty(); }))
                {
                    return; // stopped; stop() dropped the queue
                }
                job.emplace(std::move(m_Queue.front()));
                m_Queue.pop_front();
                m_JobStop = std::stop_source{};
                jobStop = m_JobStop.get_token();
                m_InFlight = true;
            }
            const bool ran = runJob(*job, jobStop, out);
            job.reset(); // the job's resources go before it counts as done
            if (!ran)
            {
                return;
            }
            {
                const std::scoped_lock lock(m_Mutex);
                m_InFlight = false;
                if (m_Queue.empty())
                {
                    m_Idle.notify_all();
                }
            }
        }
    }

    /// The no-throw boundary: runs the body on @p job. @return false when it threw, and the worker is Failed.
    bool runJob(Job& job, const std::stop_token& jobStop, Publisher& out) noexcept
    {
        std::string_view reason;
        std::string what; // copied here, on the thread that threw (#1685)
        try
        {
            m_Body(job, jobStop, out);
            return true;
        }
        catch (const std::exception& e)
        {
            try
            {
                what = e.what();
                reason = what;
            }
            catch (...)
            {
                reason = FAILURE_UNRECORDED;
            }
        }
        catch (...)
        {
            reason = FAILURE_UNKNOWN;
        }
        {
            const std::scoped_lock lock(m_Mutex);
            failLocked(reason);
            endLocked();
        }
        PublicationWake::notify(); // the owner shows the failure
        return false;
    }

    /// A failed worker's end (under m_Mutex): no job in flight, and the queue dropped. The jobs are moved
    /// out first only by stop(); here they go at once, under the lock, since the thread is ending.
    void endLocked() noexcept
    {
        m_InFlight = false;
        m_Queue.clear();
        m_Idle.notify_all();
    }

    Options m_Options;
    Body m_Body;

    mutable std::mutex m_Mutex;
    std::condition_variable_any m_Wake;
    std::condition_variable m_Idle;
    // Guarded by m_Mutex
    std::deque<Job> m_Queue;
    bool m_InFlight = false;    // the thread holds a job it took off the queue
    std::stop_source m_JobStop; // the job in flight's
    std::string m_Failure;      // why it failed
    bool m_FailureUnrecorded = false;
    // Written under m_Mutex, read without it
    std::atomic<WorkerState> m_State{WorkerState::Running};

    std::uint64_t m_NextVersion = 0; // the worker thread's
    PublicationSlot<Publication> m_Slot;

    std::jthread m_Thread; // last: stopped and joined first, while the rest is alive
};

} // namespace Domain
