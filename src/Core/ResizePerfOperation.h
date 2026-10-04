#pragma once

#include "Core/EnvUtils.h"
#include "Core/ResizePerfTrace.h"

#include <SDL3/SDL.h>
#include <spdlog/spdlog.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

namespace Core
{

enum class ResizePerfOperation : std::uint8_t
{
    SizeCommit,
    FinalSizeCommit,
    WindowSize,
    WindowSync,
    Viewport,
    ImGuiFinalize,
    OpenGLSubmit,
    SwapInterval,
    Count
};

inline constexpr std::array<std::string_view, static_cast<std::size_t>(ResizePerfOperation::Count)> RESIZE_PERF_OPERATION_NAMES{
    "size-commit", "final-size-commit", "window-size", "window-sync", "viewport", "imgui-finalize", "opengl-submit", "swap-interval"};

/// UI-thread-only diagnostics. No clock reads, allocations or logging on the disabled path.
struct ResizePerfOperations
{
    bool enabled = isEnvFlagEnabled(SDL_getenv("TASKSMACK_TRACE_RESIZE_PERF"));
    std::array<ResizePerfDurationStats, static_cast<std::size_t>(ResizePerfOperation::Count)> durations{};
    std::array<std::uint64_t, static_cast<std::size_t>(ResizePerfOperation::Count)> failures{};
    std::array<std::pair<int, int>, static_cast<std::size_t>(ResizePerfOperation::Count)> maxRequests{};
    ResizePerfDurationStats loops;
    ResizePerfDurationStats frameGaps;
    std::uint64_t previousFrameEnd = 0;
};

[[nodiscard]] inline ResizePerfOperations& resizePerfOperations()
{
    static thread_local ResizePerfOperations state;
    return state;
}

[[nodiscard]] inline double resizePerfElapsedMs(std::uint64_t start, std::uint64_t end)
{
    return (static_cast<double>(end - start) * 1000.0) / static_cast<double>(SDL_GetPerformanceFrequency());
}

/// How an operation's optional success flag reads in the resize trace log.
[[nodiscard]] inline const char* queriedResultText(std::optional<bool> result) noexcept
{
    if (!result.has_value())
    {
        return "not-queried";
    }
    return *result ? "true" : "false";
}

inline void recordResizePerfOperation(
    ResizePerfOperation operation, std::uint64_t start, std::uint64_t end, std::optional<bool> result, int requestedA, int requestedB)
{
    auto& trace = resizePerfOperations();
    const auto index = static_cast<std::size_t>(operation);
    const double durationMs = resizePerfElapsedMs(start, end);
    if (trace.durations[index].count == 0 || durationMs > trace.durations[index].maxMs)
    {
        trace.maxRequests[index] = {requestedA, requestedB};
    }
    trace.durations[index].record(durationMs);
    const bool failed = result.has_value() && !*result;
    if (failed)
    {
        ++trace.failures[index];
    }
    if (failed || durationMs > 100.0)
    {
        // Read SDL's error only on failure, before another SDL operation can replace it.
        spdlog::info("ResizePerfOperation: op={} beginCounter={} endCounter={} duration={:.3f} ms requestedA={} requestedB={} "
                     "result={} error='{}'",
                     RESIZE_PERF_OPERATION_NAMES[index],
                     start,
                     end,
                     durationMs,
                     requestedA,
                     requestedB,
                     queriedResultText(result),
                     failed ? SDL_GetError() : "");
    }
}

template<typename Call> bool traceResizePerfSDL(ResizePerfOperation operation, int requestedA, int requestedB, Call&& call)
{
    if (!resizePerfOperations().enabled)
    {
        return std::forward<Call>(call)();
    }
    const auto start = SDL_GetPerformanceCounter();
    const bool success = std::forward<Call>(call)();
    const auto end = SDL_GetPerformanceCounter();
    recordResizePerfOperation(operation, start, end, success, requestedA, requestedB);
    return success;
}

template<typename Call> void traceResizePerfVoid(ResizePerfOperation operation, int requestedA, int requestedB, Call&& call)
{
    if (!resizePerfOperations().enabled)
    {
        std::forward<Call>(call)();
        return;
    }
    const auto start = SDL_GetPerformanceCounter();
    std::forward<Call>(call)();
    const auto end = SDL_GetPerformanceCounter();
    // Void GL/ImGui calls have no status result. Do not insert glGetError or GPU synchronization.
    recordResizePerfOperation(operation, start, end, std::nullopt, requestedA, requestedB);
}

inline void recordResizePerfFrameEnd(std::uint64_t end)
{
    auto& trace = resizePerfOperations();
    if (trace.previousFrameEnd != 0)
    {
        const double gapMs = resizePerfElapsedMs(trace.previousFrameEnd, end);
        trace.frameGaps.record(gapMs);
        if (gapMs > 100.0)
        {
            spdlog::info("ResizePerfFrameGap: beginCounter={} endCounter={} duration={:.3f} ms", trace.previousFrameEnd, end, gapMs);
        }
    }
    trace.previousFrameEnd = end;
}

inline void logResizePerfOperationSummary()
{
    const auto& trace = resizePerfOperations();
    for (std::size_t i = 0; i < trace.durations.size(); ++i)
    {
        const auto& stats = trace.durations[i];
        spdlog::info("ResizePerfOperations[{}]: count={} max={:.3f} ms over100={} over250={} failures={} "
                     "maxRequestedA={} maxRequestedB={}",
                     RESIZE_PERF_OPERATION_NAMES[i],
                     stats.count,
                     stats.maxMs,
                     stats.over100,
                     stats.over250,
                     trace.failures[i],
                     trace.maxRequests[i].first,
                     trace.maxRequests[i].second);
    }
    spdlog::info("ResizePerfWallSummary: loops={} loopMax={:.3f} ms loopOver100={} loopOver250={} "
                 "frameGaps={} gapMax={:.3f} ms gapOver100={} gapOver250={}",
                 trace.loops.count,
                 trace.loops.maxMs,
                 trace.loops.over100,
                 trace.loops.over250,
                 trace.frameGaps.count,
                 trace.frameGaps.maxMs,
                 trace.frameGaps.over100,
                 trace.frameGaps.over250);
}

} // namespace Core
