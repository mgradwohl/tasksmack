#pragma once

// One single-process action at a time on a worker of its own (#2305): terminate, kill, suspend,
// resume, CPU affinity, a trace's launch. They ran on the UI thread in the frame that asked, which was
// quick while each was one syscall, but a password prompt (polkit, #2304) or a round trip to a
// privileged helper (#2181) would freeze the window for as long as either took. Here start() returns at
// once; the view shows pendingText() while busy(), and update() hands the result to the view's own
// finish on a later frame, on the UI thread, where it formats its result line as before.
//
// The work runs on a BackgroundTask (#2139): its exception, or a thread that could not start, becomes a
// failed ProcessActionResult. The IProcessActions the work calls must outlive this: the panels declare
// their actions before the views that hold one of these, so it is destroyed (and its worker waited for)
// first, as ProcessBatch::BatchRun is.
//
// Free of ImGui; tested in tests/App/test_ProcessActionRun.cpp.

#include "App/Panels/BackgroundTask.h"
#include "Platform/IProcessActions.h"
#include "Platform/ThreadName.h"

#include <functional>
#include <stop_token>
#include <string>
#include <utility>

namespace App
{

class ProcessActionRun
{
  public:
    /// Takes a finished action's result in, on the UI thread.
    using Finish = std::function<void(const Platform::ProcessActionResult&)>;

    /// Runs @p work (a callable returning a ProcessActionResult) on the worker, showing @p pendingText
    /// meanwhile; @p finish takes its result in at the update() that finds it done. Returns false, and
    /// runs nothing, while another action runs. When no thread can be started, @p finish gets the failed
    /// result at once.
    template<typename Work> bool start(std::string pendingText, Work work, Finish finish)
    {
        if (busy())
        {
            return false;
        }
        m_Pending = std::move(pendingText);
        m_Finish = std::move(finish);
        if (auto failed = m_Worker.start([work = std::move(work)](const std::stop_token& /*stopToken*/) mutable { return work(); }))
        {
            finishWith(*failed);
        }
        return true;
    }

    /// Each frame, on the UI thread: a finished action's result goes to its finish. True on that frame.
    /// @p wait blocks until the running action finishes (the tests' way to take it in deterministically).
    bool update(bool wait = false)
    {
        if (std::optional<Platform::ProcessActionResult> result = m_Worker.poll(wait))
        {
            finishWith(*result);
            return true;
        }
        return false;
    }

    [[nodiscard]] bool busy() const noexcept
    {
        return m_Worker.busy();
    }

    /// What the result line shows while busy(): "Terminating bash (PID 1234)...".
    [[nodiscard]] const std::string& pendingText() const noexcept
    {
        return m_Pending;
    }

  private:
    void finishWith(const Platform::ProcessActionResult& result)
    {
        const Finish finish = std::move(m_Finish);
        m_Finish = nullptr;
        m_Pending.clear();
        if (finish)
        {
            finish(result);
        }
    }

    [[nodiscard]] static Platform::ProcessActionResult failed(std::string detail)
    {
        return Platform::ProcessActionResult::error(std::move(detail));
    }

    std::string m_Pending;
    Finish m_Finish;
    // Last, so it is destroyed (stopping and waiting for the worker) before the members above.
    BackgroundTask<Platform::ProcessActionResult> m_Worker{Platform::PROCESS_ACTION_THREAD_NAME, &ProcessActionRun::failed};
};

} // namespace App
