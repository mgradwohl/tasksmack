#pragma once

// One piece of work at a time on a worker thread of its own, its result taken in on the UI thread
// (#2139): the worker slot every App view that waits on a slow platform call shares -- a dump, a
// service or startup action, the System Information and crash history reads, the lazy Process Details
// reads. It owns the rules each of those sites used to restate:
//
//  - The work's exception is caught on the worker and turned into the view's Failed result there, so
//    the UI thread gets a plain Result through the future and never an exception object (#1685, #1706:
//    the message buffer of a std::runtime_error is reference-counted inside the C++ runtime, invisibly
//    to ThreadSanitizer).
//  - No thread to run it on (std::system_error from std::async) is the Failed result too, at once.
//  - poll() never rethrows: what can still come through the future (building the Failed result threw,
//    bad_alloc) is taken in as the Failed result with the unknown-error detail, unread.
//  - Each start() gets a new stop token; requestStop() is for work that polls it (a dump, a service
//    action, #1591). The destructor and move assignment request a stop, then wait.
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

#include "Platform/ThreadName.h"

#include <chrono>
#include <exception>
#include <future>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
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

    /// Work in flight moves with it: the worker shares only the future's and the stop token's state.
    BackgroundTask(BackgroundTask&& other) noexcept
        : m_ThreadName(other.m_ThreadName),
          m_Failed(other.m_Failed),
          m_UnknownDetail(other.m_UnknownDetail),
          m_Future(std::move(other.m_Future)),
          m_Stop(std::move(other.m_Stop)),
          m_Adopted(std::exchange(other.m_Adopted, false))
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
        }
        return *this;
    }

    /// Starts @p work on a worker named threadName, with a new stop token: @p work is invoked as
    /// work(std::stop_token) when it takes one, else as work(). It must own (copy, or share) everything
    /// it uses, or the caller keeps that alive until the work finishes (destroying this waits for it).
    /// Does nothing while busy(): the work in flight is never replaced or waited for here.
    /// @return The Failed result when no thread could be started; nullopt when the work started (or
    ///         another was still in flight).
    template<typename Work> [[nodiscard]] std::optional<Result> start(Work work)
    {
        if (busy())
        {
            return std::nullopt;
        }
        m_Stop = std::stop_source{}; // a stop of earlier work does not carry over
        m_Adopted = false;
        try
        {
            m_Future = std::async(std::launch::async,
                                  [work = std::move(work),
                                   stopToken = m_Stop.get_token(),
                                   name = m_ThreadName,
                                   failed = m_Failed,
                                   unknown = m_UnknownDetail] mutable
                                  {
                                      // Best effort: a std::async thread may come from a pool (MSVC) and keep the name.
                                      static_cast<void>(Platform::setCurrentThreadName(name));
                                      return runOrFailed(work, stopToken, failed, unknown);
                                  });
        }
        catch (const std::system_error& e)
        {
            return m_Failed(std::string(e.what()));
        }
        return std::nullopt;
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
        return m_Future.valid();
    }

    /// The result, once the work has finished (or, with @p wait, after waiting for it); nullopt while
    /// none is in flight or it is still running. Never rethrows: see the file comment.
    [[nodiscard]] std::optional<Result> poll(bool wait = false)
    {
        if (!m_Future.valid() || (!wait && m_Future.wait_for(std::chrono::seconds(0)) != std::future_status::ready))
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
            // The worker turns throwing work into the Failed result itself (runOrFailed()), so only
            // building that result can land here (bad_alloc), or a broken promise of an adopted answer.
            // The exception object is not read: it was thrown on another thread (#1685).
            return m_Failed(std::string(m_UnknownDetail));
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
            return;
        }
        try
        {
            if (m_Future.valid())
            {
                m_Future.wait();
            }
        }
        catch (...) // NOLINT(bugprone-empty-catch): wait() on a valid future does not throw
        {}
    }

  private:
    /// Runs on the worker: @p work, or the Failed result for work that threw, its message copied here on
    /// the thread that threw it. Only building the Failed result itself (bad_alloc) can still escape, to poll().
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

    std::string_view m_ThreadName;
    FailedFn m_Failed;
    std::string_view m_UnknownDetail;
    // A future from std::async waits for its thread when destroyed; ~BackgroundTask() waits explicitly
    // first, after requesting a stop.
    std::future<Result> m_Future;
    std::stop_source m_Stop{std::nostopstate}; // the work in flight's; a new one per start()
    bool m_Adopted = false;                    // m_Future is adopt()'s: answered elsewhere, dropped rather than waited for
};

} // namespace App
