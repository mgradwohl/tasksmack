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
#include <cmath>
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
///
/// Nearest-rank definition: rank = ceil(percentile * n), 1-indexed, clamped to [1, n]. For
/// n=10 and p95, that's ceil(9.5) = 10 -> the 10th (last) of 10 sorted values, not the 9th --
/// a plain floor(p * (n - 1)) index (this function's original implementation) under-reports
/// the tail at small sample counts, which is exactly what p95/p99 exist to catch.
[[nodiscard]] inline double computePercentile(std::vector<double> samples, double percentile)
{
    if (samples.empty())
    {
        return 0.0;
    }
    const double clamped = std::clamp(percentile, 0.0, 1.0);
    const auto sampleCount = static_cast<double>(samples.size());
    auto rank1Indexed = static_cast<std::size_t>(std::ceil(clamped * sampleCount));
    rank1Indexed = std::clamp<std::size_t>(rank1Indexed, 1, samples.size());
    const std::size_t rank = rank1Indexed - 1;
    std::ranges::nth_element(samples, samples.begin() + static_cast<std::ptrdiff_t>(rank));
    return samples[rank];
}

/// Accumulates per-frame/per-event-batch timing stats. Most fields are per-interval
/// counters/sums reset by resetIntervalCounters() at each periodic log; the *SamplesMs vectors
/// are instead ROLLING windows (capped at PERCENTILE_WINDOW_SIZE, oldest dropped first) that
/// persist across those periodic resets, only cleared by a full `= {}` reset at an
/// idle<->interaction state transition. This split matters: nearest-rank percentiles over a
/// small sample count necessarily equal the max (n<100 forces p99==max; n<20 forces p95==max),
/// which would make every reported p99 statistically indistinguishable from max at the ~30
/// samples a 0.5s interaction-progress interval collects, or even the ~100 samples a 5s
/// idle-progress interval collects at a 20fps idle rate -- defeating the point of reporting a
/// percentile instead of just the max. Keeping the rolling window alive across same-state
/// periodic resets (while still clearing it at transitions, so idle and interaction data never
/// mix) lets it accumulate well past 100 samples over a few consecutive intervals of
/// continuous idle/interaction time, at which point p99 starts meaningfully differing from max.
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
    /// Sum/max of update+render+post+swap per frame (excludes drain, which is a separate
    /// per-loop-iteration accumulator, not always 1:1 with a rendered frame -- see
    /// computeSkipRenderThisFrame). This is the figure #843's "p99 frame time <= 16.6ms (60fps)"
    /// success criterion actually means: individual phase percentiles can each look fine while
    /// their sum still misses the frame budget.
    double totalFrameMs = 0.0;
    double maxTotalFrameMs = 0.0;
    /// Number of times P0 (drain budget cap) fired and broke the poll loop early.
    std::uint32_t p0BudgetCapHits = 0;
    /// Number of frames skipped by P3 (drain-overrun skip-render).
    std::uint32_t skippedRenderFrames = 0;
    /// Max wall time for any single 4-event budget-check interval inside the drain loop.
    /// A large value here indicates a single SDL_PollEvent call stalling (Wayland configure hold).
    double maxSinglePollBatchMs = 0.0;

    /// Rolling-window cap: comfortably past the n=100 threshold where nearest-rank p99 stops
    /// being forced to equal the max, without letting the window span so much wall-clock time
    /// that a stale sample from many seconds ago masks a genuine recent regression.
    static constexpr std::size_t PERCENTILE_WINDOW_SIZE = 200;

    std::vector<double> drainSamplesMs;
    std::vector<double> updateSamplesMs;
    std::vector<double> renderSamplesMs;
    std::vector<double> postRenderSamplesMs;
    std::vector<double> swapSamplesMs;
    std::vector<double> totalFrameSamplesMs;

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
        pushRollingSample(drainSamplesMs, durationMs);
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
        pushRollingSample(updateSamplesMs, updateDurationMs);
        pushRollingSample(renderSamplesMs, renderDurationMs);
        pushRollingSample(postRenderSamplesMs, postRenderDurationMs);
        pushRollingSample(swapSamplesMs, swapDurationMs);

        const double totalDurationMs = updateDurationMs + renderDurationMs + postRenderDurationMs + swapDurationMs;
        totalFrameMs += totalDurationMs;
        maxTotalFrameMs = std::max(maxTotalFrameMs, totalDurationMs);
        pushRollingSample(totalFrameSamplesMs, totalDurationMs);
    }

    [[nodiscard]] bool hasSamples() const noexcept
    {
        return (eventBatches > 0) || (frames > 0);
    }

    /// Resets the per-interval scalar counters/sums that logResizePerfTraceSummary() reports
    /// as avg/max (i.e. everything above except the rolling *SamplesMs windows), so avg/max
    /// still describe "since the last log", the same as before this rolling-window change. The
    /// rolling windows themselves are deliberately left untouched -- see the class comment.
    /// Call this at each periodic progress log; use `*this = {}` instead at an actual
    /// idle<->interaction transition, which should also clear the rolling windows.
    void resetIntervalCounters() noexcept
    {
        eventBatches = 0;
        frames = 0;
        resizeFrames = 0;
        drainedEvents = 0;
        resizeEvents = 0;
        maxEventsPerBatch = 0;
        drainMs = 0.0;
        updateMs = 0.0;
        renderMs = 0.0;
        postRenderMs = 0.0;
        swapMs = 0.0;
        maxDrainMs = 0.0;
        maxUpdateMs = 0.0;
        maxRenderMs = 0.0;
        maxPostRenderMs = 0.0;
        maxSwapMs = 0.0;
        totalFrameMs = 0.0;
        maxTotalFrameMs = 0.0;
        p0BudgetCapHits = 0;
        skippedRenderFrames = 0;
        maxSinglePollBatchMs = 0.0;
    }

  private:
    static void pushRollingSample(std::vector<double>& buffer, double value)
    {
        buffer.push_back(value);
        if (buffer.size() > PERCENTILE_WINDOW_SIZE)
        {
            buffer.erase(buffer.begin());
        }
    }
};

/// Logs one summary line: avg/max figures describe the interval since the last
/// resetIntervalCounters()/`= {}` reset, while p95/p99 are computed over the rolling
/// PERCENTILE_WINDOW_SIZE-sample window, which can span several such intervals within the same
/// idle/interaction state (see the class comment on ResizePerfTraceStats). A no-op when no
/// samples have been recorded this interval (hasSamples() is false), so callers can invoke this
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
    // update+render+post+swap: the figure #843's "p99 frame time <= 16.6ms (60fps)" success
    // criterion actually refers to -- per-phase percentiles can each look fine individually
    // while their sum still misses the frame budget.
    const double totalP95 = computePercentile(stats.totalFrameSamplesMs, 0.95);
    const double totalP99 = computePercentile(stats.totalFrameSamplesMs, 0.99);

    spdlog::info("ResizePerf[{}]: batches={} events={} resizeEvents={} maxBatchEvents={} "
                 "p0Hits={} skippedFrames={} maxPollBatch={:.3f} ms "
                 "frames={} resizeFrames={} frame avg/p95/p99/max={:.3f}/{:.3f}/{:.3f}/{:.3f} ms "
                 "drain avg/p95/p99/max={:.3f}/{:.3f}/{:.3f}/{:.3f} ms "
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
                 avg(stats.totalFrameMs, stats.frames),
                 totalP95,
                 totalP99,
                 stats.maxTotalFrameMs,
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
