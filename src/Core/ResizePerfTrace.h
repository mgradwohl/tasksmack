#pragma once

// Pure data/logging extracted from Application::run()'s optional performance tracing (enabled
// via the TASKSMACK_TRACE_RESIZE_PERF env var), so it can be unit-tested directly instead of
// only indirectly through a live SDL/OpenGL application instance. See CONTRIBUTING.md's
// "extract the pure decision logic into a small header" pattern (also used by
// Core/FramePacing.h, App/TitleBarGeometry.h).
//
// Despite the env var's resize-focused name (this instrumentation started as a resize-specific
// investigation), Application::run() now records these stats on every frame while tracing is
// enabled, not just during interaction (perf-plan #843 phase 0): idle/steady-state frames are
// logged periodically as "idle-progress", and interaction frames as "interaction-progress"/
// "interaction-end", using the same accumulator and the same p95/p99 figures below.

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace Core
{

/// Percentile (nearest-rank) of a set of samples, computed on a mutable copy since the
/// underlying partial sort (nth_element) reorders its input. Pure/allocation-only, so it's
/// cheap to call at log time (every 0.5-5s) even though it's not meant for the render hot path.
/// Returns 0.0 for an empty input; `percentile` is clamped to [0, 1] (e.g. 0.95 for p95).
[[nodiscard]] inline double computePercentile(std::vector<double> samples, double percentile)
{
    if (samples.empty())
    {
        return 0.0;
    }
    const double clamped = std::clamp(percentile, 0.0, 1.0);
    const auto rank = static_cast<std::size_t>(clamped * static_cast<double>(samples.size() - 1));
    std::ranges::nth_element(samples, samples.begin() + static_cast<std::ptrdiff_t>(rank));
    return samples[rank];
}

/// Accumulates per-frame/per-event-batch timing stats for one resize-trace logging interval.
/// All fields are plain counters/sums updated by recordEventBatch()/recordFrame(); the caller
/// (Application::run()) decides when to log a summary and reset. The *SamplesMs vectors back
/// the p95/p99 figures in logResizePerfTraceSummary() -- sums/maxes alone can't tell you
/// whether a frame budget was missed at the tail, only on average or at the single worst frame.
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

    std::vector<double> drainSamplesMs;
    std::vector<double> updateSamplesMs;
    std::vector<double> renderSamplesMs;
    std::vector<double> postRenderSamplesMs;
    std::vector<double> swapSamplesMs;

    void
    recordEventBatch(std::uint32_t eventCount, std::uint32_t resizeEventCount, double durationMs, double singlePollBatchMs, bool p0Fired)
    {
        ++eventBatches;
        drainedEvents += eventCount;
        resizeEvents += resizeEventCount;
        maxEventsPerBatch = std::max(maxEventsPerBatch, eventCount);
        drainMs += durationMs;
        maxDrainMs = std::max(maxDrainMs, durationMs);
        maxSinglePollBatchMs = std::max(maxSinglePollBatchMs, singlePollBatchMs);
        drainSamplesMs.push_back(durationMs);
        if (p0Fired)
        {
            ++p0BudgetCapHits;
        }
    }

    void
    recordFrame(bool wasResizeFrame, double updateDurationMs, double renderDurationMs, double postRenderDurationMs, double swapDurationMs)
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
        updateSamplesMs.push_back(updateDurationMs);
        renderSamplesMs.push_back(renderDurationMs);
        postRenderSamplesMs.push_back(postRenderDurationMs);
        swapSamplesMs.push_back(swapDurationMs);
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

    // p95/p99 answer "did we miss the frame budget at the tail", which avg/max can't: avg
    // hides a rare-but-real stall, and max is a single outlier that one slow sample can spike.
    const double drainP95 = computePercentile(stats.drainSamplesMs, 0.95);
    const double drainP99 = computePercentile(stats.drainSamplesMs, 0.99);
    const double updateP95 = computePercentile(stats.updateSamplesMs, 0.95);
    const double updateP99 = computePercentile(stats.updateSamplesMs, 0.99);
    const double renderP95 = computePercentile(stats.renderSamplesMs, 0.95);
    const double renderP99 = computePercentile(stats.renderSamplesMs, 0.99);
    const double postP95 = computePercentile(stats.postRenderSamplesMs, 0.95);
    const double postP99 = computePercentile(stats.postRenderSamplesMs, 0.99);
    const double swapP95 = computePercentile(stats.swapSamplesMs, 0.95);
    const double swapP99 = computePercentile(stats.swapSamplesMs, 0.99);

    spdlog::info("ResizePerf[{}]: batches={} events={} resizeEvents={} maxBatchEvents={} "
                 "p0Hits={} skippedFrames={} maxPollBatch={:.3f} ms "
                 "frames={} resizeFrames={} drain avg/p95/p99/max={:.3f}/{:.3f}/{:.3f}/{:.3f} ms "
                 "update avg/p95/p99/max={:.3f}/{:.3f}/{:.3f}/{:.3f} ms "
                 "render avg/p95/p99/max={:.3f}/{:.3f}/{:.3f}/{:.3f} ms "
                 "post avg/p95/p99/max={:.3f}/{:.3f}/{:.3f}/{:.3f} ms "
                 "swap avg/p95/p99/max={:.3f}/{:.3f}/{:.3f}/{:.3f} ms",
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
                 drainP95,
                 drainP99,
                 stats.maxDrainMs,
                 avg(stats.updateMs, stats.frames),
                 updateP95,
                 updateP99,
                 stats.maxUpdateMs,
                 avg(stats.renderMs, stats.frames),
                 renderP95,
                 renderP99,
                 stats.maxRenderMs,
                 avg(stats.postRenderMs, stats.frames),
                 postP95,
                 postP99,
                 stats.maxPostRenderMs,
                 avg(stats.swapMs, stats.frames),
                 swapP95,
                 swapP99,
                 stats.maxSwapMs);
}

} // namespace Core
