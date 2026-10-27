#pragma once

// Starts and stops the opt-in UDP event trace (#1492) on a worker, never on the UI thread (#3155). On
// Windows a start sweeps stale sessions (QueryAllTraces, ControlTrace) before StartTrace, EnableTraceEx2
// and OpenTrace, and a stop joins the consumer thread inside ProcessTrace: any of them can take long
// enough to freeze a frame, and the join has no bound. The Settings toggle's Save only records what it
// wants here; the worker does the rest.
//
// One switch at a time (BackgroundTask, ts-udp-switch). The last wish wins: turned on, then off again
// while the start still runs, the stop runs once the start is done -- a switch still pending when Save
// is clicked again is superseded, never queued behind. A start or stop that is already the collector's
// state is harmless (start() returns Started without restarting; stop() of a stopped one does nothing),
// so the switch never needs to know what the start hook did.
//
//     m_UdpSwitch.request(m_UdpCollector, enabled);  // the toggle changed
//     if (auto outcome = m_UdpSwitch.update())       // each frame
//         log(*outcome);
//
// Free of ImGui; tested in tests/App/test_UdpTraceSwitch.cpp.

#include "App/Panels/BackgroundTask.h"
#include "Platform/IUdpTrafficCollector.h"
#include "Platform/ThreadName.h"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace App
{

class UdpTraceSwitch
{
  public:
    /// How a switch ended.
    struct Outcome
    {
        bool enabled = false; ///< A start (true) or a stop (false)
        /// What start() said; nullopt for a stop, or one that failed (error).
        std::optional<Platform::UdpCollectorStartResult> started;
        std::string error; ///< Why it failed: it threw, or no thread could be started; empty otherwise
    };

    /// @param threadName The worker's name (Platform/ThreadName.h); must outlive this.
    explicit UdpTraceSwitch(std::string_view threadName = Platform::UDP_TRACE_SWITCH_THREAD_NAME) noexcept
        : m_Task(threadName, &failedSwitch, SWITCH_FAILURE)
    {}

    /// The toggle asks for the trace on (@p enabled) or off, through @p collector (null: nothing to
    /// switch, and the wish is dropped). Started at once when no switch runs, else once it is done.
    /// Never waits.
    /// @return The Failed outcome when no thread could be started for it; nullopt otherwise.
    [[nodiscard]] std::optional<Outcome> request(std::shared_ptr<Platform::IUdpTrafficCollector> collector, bool enabled)
    {
        if (!collector)
        {
            return std::nullopt;
        }
        m_Collector = std::move(collector);
        m_Wanted = enabled;
        return startIfDue();
    }

    /// Takes in a switch that has finished (or, with @p wait, waits for it first), and starts the one
    /// asked for meanwhile. Call it each frame. @return The finished switch's outcome, once; else the
    /// Failed outcome of one that couldn't be started; nullopt when there is neither.
    [[nodiscard]] std::optional<Outcome> update(bool wait = false)
    {
        std::optional<Outcome> finished = m_Task.poll(wait);
        std::optional<Outcome> failedToStart = startIfDue();
        return finished.has_value() ? std::move(finished) : std::move(failedToStart);
    }

    /// Whether a switch is running, or one is asked for and not started yet.
    [[nodiscard]] bool pending() const noexcept
    {
        return m_Task.busy() || isDue();
    }

    /// Waits for a switch still running and drops what was asked for after it (a panel's detach, which
    /// then stops the collector itself). @return The outcome of the one waited for, if any.
    std::optional<Outcome> shutdown()
    {
        m_Wanted.reset();
        return m_Task.poll(/*wait=*/true);
    }

  private:
    /// The detail of a switch that threw something other than a std::exception.
    static constexpr std::string_view SWITCH_FAILURE = "unknown error";

    /// A switch that threw, or that no thread could be started for.
    [[nodiscard]] static Outcome failedSwitch(std::string detail)
    {
        return Outcome{.enabled = false, .started = std::nullopt, .error = std::move(detail)};
    }

    /// Whether a switch is asked for that isn't the last one started.
    [[nodiscard]] bool isDue() const noexcept
    {
        return m_Wanted.has_value() && m_Wanted != m_Started;
    }

    /// Starts the switch asked for, unless one runs or it is the one last started.
    [[nodiscard]] std::optional<Outcome> startIfDue()
    {
        // m_Wanted checked here, where the use below can see it (isDue() checks it too).
        if (!m_Wanted.has_value() || m_Task.busy() || !isDue() || !m_Collector)
        {
            return std::nullopt;
        }
        const bool enabled = m_Wanted.value_or(false);
        m_Started = enabled; // not retried every frame when no thread could be started
        // The worker shares the collector (the probe holds it too); its outcome comes back through the
        // future, which orders one switch after the last.
        std::optional<Outcome> failed = m_Task.start(
            [collector = m_Collector, enabled]
            {
                if (!enabled)
                {
                    collector->stop();
                    return Outcome{.enabled = false, .started = std::nullopt, .error = {}};
                }
                // Not elevated, or refused: the collector logs why, and the counters stay TCP only.
                return Outcome{.enabled = true, .started = collector->start(), .error = {}};
            });
        if (failed.has_value())
        {
            failed->enabled = enabled;
        }
        return failed;
    }

    std::shared_ptr<Platform::IUdpTrafficCollector> m_Collector; // the last request()'s
    std::optional<bool> m_Wanted;                                // what the toggle asks for; nullopt: nothing
    std::optional<bool> m_Started;                               // the last switch started
    // Destroying this waits for a switch still running; it owns a share of the collector.
    BackgroundTask<Outcome> m_Task;
};

} // namespace App
