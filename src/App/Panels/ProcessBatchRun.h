#pragma once

// A confirmed batch action or batch priority change (#804, #1484), run on a worker rather than in the
// frame that confirmed it (#2142). Ctrl+A can select thousands of processes, and each one costs the
// platform's identity check and the action itself (on Linux a priority change renices every thread),
// so a run over all of them on the UI thread froze the window for as long as it took, with nothing to
// say why. Here start*() returns at once; the toolbar shows statusText() ("Kill: 1200 of 5000
// processes...") while busy(), and poll() takes the one-line result in on a later frame.
//
// One batch at a time: the panel offers no other process action from its row menu, and F9 does
// nothing, while one runs. The worker is a BackgroundTask (#2139): its exception or a thread that could
// not start becomes the Failed outcome, and destroying this (or shutdown()) stops the run before its
// next target and waits for the one in flight. The IProcessActions passed to start*() must outlive
// the run: ProcessesPanel declares this after its actions, so it is destroyed first.
//
// Free of ImGui; tested with fake IProcessActions in tests/App/test_ProcessBatchRun.cpp.

#include "App/Panels/BackgroundTask.h"
#include "App/Panels/ProcessBatchAction.h"
#include "App/Panels/ProcessDetailsPanel_ActionHelpers.h"
#include "Platform/IProcessActions.h"
#include "Platform/ThreadName.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace App::ProcessBatch
{

/// What a finished batch run leaves for the toolbar: its one-line result, and whether any process was
/// acted on (the panel then refreshes at once, to show it).
struct BatchRunOutcome
{
    Detail::ActionResultMessage message;
    bool anySucceeded = false;
};

/// The outcome of a run that threw, or for which no thread could be started.
[[nodiscard]] inline BatchRunOutcome batchRunFailed(std::string detail)
{
    return {.message = {.ok = false, .text = std::format("Could not run the batch: {}", detail)}, .anySucceeded = false};
}

/// The toolbar's line while a batch runs: "Kill: 1200 of 5000 processes...".
[[nodiscard]] inline std::string batchProgressText(std::string_view label, std::size_t done, std::size_t total)
{
    return std::format("{}: {} of {} processes...", label, done, total);
}

/// The label batchProgressText() gives a batch priority change.
inline constexpr std::string_view SET_PRIORITY_PROGRESS_LABEL = "Set priority";

class BatchRun
{
  public:
    BatchRun() = default;
    ~BatchRun() = default; // m_Task's destructor stops the run and waits for it
    BatchRun(const BatchRun&) = delete;
    BatchRun& operator=(const BatchRun&) = delete;
    BatchRun(BatchRun&&) = delete;
    BatchRun& operator=(BatchRun&&) = delete;

    /// Starts @p action on every target, through @p actions, on the worker (runBatchAction()).
    /// @return The Failed outcome when no thread could be started; nullopt when it started, or when a
    ///         batch was already running (nothing new starts then: check busy() first).
    [[nodiscard]] std::optional<BatchRunOutcome>
    startAction(Platform::IProcessActions& actions, Detail::ProcessAction action, std::vector<BatchTarget> targets, std::int32_t ownPid)
    {
        if (busy())
        {
            return std::nullopt;
        }
        begin(Detail::actionLabel(action), targets.size());
        return m_Task.start(
            [actions = &actions, action, targets = std::move(targets), ownPid, done = m_Done](const std::stop_token& stopToken)
            {
                const BatchResult result = runBatchAction(*actions, action, targets, ownPid, stopToken, done.get());
                return BatchRunOutcome{.message = formatBatchResultMessage(action, result), .anySucceeded = result.succeeded > 0};
            });
    }

    /// Sets @p nice on every target, through @p actions, on the worker (runBatchPriority()). Returns as
    /// startAction() does.
    [[nodiscard]] std::optional<BatchRunOutcome>
    startPriority(Platform::IProcessActions& actions, std::vector<BatchTarget> targets, std::int32_t nice, std::int32_t ownPid)
    {
        if (busy())
        {
            return std::nullopt;
        }
        begin(SET_PRIORITY_PROGRESS_LABEL, targets.size());
        return m_Task.start(
            [actions = &actions, targets = std::move(targets), nice, ownPid, done = m_Done](const std::stop_token& stopToken)
            {
                const BatchResult result = runBatchPriority(*actions, targets, nice, ownPid, stopToken, done.get());
                return BatchRunOutcome{.message = formatBatchPriorityResultMessage(nice, result), .anySucceeded = result.succeeded > 0};
            });
    }

    /// Whether a batch was started and its outcome not taken in yet.
    [[nodiscard]] bool busy() const noexcept
    {
        return m_Task.busy();
    }

    /// The progress line for the toolbar while busy(); rebuilt only when the count has moved.
    [[nodiscard]] const std::string& statusText()
    {
        const std::size_t done = m_Done->load(std::memory_order_relaxed);
        if (done != m_ShownDone)
        {
            m_ShownDone = done;
            m_StatusText = batchProgressText(m_Label, done, m_Total);
        }
        return m_StatusText;
    }

    /// The outcome once the batch has finished (or, with @p wait, after waiting for it); nullopt while
    /// none is running or it is still running. Never rethrows (BackgroundTask::poll()).
    [[nodiscard]] std::optional<BatchRunOutcome> poll(bool wait = false)
    {
        return m_Task.poll(wait);
    }

    /// Stops the run before its next target and waits for the one in flight; its outcome, if not taken
    /// in, is left for poll().
    void shutdown() noexcept
    {
        m_Task.requestStop();
        m_Task.wait();
    }

  private:
    /// A new count for each run: the worker holds its own run's, never the next one's.
    void begin(std::string_view label, std::size_t total)
    {
        m_Done = std::make_shared<std::atomic<std::size_t>>(0);
        m_Label = label;
        m_Total = total;
        m_ShownDone = std::numeric_limits<std::size_t>::max();
    }

    std::shared_ptr<std::atomic<std::size_t>> m_Done = std::make_shared<std::atomic<std::size_t>>(0); // shared with the worker
    std::string m_Label;
    std::size_t m_Total = 0;
    std::size_t m_ShownDone = std::numeric_limits<std::size_t>::max(); // the count m_StatusText was built for
    std::string m_StatusText;
    // Last, so it is destroyed (stopping and waiting for the worker) before the members above.
    BackgroundTask<BatchRunOutcome> m_Task{Platform::BATCH_ACTION_THREAD_NAME, &batchRunFailed};
};

} // namespace App::ProcessBatch
