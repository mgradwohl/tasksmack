#pragma once

// The lazy background read shared by Process Details' on-demand sections (Connections #799, Modules
// #802, Open files #183): one read at a time on a worker thread, started only while the section was
// drawn open on the last frame, once when it opens or the selection changes and then every refresh
// interval -- or, constructed ON_DEMAND (Open files), only after request() (a button), with no refresh.
// The view keeps its rows, sort and filter; this keeps the cadence, the worker and the staleness
// checks, so each section's update() is the same few lines:
//
//     if (auto result = m_Read.takeFinished(target, false)) applyResult(*result);
//     if (!m_Read.due(deltaSeconds) || reader == nullptr || !reader->hasX()) return false;
//     if (auto failed = m_Read.start(target, [reader](const auto& t) { return reader->readX(t); })) applyResult(*failed);
//
// Free of ImGui; tested in tests/App/test_LazyBackgroundRead.cpp.

#include "Platform/IProcessActions.h"
#include "Platform/ThreadName.h"

#include <chrono>
#include <cstdint>
#include <exception>
#include <future>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace App::Detail
{

/// @tparam Result The reader's result type.
template<typename Result> class LazyBackgroundRead
{
  public:
    /// Builds the Failed result shown for a read that threw, or one no thread could be started for.
    using FailedResultFn = Result (*)(std::string detail);

    /// The refreshMs that reads only on request(): once per request, never on a timer.
    static constexpr int ON_DEMAND = -1;

    /// @param refreshMs   The re-read interval while the section stays open, or ON_DEMAND.
    /// @param threadName  The worker's name (Platform/ThreadName.h).
    /// @param failed      Builds a Failed result from an exception's message.
    LazyBackgroundRead(int refreshMs, std::string_view threadName, FailedResultFn failed) noexcept
        : m_RefreshMs(refreshMs), m_ThreadName(threadName), m_Failed(failed)
    {}

    /// Records that render() drew the section open this frame.
    void markDrawnOpen() noexcept
    {
        m_DrawnOpen = true;
    }

    /// Whether a read is running on its worker.
    [[nodiscard]] bool inFlight() const noexcept
    {
        return m_Pending.valid();
    }

    /// ON_DEMAND: asks for one read, started by the next due() that finds the section drawn open.
    void request() noexcept
    {
        m_RequestPending = true;
    }

    /// ON_DEMAND: a read was asked for and has not been taken in or dropped yet (the view's "Scanning").
    [[nodiscard]] bool requestOutstanding() const noexcept
    {
        return m_RequestPending || (m_Pending.valid() && m_PendingGeneration == m_Generation);
    }

    /// A different process was selected: a read in flight is dropped when it arrives, and the next
    /// one is due as soon as the section is drawn open again.
    void reset() noexcept
    {
        ++m_Generation;
        m_HasRequested = false;
        m_RequestPending = false;
        m_SecondsSinceRequest = 0.0F;
        m_DrawnOpen = false; // the open frame was the previous process's
    }

    /// The read in flight, once it has finished (or, with @p wait, after waiting for it), when it is for
    /// this selection and @p target; nullopt otherwise. A read for another target without a selection
    /// change (the panel's target moved under it) makes the next read due at once.
    [[nodiscard]] std::optional<Result> takeFinished(const Platform::ProcessTarget& target, bool wait)
    {
        if (!m_Pending.valid() || (!wait && m_Pending.wait_for(std::chrono::seconds(0)) != std::future_status::ready))
        {
            return std::nullopt;
        }
        Result result;
        try
        {
            result = m_Pending.get();
        }
        catch (...)
        {
            // The worker turns a throwing read into the Failed result itself (start()), so only building
            // that result can land here (bad_alloc). The exception object is not read: it was thrown on
            // the worker, and its message must not be shared across threads (#1685).
            result = m_Failed(std::string(UNKNOWN_FAILURE));
        }
        if (m_PendingGeneration != m_Generation)
        {
            return std::nullopt;
        }
        if (m_PendingTarget != target)
        {
            m_HasRequested = false;
            m_RequestPending = m_RefreshMs == ON_DEMAND; // the scan asked for was of another target: run it again
            return std::nullopt;
        }
        return result;
    }

    /// Advances the clock by @p deltaSeconds and consumes the drawn-open flag. True when a read should
    /// start now: drawn open, none in flight, and none yet since the selection changed or the refresh
    /// interval has passed since the last started. The clock runs while closed, so a section reopened
    /// after the interval reads at once.
    [[nodiscard]] bool due(float deltaSeconds) noexcept
    {
        m_SecondsSinceRequest += deltaSeconds;
        const bool shownOpen = std::exchange(m_DrawnOpen, false);
        if (m_RefreshMs == ON_DEMAND)
        {
            if (!shownOpen)
            {
                // Closed: a request not started yet is withdrawn, and a read in flight is dropped when it arrives.
                m_RequestPending = false;
                m_Generation += m_Pending.valid() ? 1U : 0U;
                return false;
            }
            return !m_Pending.valid() && std::exchange(m_RequestPending, false);
        }
        if (!shownOpen || m_Pending.valid())
        {
            return false;
        }
        return !m_HasRequested || (m_SecondsSinceRequest * 1000.0F) >= static_cast<float>(m_RefreshMs);
    }

    /// Starts @p read(target) on a worker. The caller keeps what @p read uses alive until the read
    /// finishes (destroying this waits for it). @return The Failed result to show when no thread could
    /// be started (retried at the next refresh); nullopt when the read started.
    template<typename ReadFn> [[nodiscard]] std::optional<Result> start(const Platform::ProcessTarget& target, ReadFn read)
    {
        m_HasRequested = true;
        m_SecondsSinceRequest = 0.0F;
        m_PendingGeneration = m_Generation;
        m_PendingTarget = target;
        try
        {
            m_Pending = std::async(std::launch::async,
                                   [read = std::move(read), target, name = m_ThreadName, failed = m_Failed]
                                   {
                                       // Best effort: a std::async thread may come from a pool (MSVC) and keep the name.
                                       static_cast<void>(Platform::setCurrentThreadName(name));
                                       return readOrFailed(read, target, failed);
                                   });
        }
        catch (const std::system_error& e)
        {
            return m_Failed(e.what());
        }
        return std::nullopt;
    }

  private:
    /// The detail of a read that threw something other than a std::exception.
    static constexpr std::string_view UNKNOWN_FAILURE = "unknown error";

    /// Runs on the worker: @p read(target), or the Failed result for a read that threw. The exception
    /// is caught and its message copied here, on the thread that threw it, so the UI thread gets a
    /// plain Result through the future and never an exception object (#1685: the message buffer of a
    /// std::runtime_error is reference-counted inside the C++ runtime, invisibly to ThreadSanitizer).
    /// Only building the Failed result itself (bad_alloc) can still escape, to takeFinished().
    template<typename ReadFn>
    [[nodiscard]] static Result readOrFailed(const ReadFn& read, const Platform::ProcessTarget& target, FailedResultFn failed)
    {
        try
        {
            return read(target);
        }
        catch (const std::exception& e)
        {
            return failed(std::string(e.what()));
        }
        catch (...)
        {
            return failed(std::string(UNKNOWN_FAILURE));
        }
    }

    int m_RefreshMs;
    std::string_view m_ThreadName;
    FailedResultFn m_Failed;
    bool m_DrawnOpen = false;      // render() drew the section open since the last due()
    bool m_HasRequested = false;   // a read was started since the selection changed
    bool m_RequestPending = false; // ON_DEMAND: request() was called and no read started for it yet
    float m_SecondsSinceRequest = 0.0F;

    // A future from std::async waits for its thread when destroyed: destroying this waits for one read.
    std::future<Result> m_Pending;
    std::uint64_t m_Generation = 0; // bumped by each selection change
    std::uint64_t m_PendingGeneration = 0;
    Platform::ProcessTarget m_PendingTarget{};
};

} // namespace App::Detail
