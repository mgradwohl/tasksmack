#pragma once

// Pure data/logging extracted from Application::run()'s optional resize-performance tracing
// (enabled via the TASKSMACK_TRACE_RESIZE_PERF env var), so it can be unit-tested directly
// instead of only indirectly through a live SDL/OpenGL application instance. See
// CONTRIBUTING.md's "extract the pure decision logic into a small header" pattern (also used by
// Core/FramePacing.h, App/TitleBarGeometry.h).

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdint>
#include <string_view>

namespace Core
{

/// Accumulates per-frame/per-event-batch timing stats for one resize-trace logging interval.
/// All fields are plain counters/sums updated by recordEventBatch()/recordFrame(); the caller
/// (Application::run()) decides when to log a summary and reset.
struct ResizePerfTraceStats
{
    std::uint32_t eventBatches = 0;
    std::uint32_t frames = 0;
    std::uint32_t resizeFrames = 0;
    std::uint32_t drainedEvents = 0;
    std::uint32_t resizeEvents = 0;
    std::uint32_t maxEventsPerBatch = 0;
    double drainMs = 0.0;
    double updateMs = 0.0;
    double renderMs = 0.0;
    double postRenderMs = 0.0;
    double swapMs = 0.0;
    double maxDrainMs = 0.0;
    double maxUpdateMs = 0.0;
    double maxRenderMs = 0.0;
    double maxPostRenderMs = 0.0;
    double maxSwapMs = 0.0;
    /// Number of times P0 (drain budget cap) fired and broke the poll loop early.
    std::uint32_t p0BudgetCapHits = 0;
    /// Number of frames skipped by P3 (drain-overrun skip-render).
    std::uint32_t skippedRenderFrames = 0;
    /// Max wall time for any single 4-event budget-check interval inside the drain loop.
    /// A large value here indicates a single SDL_PollEvent call stalling (Wayland configure hold).
    double maxSinglePollBatchMs = 0.0;

    void recordEventBatch(
        std::uint32_t eventCount, std::uint32_t resizeEventCount, double durationMs, double singlePollBatchMs, bool p0Fired) noexcept
    {
        ++eventBatches;
        drainedEvents += eventCount;
        resizeEvents += resizeEventCount;
        maxEventsPerBatch = std::max(maxEventsPerBatch, eventCount);
        drainMs += durationMs;
        maxDrainMs = std::max(maxDrainMs, durationMs);
        maxSinglePollBatchMs = std::max(maxSinglePollBatchMs, singlePollBatchMs);
        if (p0Fired)
        {
            ++p0BudgetCapHits;
        }
    }

    void recordFrame(
        bool wasResizeFrame, double updateDurationMs, double renderDurationMs, double postRenderDurationMs, double swapDurationMs) noexcept
    {
        ++frames;
        if (wasResizeFrame)
        {
            ++resizeFrames;
        }

        updateMs += updateDurationMs;
        renderMs += renderDurationMs;
        postRenderMs += postRenderDurationMs;
        swapMs += swapDurationMs;
        maxUpdateMs = std::max(maxUpdateMs, updateDurationMs);
        maxRenderMs = std::max(maxRenderMs, renderDurationMs);
        maxPostRenderMs = std::max(maxPostRenderMs, postRenderDurationMs);
        maxSwapMs = std::max(maxSwapMs, swapDurationMs);
    }

    [[nodiscard]] bool hasSamples() const noexcept
    {
        return (eventBatches > 0) || (frames > 0);
    }
};

/// Logs one summary line for the stats accumulated since the last reset. A no-op when no
/// samples have been recorded (hasSamples() is false), so callers can invoke this
/// unconditionally at interaction boundaries and shutdown.
inline void logResizePerfTraceSummary(const ResizePerfTraceStats& stats, const std::string_view reason)
{
    if (!stats.hasSamples())
    {
        return;
    }

    const auto avg = [](const double total, const std::uint32_t count) noexcept -> double
    {
        return (count == 0) ? 0.0 : (total / static_cast<double>(count));
    };

    spdlog::info("ResizePerf[{}]: batches={} events={} resizeEvents={} maxBatchEvents={} "
                 "p0Hits={} skippedFrames={} maxPollBatch={:.3f} ms "
                 "frames={} resizeFrames={} drain avg/max={:.3f}/{:.3f} ms "
                 "update avg/max={:.3f}/{:.3f} ms render avg/max={:.3f}/{:.3f} ms "
                 "post avg/max={:.3f}/{:.3f} ms swap avg/max={:.3f}/{:.3f} ms",
                 reason,
                 stats.eventBatches,
                 stats.drainedEvents,
                 stats.resizeEvents,
                 stats.maxEventsPerBatch,
                 stats.p0BudgetCapHits,
                 stats.skippedRenderFrames,
                 stats.maxSinglePollBatchMs,
                 stats.frames,
                 stats.resizeFrames,
                 avg(stats.drainMs, stats.eventBatches),
                 stats.maxDrainMs,
                 avg(stats.updateMs, stats.frames),
                 stats.maxUpdateMs,
                 avg(stats.renderMs, stats.frames),
                 stats.maxRenderMs,
                 avg(stats.postRenderMs, stats.frames),
                 stats.maxPostRenderMs,
                 avg(stats.swapMs, stats.frames),
                 stats.maxSwapMs);
}

} // namespace Core
