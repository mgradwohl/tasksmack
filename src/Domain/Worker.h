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
//    Admission is synchronous (#3472): everything a job needs before its body (its stop state) is made in
//    submit(), under the lock, so a failure there (no memory) answers Refused, and taking a job off the queue
//    constructs nothing: its node is spliced, not moved, so even a debug STL's container proxy isn't made. A job submit() admitted reaches
//    its body unless it leaves the queue first: stop() or the destructor drops it, a later DropOldest submit drops it, a MergeByKey submit
//    replaces it, or an earlier job's body throws (the worker is Failed, and drops its queue). The worker never fails between.
//  - Cancellation (#3333): every job gets a stop token of its own, signalled by cancelCurrent(), stop() and
//    the destructor. A resource that needs its own safe abort (a WinHTTP request) is guarded by a cancel
//    hook the body arms for exactly the resource's lifetime (JobContext::armCancel()): the hook is called
//    only while armed, and disarming waits for a hook that is running, so it never outlives its target.
//  - Publication: the body publishes immutable snapshots, incremental (progress()) and final (finish()),
//    through a PublicationSlot; the owner reads latest() on the UI thread, which waits for a pointer swap
//    at most. Each publication, and a failure, wakes the frame loop (PublicationWake, #3156).
//  - Shutdown: stop() (and the destructor) cancels the job in flight, waits for it, and drops what is still
//    queued without running it. It allocates nothing, so it can't fail when memory has run out.
//
// One thread per worker (#2731: each data domain on its own worker), started with the first submit(). No
// thread pool. Free of the OS but for the thread name; tested in tests/Domain/test_Worker.cpp.
//
// Typical use:
//
//     Domain::Worker<Request, Result> m_Worker{{.threadName = Platform::X_THREAD_NAME, .capacity = 64,
//                                               .admission = Domain::Admission::Refuse},
//                                              [](Request& job, const std::stop_token& stop, auto& context) {
//                                                  Connection connection(job);
//                                                  auto abort = [&connection] noexcept { connection.abort(); };
//                                                  const auto armed = context.armCancel(abort); // until connection goes
//                                                  context.finish(connection.read(stop));
//                                              }};
//     if (m_Worker.submit(request) == Domain::SubmitResult::Refused) showNotYet(); // copied inside; never throws
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
#include <exception>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
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
    Queued,        ///< Queued at the back, with all it needs to run: its body runs unless it leaves the queue first.
    Merged,        ///< It replaced a queued job with the same key (MergeByKey).
    DroppedOldest, ///< Queued at the back; the oldest queued job was dropped for it (DropOldest).
    /// Not admitted, and the queue is as it was: it is full, or there was no memory to queue the job, or
    /// sameKey threw. Try again later.
    Refused,
    Closed, ///< Not admitted: the worker has stopped or failed (state()), and takes no more work.
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

/// @tparam Job       One unit of work, queued by value. Destroyed unrun when dropped, possibly under the
///                   worker's lock, so its destructor must not call back into the worker. Its move
///                   construction and assignment must not throw: that is what keeps admission transactional.
/// @tparam Snapshot  What the body publishes for the owner; default-constructible (the value before any).
/// @tparam StopState Each job's stop state: std::stop_source, which every worker uses. A test substitutes one
///                   whose construction fails, as std::stop_source's allocation can, on every platform (#3472).
template<typename Job, typename Snapshot, typename StopState = std::stop_source> class Worker
{
    static_assert(std::is_nothrow_move_constructible_v<Job> && std::is_nothrow_move_assignable_v<Job>,
                  "Domain::Worker moves jobs inside its admission and shutdown paths, which must not throw");
    static_assert(std::is_nothrow_move_constructible_v<StopState> && std::is_nothrow_move_assignable_v<StopState>,
                  "Domain::Worker moves a job's stop state off the queue, which must not allocate or throw");

    /// The armed cancel hook: a callable the body owns, and how to call it.
    struct ArmedHook
    {
        const void* target = nullptr; ///< The hook, const or not; call() restores its type.
        void (*call)(const void* target) noexcept = nullptr;
        bool fired = false; ///< Called already: each arming is called at most once.
    };

  public:
    using Publication = WorkerPublication<Snapshot>;

    /// Disarms the cancel hook it armed when it goes, waiting for a call of it that is running.
    class [[nodiscard]] CancelGuard
    {
      public:
        ~CancelGuard()
        {
            m_Owner->disarm(m_Previous);
        }

        CancelGuard(const CancelGuard&) = delete;
        CancelGuard& operator=(const CancelGuard&) = delete;
        CancelGuard(CancelGuard&&) = delete;
        CancelGuard& operator=(CancelGuard&&) = delete;

      private:
        friend class Worker;
        CancelGuard(Worker& owner, ArmedHook previous) noexcept : m_Owner(&owner), m_Previous(previous)
        {}
        Worker* m_Owner;
        ArmedHook m_Previous; // put back when this goes: guards nest
    };

    /// What the body has besides its job: publication, and the cancel hook. Only the worker thread holds
    /// one, so publications are in order.
    class JobContext
    {
      public:
        JobContext(const JobContext&) = delete;
        JobContext& operator=(const JobContext&) = delete;
        JobContext(JobContext&&) = delete;
        JobContext& operator=(JobContext&&) = delete;
        ~JobContext() = default;

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

        /// Arms @p hook as the job's cancel hook until the returned guard goes: cancelCurrent(), stop() and
        /// the destructor call it once, on the cancelling thread, after signalling the job's stop token. If
        /// the job was cancelled already, it is called here, at once. It runs under the worker's lock, and
        /// the guard's destructor takes that lock, so declare the guard after what the hook uses: that then
        /// outlives every call. The hook must be quick and must not call back into the worker. Guards nest:
        /// when an inner one goes, the outer hook is armed again, and called at once if the job was
        /// cancelled meanwhile. @p hook may be const (a const lambda) or not (a mutable one).
        template<typename Hook>
            requires std::is_nothrow_invocable_v<Hook&>
        [[nodiscard]] CancelGuard armCancel(Hook& hook) noexcept
        {
            return m_Owner->arm(ArmedHook{
                .target = static_cast<const void*>(std::addressof(hook)),
                .call =
                    [](const void* target) noexcept
                {
                    // Hook is the type it was armed as, const or not; only storage erased the const.
                    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast) - restores the armed object's own type
                    (*static_cast<Hook*>(const_cast<void*>(target)))();
                },
                .fired = false,
            });
        }

      private:
        friend class Worker;
        explicit JobContext(Worker& owner) noexcept : m_Owner(&owner)
        {}
        Worker* m_Owner; // the worker this belongs to; outlives it
    };

    /// Runs one job on the worker thread. @p stop is the job's own: signalled by cancelCurrent(), stop() and
    /// the destructor. A stop_callback the body registers on it runs with the worker's lock held, so it must
    /// not call back into the worker.
    using Body = std::function<void(Job& job, const std::stop_token& stop, JobContext& context)>;

    struct Options
    {
        /// The thread's name (Platform/ThreadName.h, 15 bytes at most); must outlive the worker.
        std::string_view threadName;
        std::size_t capacity = 1; ///< The most jobs queued at once, not counting the one in flight; at least 1.
        Admission admission = Admission::Refuse;
        /// MergeByKey: whether @p queued and @p incoming are for the same thing. Without it nothing merges.
        // NOLINTNEXTLINE(readability-redundant-member-init) - lets designated initializers leave it out
        std::function<bool(const Job& queued, const Job& incoming)> sameKey{};
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

    /// Offers @p job to the queue under the worker's Admission policy; never waits for a job, and never
    /// throws. Starts the thread on the first call: when it can't start, the worker is Failed and this
    /// answers Closed. Transactional: a job that isn't admitted leaves the queue as it was. A job queued here
    /// carries its stop state, made here (#3472): Queued (or DroppedOldest) means its body runs, unless the job
    /// leaves the queue first (stop(), a DropOldest or MergeByKey submit, an earlier job's body that throws).
    [[nodiscard]] SubmitResult submit(Job&& job) noexcept
    {
        std::optional<Job> dropped; // destroyed after the lock is released
        bool noThread = false;
        const SubmitResult result = [this, &job, &dropped, &noThread] noexcept
        {
            const std::scoped_lock lock(m_Mutex);
            return admitLocked(std::move(job), dropped, noThread);
        }();
        if (noThread)
        {
            PublicationWake::notify(); // the owner shows the failure, as for every other one
        }
        return result;
    }

    /// submit() of a copy of @p job, made here, so a copy that throws (no memory) answers Refused, with the
    /// queue as it was, rather than throwing in the caller. Only for a copyable Job.
    [[nodiscard]] SubmitResult submit(const Job& job) noexcept
    {
        // An ended worker answers Closed whatever the copy would do; admitLocked() checks again under the lock,
        // for an end that comes after this.
        if (m_State.load(std::memory_order_acquire) != WorkerState::Running)
        {
            return SubmitResult::Closed;
        }
        std::optional<Job> copy;
        try
        {
            copy.emplace(job);
        }
        catch (...)
        {
            return SubmitResult::Refused;
        }
        return submit(std::move(*copy));
    }

    /// Signals the job in flight's stop token and calls its armed cancel hook, if any; the queue goes on.
    /// Nothing when no job is in flight.
    void cancelCurrent() noexcept
    {
        const std::scoped_lock lock(m_Mutex);
        cancelInFlightLocked();
    }

    /// Ends the worker: cancels the job in flight (cancelCurrent()), waits for it, and drops the queued jobs
    /// unrun. The state becomes Stopped, unless the worker had failed. Allocates nothing. Safe to call more
    /// than once, from the owner's thread; never from the body (it would wait for itself).
    /// @return How many queued jobs were dropped.
    std::size_t stop() noexcept
    {
        std::size_t dropped = 0;
        {
            const std::scoped_lock lock(m_Mutex);
            WorkerState running = WorkerState::Running;
            static_cast<void>(m_State.compare_exchange_strong(running, WorkerState::Stopped, std::memory_order_acq_rel));
            dropped = m_Queue.size();
            m_Queue.clear(); // in place: draining into another container could allocate (a debug STL's proxy)
            cancelInFlightLocked();
            static_cast<void>(m_Thread.request_stop());
        }
        if (m_Thread.joinable() && m_Thread.get_id() != std::this_thread::get_id())
        {
            m_Thread.join();
        }
        m_Idle.notify_all();
        return dropped;
    }

  private:
    /// submit() under m_Mutex. @p dropped takes a job DropOldest drops; @p noThread is set when the thread
    /// couldn't start, so submit() wakes the frame loop once the lock is released.
    [[nodiscard]] SubmitResult admitLocked(Job&& job, std::optional<Job>& dropped, bool& noThread) noexcept
    {
        if (m_State.load(std::memory_order_relaxed) != WorkerState::Running)
        {
            return SubmitResult::Closed;
        }
        if (!m_Thread.joinable() && !startThreadLocked())
        {
            noThread = true;
            return SubmitResult::Closed;
        }
        try
        {
            if (m_Options.admission == Admission::MergeByKey && m_Options.sameKey)
            {
                // sameKey may throw; nothing has changed until the (non-throwing) move below. The queued entry
                // keeps its stop state: a merge makes none.
                const auto same =
                    std::ranges::find_if(m_Queue, [this, &job](const QueuedJob& queued) { return m_Options.sameKey(queued.job, job); });
                if (same != m_Queue.end())
                {
                    same->job = std::move(job);
                    return SubmitResult::Merged;
                }
            }
            const bool full = m_Queue.size() >= m_Options.capacity;
            if (full && m_Options.admission != Admission::DropOldest)
            {
                return SubmitResult::Refused;
            }
            // Only the job's stop state and the push allocate (no memory throws), and either leaves the queue and
            // @p job as they were: the entry is built in place, after the push has its room. The rest moves.
            StopState stop;
            m_Queue.emplace_back(std::move(job), std::move(stop));
            if (full)
            {
                dropped.emplace(std::move(m_Queue.front().job));
                m_Queue.pop_front();
            }
            m_Wake.notify_one();
            return full ? SubmitResult::DroppedOldest : SubmitResult::Queued;
        }
        catch (...)
        {
            return SubmitResult::Refused;
        }
    }

  public:
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

    /// Waits up to @p timeout until no job is queued or in flight: every job taken has been destroyed, and
    /// every queued one run or dropped. A worker that failed or stopped gets there once its last job is gone,
    /// not before. For tests and shutdown paths only: never on the UI thread. @return Whether it did in time.
    [[nodiscard]] bool waitUntilIdle(std::chrono::milliseconds timeout)
    {
        std::unique_lock lock(m_Mutex);
        return m_Idle.wait_for(lock, timeout, [this] { return m_Queue.empty() && !m_InFlight; });
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

    /// JobContext::armCancel(): arms @p hook (calling it at once if the job was cancelled already).
    [[nodiscard]] CancelGuard arm(ArmedHook hook) noexcept
    {
        const std::scoped_lock lock(m_Mutex);
        const ArmedHook previous = std::exchange(m_Hook, hook);
        if (m_JobStop.stop_requested())
        {
            fireHookLocked();
        }
        return CancelGuard(*this, previous);
    }

    /// CancelGuard's end: puts back the hook armed before. Taking the lock waits for a call that is running.
    /// A job cancelled while the inner hook was armed delivers that cancel to the outer one now, once: it
    /// may be what aborts the job.
    void disarm(ArmedHook previous) noexcept
    {
        const std::scoped_lock lock(m_Mutex);
        m_Hook = previous;
        if (m_InFlight && m_JobStop.stop_requested())
        {
            fireHookLocked();
        }
    }

    /// Calls the armed hook once (under m_Mutex).
    void fireHookLocked() noexcept
    {
        if (m_Hook.call != nullptr && !m_Hook.fired)
        {
            m_Hook.fired = true;
            m_Hook.call(m_Hook.target);
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
        fireHookLocked();
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

    /// The thread. Never throws: the body's exceptions end it in loop(), with their reason. The loop's own
    /// bookkeeping constructs nothing per job (#3472): the job's stop state is made at submit, its node is
    /// spliced off the queue, and a failure's reason goes into storage made with the worker. What is left of it
    /// that could throw (waiting, std::system_error) ends it here.
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
        JobContext context(*this);
        while (true)
        {
            std::stop_token jobStop;
            {
                std::unique_lock lock(m_Mutex);
                if (!m_Wake.wait(lock, stop, [this] { return !m_Queue.empty(); }))
                {
                    return; // stopped; stop() dropped the queue
                }
                // Nothing is constructed here (#3472), so nothing can fail between Queued and the body: the job's
                // node moves to m_Current as it is (a splice, not a move of the job, which a debug STL's container
                // proxy would allocate for), and its stop state, made at admission, moves to m_JobStop.
                m_Current.splice(m_Current.end(), m_Queue, m_Queue.begin());
                m_JobStop = std::move(m_Current.front().stop);
                jobStop = m_JobStop.get_token();
                m_InFlight = true;
            }
            const std::optional<std::string_view> failure = runJob(m_Current.front().job, jobStop, context);
            m_Current.clear(); // the job's resources go before it counts as done, failed or not
            {
                const std::scoped_lock lock(m_Mutex);
                if (failure.has_value())
                {
                    failLocked(*failure);
                    endLocked();
                }
                else
                {
                    m_InFlight = false;
                    if (m_Queue.empty())
                    {
                        m_Idle.notify_all();
                    }
                }
            }
            if (failure.has_value())
            {
                PublicationWake::notify(); // the owner shows the failure
                return;
            }
        }
    }

    /// The no-throw boundary: runs the body on @p job. @return The reason when it threw (pointing into
    /// m_FailureWhat, or a fixed text), and nullopt when it returned. The caller fails the worker, after the job
    /// is gone.
    std::optional<std::string_view> runJob(Job& job, const std::stop_token& jobStop, JobContext& context) noexcept
    {
        std::string_view reason;
        try
        {
            m_Body(job, jobStop, context);
            return std::nullopt;
        }
        catch (const std::exception& e)
        {
            try
            {
                m_FailureWhat = e.what();
                reason = m_FailureWhat;
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
        return reason;
    }

    /// A failed worker's end (under m_Mutex): no job in flight, and the queue dropped in place.
    void endLocked() noexcept
    {
        m_InFlight = false;
        m_Queue.clear();
        m_Idle.notify_all();
    }

    /// A queued job and its stop state, made at admission (#3472).
    struct QueuedJob
    {
        QueuedJob(Job&& queuedJob, StopState&& queuedStop) noexcept : job(std::move(queuedJob)), stop(std::move(queuedStop))
        {}
        Job job;
        StopState stop;
    };

    Options m_Options;
    Body m_Body;

    mutable std::mutex m_Mutex;
    std::condition_variable_any m_Wake;
    std::condition_variable m_Idle;
    // Guarded by m_Mutex
    std::list<QueuedJob> m_Queue; // a list, so the thread takes a job by splicing its node (#3472)
    bool m_InFlight = false;      // the thread holds a job it took off the queue
    StopState m_JobStop;          // the job in flight's, moved off the queue with it
    ArmedHook m_Hook;             // the job's cancel hook, while its CancelGuard lives
    std::string m_Failure;        // why it failed
    bool m_FailureUnrecorded = false;
    // Written under m_Mutex, read without it
    std::atomic<WorkerState> m_State{WorkerState::Running};

    // The worker thread's. Made with the worker, so a job constructs neither before its body (#3472).
    std::list<QueuedJob> m_Current; // the job in flight's node, spliced off m_Queue under m_Mutex
    std::string m_FailureWhat;      // a body's exception text, copied on this thread (#1685)
    std::uint64_t m_NextVersion = 0;
    PublicationSlot<Publication> m_Slot;

    std::jthread m_Thread; // last: stopped and joined first, while the rest is alive
};

} // namespace Domain
