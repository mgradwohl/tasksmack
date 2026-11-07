#pragma once

// One piece of work at a time on a worker thread of its own, its result taken in on the UI thread
// (#2139): the worker slot every App view that waits on a slow platform call shares -- a dump, a
// service or startup action, the System Information and crash history reads, the lazy Process Details
// reads. Each start() runs its work as the one job of a new Domain::Worker (#3343), so the worker's rules
// hold for every caller; this owns the rules each of those sites used to restate:
//
//  - The work's exception is caught on the worker and turned into the view's Failed result there, so
//    the UI thread gets a plain Result and never an exception object (#1685, #1706: the message buffer
//    of a std::runtime_error is reference-counted inside the C++ runtime, invisibly to ThreadSanitizer).
//  - What can still throw on the worker past the work (building the Failed result, or publishing the
//    result: bad_alloc) ends the worker in its Failed state, never std::terminate; poll() reports that as
//    the Failed result with the unknown-error detail, the exception unread.
//  - No thread to run it on, or no memory to start it, is the Failed result too, at once, from start().
//  - Each start() gets a new stop token; requestStop() is for work that polls it (a dump, a service
//    action, #1591). The destructor and move assignment request a stop, then wait for the work: work
//    started always runs, never dropped unrun, as a view's last action (a setting written at exit) needs.
//  - The result is the worker's final publication, which wakes the frame loop (Domain::PublicationWake,
//    #3156) once it is published, so a static tab shows it at once and a frame the wake brings finds it.
//  - No thread sits idle between starts: once poll() takes the result in, the worker is closed (its thread
//    ends by itself) and destroyed at the next start() or with this, when there is nothing left to wait for.
//
// One exception to the worker: adopt() takes in an answer something else delivers (a promise filled from
// SDL's event pump) as a std::future. No thread of ours runs it, so there is no work to put on a worker.
//
// Typical use:
//
//     BackgroundTask<Result> m_Worker{Platform::X_THREAD_NAME, &failedResult};
//     if (auto failed = m_Worker.start([actions, request](std::stop_token stop) { return actions->run(request, stop); }))
//         show(*failed);                  // no thread could be started
//     if (auto result = m_Worker.poll())  // each frame, on the UI thread
//         show(*result);
//
// Free of ImGui; tested in tests/App/test_BackgroundTask.cpp.

#include "Domain/Worker.h"

#include <chrono>
#include <exception>
#include <future>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace App
{

/// @tparam Result The work's result; Failed results are built by the view's FailedFn.
template<typename Result> class BackgroundTask
{
  public:
    /// Builds the Failed result for work that threw, or for which no thread could be started, from the
    /// exception's message (or the unknown-error detail).
    using FailedFn = Result (*)(std::string detail);

    /// The detail of work that threw something other than a std::exception, unless the view names its own.
    static constexpr std::string_view UNKNOWN_ERROR = "Unknown error";
    /// The detail of work that couldn't be queued on its new worker (no memory for its stop state).
    static constexpr std::string_view NOT_QUEUED = "No memory to start the work";

    /// @param threadName     The worker's name (Platform/ThreadName.h, 15 bytes at most); must outlive this.
    /// @param failed         Builds a Failed result from a detail.
    /// @param unknownDetail  The detail for work that threw a non-std exception; must outlive this.
    BackgroundTask(std::string_view threadName, FailedFn failed, std::string_view unknownDetail = UNKNOWN_ERROR) noexcept
        : m_ThreadName(threadName), m_Failed(failed), m_UnknownDetail(unknownDetail)
    {}

    /// Requests a stop and waits for work still running (an adopted answer is dropped, see adopt()).
    ~BackgroundTask()
    {
        requestStop();
        wait();
    }

    BackgroundTask(const BackgroundTask&) = delete;
    BackgroundTask& operator=(const BackgroundTask&) = delete;

    /// Work in flight moves with it: the worker is held by pointer, and shares only the stop token's state.
    BackgroundTask(BackgroundTask&& other) noexcept
        : m_ThreadName(other.m_ThreadName),
          m_Failed(other.m_Failed),
          m_UnknownDetail(other.m_UnknownDetail),
          m_Future(std::move(other.m_Future)),
          m_Stop(std::move(other.m_Stop)),
          m_Adopted(std::exchange(other.m_Adopted, false)),
          m_Retired(std::move(other.m_Retired)),
          m_Running(std::move(other.m_Running))
    {}

    /// Requests a stop and waits for this task's own work first, as the destructor does.
    BackgroundTask& operator=(BackgroundTask&& other) noexcept
    {
        if (this != &other)
        {
            requestStop();
            wait();
            m_ThreadName = other.m_ThreadName;
            m_Failed = other.m_Failed;
            m_UnknownDetail = other.m_UnknownDetail;
            m_Future = std::move(other.m_Future);
            m_Stop = std::move(other.m_Stop);
            m_Adopted = std::exchange(other.m_Adopted, false);
            m_Retired = std::move(other.m_Retired); // this task's own goes: its work finished, so nothing is waited on
            m_Running = std::move(other.m_Running); // likewise, after wait()
        }
        return *this;
    }

    /// Starts @p work on a new worker named threadName, with a new stop token: @p work is invoked as
    /// work(std::stop_token) when it takes one, else as work(). It must own (copy, or share) everything
    /// it uses, or the caller keeps that alive until the work finishes (destroying this waits for it).
    /// Does nothing while busy(): the work in flight is never replaced or waited for here.
    /// @return The Failed result when no thread could be started, or there was no memory to start the work;
    ///         nullopt when the work started (or another was still in flight).
    template<typename Work> [[nodiscard]] std::optional<Result> start(Work work)
    {
        if (busy())
        {
            return std::nullopt;
        }
        // The last work's worker, closed when its result was taken in: its thread has ended, or is ending with
        // nothing left to do, so this join waits on no work.
        m_Retired.reset();
        m_Adopted = false;
        std::string detail;
        try
        {
            m_Stop = std::stop_source{}; // a stop of earlier work does not carry over
            auto worker = std::make_unique<TaskWorker>(
                typename TaskWorker::Options{.threadName = m_ThreadName, .capacity = 1, .admission = Domain::Admission::Refuse},
                makeBody(m_Failed, m_UnknownDetail));
            const Domain::SubmitResult submitted =
                worker->submit(Job{.work = std::make_unique<HeldWork<Work>>(std::move(work)), .stop = m_Stop});
            if (submitted == Domain::SubmitResult::Queued)
            {
                m_Running = std::move(worker);
                return std::nullopt;
            }
            // A new worker answers Closed only when its thread couldn't start (it is Failed and says so), and
            // Refused only when the job's stop state couldn't be made.
            detail = submitted == Domain::SubmitResult::Closed ? worker->failure() : std::string(NOT_QUEUED);
        }
        catch (const std::exception& e)
        {
            // No memory for the stop source, the worker or the job: nothing was started.
            detail = e.what();
        }
        return m_Failed(std::move(detail));
    }

    /// Takes in an answer something else delivers instead of a worker: a promise filled from SDL's
    /// event pump, say. busy() and poll() treat it as started work; wait() and the destructor drop it
    /// instead of waiting, since it may be answered from the very thread that would wait. Does nothing
    /// while busy().
    void adopt(std::future<Result> answer) noexcept
    {
        if (busy())
        {
            return;
        }
        m_Future = std::move(answer);
        m_Adopted = true;
    }

    /// Whether work was started (or an answer adopted) and its result not taken in yet.
    [[nodiscard]] bool busy() const noexcept
    {
        return m_Running != nullptr || m_Future.valid();
    }

    /// The result, once the work has finished (or, with @p wait, after waiting for it); nullopt while
    /// none is in flight or it is still running. Never rethrows: see the file comment.
    [[nodiscard]] std::optional<Result> poll(bool wait = false)
    {
        if (m_Future.valid())
        {
            return pollAdopted(wait);
        }
        if (m_Running == nullptr)
        {
            return std::nullopt;
        }
        if (wait)
        {
            waitForRunning();
        }
        // The publication first, then the state: a worker that published has finished its work, and one that
        // failed publishes nothing after.
        const std::shared_ptr<const Publication> published = m_Running->latest();
        if (published->version == 0 && m_Running->state() == Domain::WorkerState::Running)
        {
            return std::nullopt;
        }
        const std::shared_ptr<Result> answer = published->version != 0 ? published->value : nullptr;
        retire();
        if (answer == nullptr)
        {
            // The worker failed past the work (building its Failed result, or publishing the result, threw:
            // bad_alloc). The exception was contained on the worker and is not read here (#1685).
            return m_Failed(std::string(m_UnknownDetail));
        }
        try
        {
            // Moved, not copied: the worker never touches a result once it is published, and this is its only reader.
            return std::optional<Result>(std::move(*answer));
        }
        catch (...)
        {
            return m_Failed(std::string(m_UnknownDetail)); // moving the result threw (bad_alloc)
        }
    }

    /// Asks the work in flight to stop, through its stop token; no effect on work that ignores it.
    void requestStop() noexcept
    {
        static_cast<void>(m_Stop.request_stop()); // false when already requested: nothing more to do
    }

    /// Waits for the work in flight to finish, leaving its result for poll(). An adopted answer is
    /// dropped instead (busy() is false after): waiting could never end if this thread answers it.
    void wait() noexcept
    {
        if (m_Adopted)
        {
            m_Future = {};
            m_Adopted = false;
        }
        if (m_Running != nullptr)
        {
            waitForRunning();
        }
    }

  private:
    /// The work, type-erased, so one worker type runs every Work a view starts.
    class ErasedWork
    {
      public:
        ErasedWork() = default;
        virtual ~ErasedWork() = default;
        ErasedWork(const ErasedWork&) = delete;
        ErasedWork& operator=(const ErasedWork&) = delete;
        ErasedWork(ErasedWork&&) = delete;
        ErasedWork& operator=(ErasedWork&&) = delete;

        /// The work's result, or the Failed result for work that threw (runOrFailed()).
        [[nodiscard]] virtual Result run(const std::stop_token& stopToken, FailedFn failed, std::string_view unknown) = 0;
    };

    template<typename Work> class HeldWork final : public ErasedWork
    {
      public:
        explicit HeldWork(Work&& work) : m_Work(std::move(work))
        {}
        [[nodiscard]] Result run(const std::stop_token& stopToken, FailedFn failed, std::string_view unknown) override
        {
            return runOrFailed(m_Work, stopToken, failed, unknown);
        }

      private:
        Work m_Work;
    };

    /// The worker's one job: the work, and the stop source requestStop() signals (it shares m_Stop's state).
    struct Job
    {
        std::unique_ptr<ErasedWork> work;
        std::stop_source stop;
    };

    /// What the worker publishes: the result, once. A pointer, so a Result needn't be default-constructible,
    /// and poll() moves it out rather than copying it.
    using TaskWorker = Domain::Worker<Job, std::shared_ptr<Result>>;
    using Publication = TaskWorker::Publication;

    /// The job's body: runs the work and publishes its result.
    [[nodiscard]] static TaskWorker::Body makeBody(FailedFn failed, std::string_view unknown)
    {
        return [failed, unknown](Job& job, const std::stop_token& workerStop, TaskWorker::JobContext& context)
        {
            // The worker's own cancel (its stop(), were it destroyed with the work still running) reaches the work
            // too, through the stop source the work polls. It runs under the worker's lock, and only signals.
            std::stop_source& stop = job.stop;
            const std::stop_callback link(workerStop, [&stop] noexcept { static_cast<void>(stop.request_stop()); });
            Result result = [&job, &stop, failed, unknown]
            {
                // The work is moved out and released here, before the result is published.
                const std::unique_ptr<ErasedWork> work = std::move(job.work);
                return work->run(stop.get_token(), failed, unknown);
            }();
            // No memory for the publication throws, which fails the worker: poll() reports that.
            context.finish(std::make_shared<Result>(std::move(result)));
        };
    }

    /// Runs on the worker: @p work, or the Failed result for work that threw, its message copied here on
    /// the thread that threw it. Only building the Failed result itself (bad_alloc) can still escape, to the
    /// worker's own boundary.
    template<typename Work>
    [[nodiscard]] static Result runOrFailed(Work& work, const std::stop_token& stopToken, FailedFn failed, std::string_view unknown)
    {
        try
        {
            if constexpr (std::is_invocable_v<Work&, std::stop_token>)
            {
                return work(stopToken);
            }
            else
            {
                return work();
            }
        }
        catch (const std::exception& e)
        {
            return failed(std::string(e.what()));
        }
        catch (...)
        {
            return failed(std::string(unknown));
        }
    }

    /// poll() of an adopted answer.
    [[nodiscard]] std::optional<Result> pollAdopted(bool wait)
    {
        if (!wait && m_Future.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        {
            return std::nullopt;
        }
        m_Adopted = false;
        try
        {
            return m_Future.get();
        }
        catch (...)
        {
            // A broken promise, or an exception the answering side set; not read, as it was made on another
            // thread (#1685).
            return m_Failed(std::string(m_UnknownDetail));
        }
    }

    /// Waits until the running work has finished: published its result, or failed.
    void waitForRunning() noexcept
    {
        try
        {
            while (!m_Running->waitUntilIdle(std::chrono::seconds(1)))
            {
                // Work that runs for longer than a second: go on waiting, as for any other.
            }
        }
        catch (...) // NOLINT(bugprone-empty-catch): a wait that throws (std::system_error) gives up; the worker's stop() still joins
        {}
    }

    /// Closes the finished work's worker, so its thread ends by itself, and keeps it until the next start()
    /// (or this) destroys it: the UI thread never joins a thread that is still finishing its bookkeeping.
    void retire() noexcept
    {
        static_cast<void>(m_Running->close()); // nothing is queued: its one job has run
        m_Retired = std::move(m_Running);
    }

    std::string_view m_ThreadName;
    FailedFn m_Failed;
    std::string_view m_UnknownDetail;
    std::future<Result> m_Future;              // an adopted answer (adopt()); a worker's result is its publication
    std::stop_source m_Stop{std::nostopstate}; // the work in flight's; a new one per start()
    bool m_Adopted = false;                    // m_Future is adopt()'s: answered elsewhere, dropped rather than waited for
    // Destroyed last to first, after ~BackgroundTask() waited for the work: the running worker stops and joins,
    // then the retired one, whose thread has ended.
    std::unique_ptr<TaskWorker> m_Retired; // the last work's worker, closed when its result was taken in
    std::unique_ptr<TaskWorker> m_Running; // the work in flight's, until poll() takes its result in
};

} // namespace App
