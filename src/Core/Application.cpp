#include "Application.h"

#include "Core/EnvUtils.h"
#include "Core/Event.h"
#include "Core/FramePacing.h"
#include "Core/Layer.h"
#include "Core/ResizePerfTrace.h"
#include "Core/VideoBackend.h"
#include "Core/Window.h"
#include "Core/WindowEvents.h"

#include <SDL3/SDL.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <ranges>
#include <ratio>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <format>

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace Core
{

std::unique_ptr<Application> Application::s_Instance = nullptr;

namespace
{
// Track stack-allocated Application for tests and fallback access.
// Uses std::reference_wrapper to avoid storing a raw pointer; cleared in the destructor on the same thread.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables) - intentionally mutable for state tracking
thread_local std::optional<std::reference_wrapper<Application>> g_StackApplicationInstance;

// Maximum delta time clamped in the render loop to avoid large jumps after stalls or resize pauses.
constexpr float MAX_DELTA_TIME = 0.1F;

// When no SDL events arrive, sleep this long before rendering the next frame.
// This limits the idle render rate to ~20 fps, reducing CPU usage when the display
// hasn't changed. Mouse movement and keyboard events wake the sleep immediately,
// so interactive frame rate is unaffected.
constexpr int IDLE_FRAME_SLEEP_MS = 50;

// When the window is minimized there is nothing visible to render, so the sleep
// is extended to ~5 fps. Any event (e.g. SDL_EVENT_WINDOW_RESTORED) wakes
// immediately, so restore latency is unaffected.
constexpr int MINIMIZED_FRAME_SLEEP_MS = 200;

// During interactive move/resize, event delivery can be bursty depending on
// compositor/window-manager behavior. Keep redraw active for a short grace
// window after each relevant window event so the framebuffer stays responsive
// without forcing continuous high-rate rendering when idle.
constexpr float INTERACTION_REDRAW_GRACE_SECONDS = 0.35F;
constexpr const char* RESIZE_PERF_TRACE_ENV = "TASKSMACK_TRACE_RESIZE_PERF";
constexpr float RESIZE_PERF_TRACE_LOG_INTERVAL_SECONDS = 0.5F;
constexpr int RESIZE_PERF_TRACE_TOP_LAYER_COUNT = 3;

// P0: Break the event drain loop if wall-clock drain exceeds this threshold.
// On Wayland, individual SDL_PollEvent calls can stall on compositor protocol
// (xdg_surface configure handshake). Capping drain limits per-frame stall time.
constexpr double DRAIN_BUDGET_MS = 8.0;

// P3: If total drain time exceeds a full 60 fps frame budget, skip rendering
// this frame. The event queue is fully processed; rendering catches up next frame.
constexpr double DRAIN_SKIP_RENDER_MS = 16.0;

// Slow-frame thresholds for per-phase logging during resize tracing.
// Compute phases (update, render, post) are CPU work we control; threshold is one
// 60 fps budget slice. Swap includes compositor hold time on Wayland, which routinely
// reaches 8-15 ms even on smooth frames — only log truly bad stalls there.
constexpr double RESIZE_PERF_TRACE_SLOW_COMPUTE_THRESHOLD_MS = 8.0;
constexpr double RESIZE_PERF_TRACE_SLOW_SWAP_THRESHOLD_MS = 30.0;

#ifndef _WIN32
[[nodiscard]] bool hasSafeOwnerOnlyPermissions(const std::filesystem::perms perms)
{
    // Require that group/other bits are entirely clear (no world/group access)
    // and that the owner has at least read, write, and execute so that the
    // directory is actually usable as an XDG runtime dir.
    constexpr auto forbidden = std::filesystem::perms::group_all | std::filesystem::perms::others_all;
    constexpr auto required = std::filesystem::perms::owner_read | std::filesystem::perms::owner_write | std::filesystem::perms::owner_exec;
    return ((perms & forbidden) == std::filesystem::perms::none) && ((perms & required) == required);
}

[[nodiscard]] bool isUsableRuntimeDir(const std::filesystem::path& path)
{
    std::error_code ec;
    const auto symlink = std::filesystem::symlink_status(path, ec);
    if (ec)
    {
        return false;
    }
    if (std::filesystem::is_symlink(symlink) || !std::filesystem::is_directory(symlink))
    {
        return false;
    }

    const auto status = std::filesystem::status(path, ec);
    if (ec)
    {
        return false;
    }

    struct stat st{};
    if (::stat(path.c_str(), &st) != 0)
    {
        return false;
    }

    return (st.st_uid == getuid()) && hasSafeOwnerOnlyPermissions(status.permissions());
}

[[nodiscard]] std::optional<std::string> chooseRuntimeDir(const std::string& systemdPath, const std::string& fallbackPath)
{
    std::error_code ec;

    if (std::filesystem::exists(systemdPath, ec) && !ec && isUsableRuntimeDir(systemdPath))
    {
        return systemdPath;
    }

    // Guard against symlink and non-directory attacks on predictable /tmp path.
    const auto fallbackSymlink = std::filesystem::symlink_status(fallbackPath, ec);
    if (!ec && std::filesystem::exists(fallbackPath, ec))
    {
        if (std::filesystem::is_symlink(fallbackSymlink) || !std::filesystem::is_directory(fallbackSymlink))
        {
            spdlog::warn("Refusing unsafe XDG fallback path '{}': expected a real directory, not symlink/non-directory", fallbackPath);
            return std::nullopt;
        }
    }

    std::filesystem::create_directories(fallbackPath, ec);
    if (ec)
    {
        spdlog::warn("XDG_RUNTIME_DIR not set and could not create fallback '{}': {}", fallbackPath, ec.message());
        return std::nullopt;
    }

    // Guard against applying permissions to a directory we don't own (e.g. on
    // multi-user systems the predictable /tmp path may have been created by a
    // different UID). Chmod on an unowned directory is a side-effect we avoid.
    struct stat stFallback{};
    if (::stat(fallbackPath.c_str(), &stFallback) != 0 || stFallback.st_uid != getuid())
    {
        spdlog::warn("XDG fallback path '{}' is not owned by current user; cannot use it", fallbackPath);
        return std::nullopt;
    }

    // Re-validate with lstat (symlink_status) immediately before chmod to close
    // the TOCTOU window between the ownership check and the permission change.
    // An attacker could swap the directory for a symlink in that window.
    const auto preChmodStatus = std::filesystem::symlink_status(fallbackPath, ec);
    if (ec || std::filesystem::is_symlink(preChmodStatus) || !std::filesystem::is_directory(preChmodStatus))
    {
        spdlog::warn("XDG fallback path '{}' changed to a symlink or non-directory before chmod; aborting", fallbackPath);
        return std::nullopt;
    }

    std::filesystem::permissions(fallbackPath, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace, ec);
    if (ec)
    {
        spdlog::warn("Could not set permissions on '{}': {}", fallbackPath, ec.message());
        return std::nullopt;
    }

    if (!isUsableRuntimeDir(fallbackPath))
    {
        spdlog::warn("XDG fallback path '{}' is not usable after creation", fallbackPath);
        return std::nullopt;
    }

    return fallbackPath;
}

// Ensure XDG_RUNTIME_DIR is set before SDL initializes. When tasksmack is run
// as root (e.g. via sudo) the session manager never sets this variable, causing
// libwayland-client to emit "XDG_RUNTIME_DIR is invalid or not set" and SDL to
// fall back from Wayland to X11. We prefer X11 anyway in that case, but the
// warning is noisy and confusing. Setting a valid directory silences it.
// The standard location /run/user/<uid> is used if it exists; otherwise a
// per-user directory under /tmp is created with mode 0700 (required by the XDG
// spec so that only the owner can read/write the runtime files).
void ensureXdgRuntimeDir()
{
    if (const char* existing = SDL_getenv("XDG_RUNTIME_DIR"); existing != nullptr)
    {
        const std::string existingPath(existing);
        if (!existingPath.empty() && isUsableRuntimeDir(existingPath))
        {
            return; // valid path already provided by session manager
        }

        spdlog::warn("Ignoring invalid XDG_RUNTIME_DIR='{}'; using a safe fallback", existingPath);
    }

    const uid_t uid = getuid();
    const std::string systemdPath = std::format("/run/user/{}", uid);
    const std::string fallbackPath = std::format("/tmp/runtime-{}", uid);

    const auto chosen = chooseRuntimeDir(systemdPath, fallbackPath);
    if (!chosen.has_value())
    {
        return;
    }

    // setenv is POSIX; SDL_setenv would also work but setenv keeps it in the
    // real process environment so child processes inherit it correctly.
    // NOLINTNEXTLINE(concurrency-mt-unsafe,misc-include-cleaner) - called once before any threads start; setenv is provided by <cstdlib> (already included)
    if (::setenv("XDG_RUNTIME_DIR", chosen->c_str(), 1) != 0)
    {
        // NOLINTNEXTLINE(concurrency-mt-unsafe) - called once before any threads start, same as setenv() above
        spdlog::warn("Failed to set XDG_RUNTIME_DIR='{}': {}", *chosen, std::strerror(errno));
        return;
    }
    spdlog::info("Using XDG_RUNTIME_DIR='{}'", *chosen);
}
#endif

// A layer that throws is caught and logged here rather than left to unwind out of the main
// loop: for a long-running monitor, one bad frame or one bad event should not call
// std::terminate() and take down the whole process. Note this is a best-effort mitigation,
// not a full guarantee: layers pair raw ImGui::Begin()/End() calls (not RAII), so an exception
// thrown between them still leaves ImGui's window stack unbalanced for the rest of this frame;
// the assertion ImGui uses to detect that is compiled out in NDEBUG (Release) builds but active
// in Debug. Shared by renderFrame(), raiseEvent(), and the SDL event passthrough loop in run()
// so every per-frame/per-event layer callback degrades the same way instead of only some of
// them (#778).
template<typename Call> void guardLayerCall(const std::unique_ptr<Layer>& layer, std::string_view phase, const Call& call)
{
    try
    {
        call();
    }
    catch (const std::exception& e)
    {
        // The logging call itself can allocate (message formatting) and thus throw under the
        // same OOM condition this guard exists for; catching it here too keeps this whole
        // handler non-throwing so it can never re-escape and defeat the guard's entire purpose.
        try
        {
            spdlog::error("Layer '{}' threw during {}: {}", layer->getName(), phase, e.what());
        }
        catch (...) // NOLINT(bugprone-empty-catch) -- intentional: logging is best-effort here,
                    // must not throw
        {}
    }
    catch (...)
    {
        try
        {
            spdlog::error("Layer '{}' threw an unknown exception during {}", layer->getName(), phase);
        }
        catch (...) // NOLINT(bugprone-empty-catch) -- intentional: logging is best-effort here,
                    // must not throw
        {}
    }
}

} // namespace

Application::Application(ApplicationSpecification spec) : m_Spec(std::move(spec))
{
    // For stack-allocated instances (tests), track in thread-local.
    // Only set if neither ownership mechanism is already active.
    const bool singletonSetHere = !s_Instance && !g_StackApplicationInstance.has_value();
    if (singletonSetHere)
    {
        g_StackApplicationInstance = std::ref(*this);
    }

    bool sdlInitialized = false;
    try
    {
        m_ResizePerfTraceEnabled = isEnvFlagEnabled(SDL_getenv(RESIZE_PERF_TRACE_ENV));
        spdlog::info("Initializing {} application", m_Spec.Name);
        if (m_ResizePerfTraceEnabled)
        {
            spdlog::info("Resize performance tracing enabled via {}", RESIZE_PERF_TRACE_ENV);
        }

        // Validate window dimensions; fall back to a sensible default rather than
        // passing zero to the windowing system, which would produce undefined behavior.
        constexpr int DEFAULT_WIDTH = 1280;
        constexpr int DEFAULT_HEIGHT = 720;
        if (m_Spec.Width <= 0 || m_Spec.Height <= 0)
        {
            spdlog::warn(
                "Invalid window dimensions {}x{}, using defaults {}x{}", m_Spec.Width, m_Spec.Height, DEFAULT_WIDTH, DEFAULT_HEIGHT);
            m_Spec.Width = DEFAULT_WIDTH;
            m_Spec.Height = DEFAULT_HEIGHT;
        }

        // Initialize SDL video subsystem
#ifndef _WIN32
        ensureXdgRuntimeDir();
#endif
        if (!SDL_Init(SDL_INIT_VIDEO))
        {
            spdlog::critical("Failed to initialize SDL: {}", SDL_GetError());
            throw std::runtime_error("Failed to initialize SDL");
        }
        sdlInitialized = true;

        spdlog::info("SDL initialized: {}", SDL_GetRevision());

        // Initialize video backend detection (must happen after SDL_Init)
        VideoBackend::initialize();

        WindowSpecification windowSpec;
        windowSpec.Title = m_Spec.Name;
        windowSpec.Width = m_Spec.Width;
        windowSpec.Height = m_Spec.Height;
        windowSpec.VSync = m_Spec.VSync;
        // Custom title bar by default everywhere; opt-in escape hatch to native OS/compositor
        // decorations on native Wayland only (#745). VideoBackend::initialize() above must run
        // before this check. Decision logic lives in the pure, unit-tested
        // VideoBackend::shouldUseBorderlessTitleBar().
        windowSpec.Borderless =
            VideoBackend::shouldUseBorderlessTitleBar(m_Spec.ForceNativeDecorationsOnWayland, VideoBackend::isWayland());

        m_Window = std::make_unique<Window>(windowSpec);
    }
    catch (...)
    {
        // If construction fails, clear the singleton reference we set above so
        // subsequent tests do not observe a dangling reference to the partially-
        // constructed (and already-unwound) Application object.
        if (singletonSetHere)
        {
            g_StackApplicationInstance.reset();
        }
        // Undo SDL initialization if it succeeded but Window construction failed.
        // The destructor never runs for a failed construction, so we must balance
        // SDL_Init() here to avoid leaking global SDL state into later tests.
        if (sdlInitialized)
        {
            SDL_Quit();
        }
        throw;
    }
}

Application::~Application()
{
    // Note: Layers should have been detached via detachAllLayers() before destruction.
    // This cleanup is defensive - clear any remaining layers silently.
    // If layers weren't properly detached, their onDetach() may fail trying to access
    // the Application singleton (which is being destroyed).
    for (auto& layer : std::views::reverse(m_LayerStack))
    {
        layer->onDetach();
    }
    m_LayerStack.clear();
    m_Window.reset();

    SDL_Quit();

    // Clear thread-local reference if this was a stack-allocated instance
    // Note: Assumes Application is destroyed on the same thread it was created (SDL requirement)
    if (g_StackApplicationInstance.has_value() && &g_StackApplicationInstance->get() == this)
    {
        g_StackApplicationInstance.reset();
    }

    // Note: No need to reset s_Instance here - it's either:
    //   1. Being destroyed by the unique_ptr that owns us (will be nulled after this returns)
    //   2. We're stack-allocated (s_Instance doesn't point to us)
    //   3. Being explicitly cleared via setInstance(nullptr) (already null)
    // Attempting to check s_Instance.get() == this would cause issues in case 1.
}

void Application::detachAllLayers()
{
    // Detach layers in reverse order (topmost first)
    for (auto& layer : std::views::reverse(m_LayerStack))
    {
        layer->onDetach();
    }
    m_LayerStack.clear();
}

void Application::run()
{
    m_Running = true;

    float lastTime = getTime();

    const auto computeDeltaTime = [&lastTime]() -> float
    {
        const float currentTime = getTime();
        const float deltaTime = std::min(currentTime - lastTime, MAX_DELTA_TIME);
        lastTime = currentTime;
        return deltaTime;
    };

    m_InteractionRedrawUntil = 0.0F;

    ResizePerfTraceStats resizeTraceStats;
    bool wasTracingInteraction = false;
    bool wasInteracting = false;
    float lastResizeTraceLogTime = getTime();
    // Snapshot of whether TitleBarLayer changed window geometry (position/size) during
    // the PREVIOUS frame's onUpdate. Used to gate grace-period sleep: if no geometry
    // changed last frame, we allow the idle sleep even inside the interaction grace window,
    // preventing wasted renders when the window is stationary post-interaction.
    bool geometryChangedLastFrame = false;

    spdlog::info("Entering main loop");

    while (m_Running)
    {
        // Process SDL events
        bool hadEvents = false;
        bool needsResizeRedraw = false;
        std::uint32_t drainedEventCount = 0;
        std::uint32_t resizeEventCount = 0;
        SDL_Event sdlEvent;
        const bool traceResizePerfThisFrame = m_ResizePerfTraceEnabled;
        // Snapshot geometry-changed flag from previous frame, then reset for this frame.
        geometryChangedLastFrame = m_WindowGeometryChangedThisFrame;
        m_WindowGeometryChangedThisFrame = false;
        // Always capture drain start: used by P0 budget check and P3 skip-render decision
        // regardless of whether tracing is active.
        const auto eventDrainStart = std::chrono::steady_clock::now();
        auto lastDrainBatchCheckTime = eventDrainStart;
        double maxSinglePollBatchMs = 0.0;
        bool p0FiredThisDrain = false;
        while (SDL_PollEvent(&sdlEvent))
        {
            hadEvents = true;
            ++drainedEventCount;
            // Let layers handle raw SDL events (for ImGui integration and input handling)
            for (const auto& layer : m_LayerStack)
            {
                guardLayerCall(layer, "onSDLEvent", [&] { layer->onSDLEvent(&sdlEvent); });
            }

            // Translate window close events to our event system for clean shutdown coordination
            if (sdlEvent.type == SDL_EVENT_QUIT || sdlEvent.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
            {
                WindowCloseEvent event;
                raiseEvent(event);
                if (!event.isHandled())
                {
                    stop();
                }
            }

            // Drive viewport updates from resize-related events.
            // On Windows, interactive border drag can surface WINDOW_RESIZED/EXPOSED before
            // (or instead of) WINDOW_PIXEL_SIZE_CHANGED in some paths. Handle all relevant
            // variants and use physical pixel size when explicit dimensions are unavailable.
            if (sdlEvent.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED)
            {
                ++resizeEventCount;
                WindowResizedEvent resizeEvent(sdlEvent.window.data1, sdlEvent.window.data2);
                raiseEvent(resizeEvent);
                needsResizeRedraw = true;
                m_InteractionRedrawUntil = getTime() + INTERACTION_REDRAW_GRACE_SECONDS;
            }
            else if (sdlEvent.type == SDL_EVENT_WINDOW_RESIZED || sdlEvent.type == SDL_EVENT_WINDOW_EXPOSED)
            {
                ++resizeEventCount;
                const auto [pixelW, pixelH] = m_Window->getSizeInPixels();
                if (pixelW > 0 && pixelH > 0)
                {
                    WindowResizedEvent resizeEvent(pixelW, pixelH);
                    raiseEvent(resizeEvent);
                    needsResizeRedraw = true;
                    m_InteractionRedrawUntil = getTime() + INTERACTION_REDRAW_GRACE_SECONDS;
                }
            }
            else if (sdlEvent.type == SDL_EVENT_WINDOW_MOVED)
            {
                ++resizeEventCount;
                m_InteractionRedrawUntil = getTime() + INTERACTION_REDRAW_GRACE_SECONDS;
            }

            // P0: Drain time budget — break if this batch has spent too long in the drain
            // loop. Check every 4 events to limit steady_clock::now() call overhead.
            // On Wayland, individual SDL_PollEvent calls can stall on compositor protocol;
            // breaking here caps the combined per-frame drain stall.
            if ((drainedEventCount & 3U) == 0U)
            {
                const auto drainNow = std::chrono::steady_clock::now();
                // Track max time for any single 4-event batch to isolate single-call stalls.
                const double batchMs = std::chrono::duration<double, std::milli>(drainNow - lastDrainBatchCheckTime).count();
                maxSinglePollBatchMs = std::max(maxSinglePollBatchMs, batchMs);
                lastDrainBatchCheckTime = drainNow;
                if (FramePacing::computeShouldBreakEventDrain(std::chrono::duration<double, std::milli>(drainNow - eventDrainStart).count(),
                                                              DRAIN_BUDGET_MS))
                {
                    p0FiredThisDrain = true;
                    break;
                }
            }
        }
        // Always capture end time; used for both trace recording and P3 skip-render decision.
        const auto eventDrainEnd = std::chrono::steady_clock::now();
        const double totalDrainMs = std::chrono::duration<double, std::milli>(eventDrainEnd - eventDrainStart).count();
        if (traceResizePerfThisFrame)
        {
            resizeTraceStats.recordEventBatch(drainedEventCount, resizeEventCount, totalDrainMs, maxSinglePollBatchMs, p0FiredThisDrain);
        }

        if (m_Window->shouldClose())
        {
            stop();
            break;
        }

        // Keep interactive move/resize visually responsive across platforms
        // without reintroducing per-event rendering stalls: render at most once
        // per drained event batch.
        bool didImmediateResizeRedraw = false;
        const bool forceInteractionRedraw = FramePacing::isWithinInteractionGrace(getTime(), m_InteractionRedrawUntil);
        const bool isInteracting = FramePacing::computeIsInteracting(needsResizeRedraw, forceInteractionRedraw, resizeEventCount);
        const bool tracingInteraction = m_ResizePerfTraceEnabled && isInteracting;

        // P1: Adaptive vsync — disable vsync at interaction start to break the
        // vsync/compositor-stall coupling that causes drain and swap spikes on
        // Wayland. Restore adaptive vsync when the interaction ends (after the
        // grace period expires) so idle frames remain tear-free.
        switch (FramePacing::computeVsyncTransition(wasInteracting, isInteracting, m_Spec.VSync, m_VsyncDisabledForInteraction))
        {
        case FramePacing::VsyncTransition::Disable:
            Window::setVSync(false);
            m_VsyncDisabledForInteraction = true;
            break;
        case FramePacing::VsyncTransition::Restore:
            Window::setVSync(true);
            m_VsyncDisabledForInteraction = false;
            break;
        case FramePacing::VsyncTransition::NoChange:
            break;
        }

        if (m_ResizePerfTraceEnabled && wasTracingInteraction && !tracingInteraction)
        {
            logResizePerfTraceSummary(resizeTraceStats, "interaction-end");
            resizeTraceStats = {};
        }
        // Reset stats at interaction start so idle-frame event batches accumulated before
        // the interaction do not skew the first interaction-progress log averages.
        if (m_ResizePerfTraceEnabled && !wasTracingInteraction && tracingInteraction)
        {
            resizeTraceStats = {};
            lastResizeTraceLogTime = getTime();
        }

        // P3: If drain severely exceeded a full-frame budget, skip rendering this
        // frame to avoid compounding the stall with render+swap time. Events are
        // fully processed; the display catches up on the next frame.
        const bool skipRenderThisFrame =
            FramePacing::computeSkipRenderThisFrame(totalDrainMs, DRAIN_SKIP_RENDER_MS, m_Window->isMinimized());
        if (skipRenderThisFrame && traceResizePerfThisFrame && tracingInteraction)
        {
            ++resizeTraceStats.skippedRenderFrames;
        }

        if ((needsResizeRedraw || forceInteractionRedraw) && !m_Window->isMinimized() && !skipRenderThisFrame)
        {
            double updateMs = 0.0;
            double renderMs = 0.0;
            double postRenderMs = 0.0;
            double swapMs = 0.0;
            renderFrame(computeDeltaTime(),
                        tracingInteraction,
                        true,
                        tracingInteraction ? &updateMs : nullptr,
                        tracingInteraction ? &renderMs : nullptr,
                        tracingInteraction ? &postRenderMs : nullptr,
                        tracingInteraction ? &swapMs : nullptr);
            if (tracingInteraction)
            {
                resizeTraceStats.recordFrame(true, updateMs, renderMs, postRenderMs, swapMs);
            }
            didImmediateResizeRedraw = true;
        }

        // When the event queue is empty, decide whether to sleep or render immediately:
        // - Inside the grace period: skip the sleep and fall through to renderFrame so the
        //   display stays current during burst gaps between resize/move events.
        // - Outside the grace period: sleep briefly (~20 fps idle, 5 fps minimized) to
        //   reduce CPU/GPU usage when the display hasn't changed. Any SDL event wakes the
        //   sleep immediately, keeping interactive frame rate unaffected.
        if (!hadEvents)
        {
            const bool keepInteractionRedrawActive = FramePacing::isWithinInteractionGrace(getTime(), m_InteractionRedrawUntil);
            // During the grace period, allow sleep if window geometry did not change last frame.
            // This prevents ~20 wasted renders after the user releases mouse while the window
            // is stationary. SDL_WaitEventTimeout wakes immediately on any event, so
            // responsiveness is unaffected. geometryChangedLastFrame reflects the previous
            // frame's onUpdate result (1-frame lag is intentional and benign).
            if (FramePacing::computeShouldSleepWhenIdle(keepInteractionRedrawActive, geometryChangedLastFrame))
            {
                const int sleepMs = FramePacing::computeIdleSleepMs(m_Window->isMinimized(), IDLE_FRAME_SLEEP_MS, MINIMIZED_FRAME_SLEEP_MS);
                SDL_WaitEventTimeout(nullptr, sleepMs);
            }
        }

        if (!didImmediateResizeRedraw && !skipRenderThisFrame)
        {
            double updateMs = 0.0;
            double renderMs = 0.0;
            double postRenderMs = 0.0;
            double swapMs = 0.0;
            renderFrame(computeDeltaTime(),
                        tracingInteraction,
                        false,
                        tracingInteraction ? &updateMs : nullptr,
                        tracingInteraction ? &renderMs : nullptr,
                        tracingInteraction ? &postRenderMs : nullptr,
                        tracingInteraction ? &swapMs : nullptr);
            if (tracingInteraction)
            {
                resizeTraceStats.recordFrame(false, updateMs, renderMs, postRenderMs, swapMs);
            }
        }

        if (tracingInteraction && ((getTime() - lastResizeTraceLogTime) >= RESIZE_PERF_TRACE_LOG_INTERVAL_SECONDS))
        {
            logResizePerfTraceSummary(resizeTraceStats, "interaction-progress");
            resizeTraceStats = {};
            lastResizeTraceLogTime = getTime();
        }

        wasTracingInteraction = tracingInteraction;
        wasInteracting = isInteracting;
    }

    if (m_ResizePerfTraceEnabled)
    {
        logResizePerfTraceSummary(resizeTraceStats, "shutdown");
    }

    spdlog::info("Exiting main loop");
}

void Application::renderFrame(float deltaTime,
                              bool tracingInteractionFrame,
                              bool resizeTriggeredFrame,
                              double* updateMs,
                              double* renderMs,
                              double* postRenderMs,
                              double* swapMs)
{
    const bool tracing = (updateMs != nullptr);

    struct LayerPhaseDuration
    {
        std::string name;
        double durationMs = 0.0;
    };

    std::vector<LayerPhaseDuration> updateLayerDurations;
    std::vector<LayerPhaseDuration> postLayerDurations;
    if (tracingInteractionFrame)
    {
        updateLayerDurations.reserve(m_LayerStack.size());
        postLayerDurations.reserve(m_LayerStack.size());
    }

    const auto updateStart = tracing ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    // Update all layers
    for (const auto& layer : m_LayerStack)
    {
        const auto layerStart = tracingInteractionFrame ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        guardLayerCall(layer, "onUpdate", [&] { layer->onUpdate(deltaTime); });
        if (tracingInteractionFrame)
        {
            const auto layerEnd = std::chrono::steady_clock::now();
            updateLayerDurations.emplace_back(LayerPhaseDuration{
                .name = layer->getName(), .durationMs = std::chrono::duration<double, std::milli>(layerEnd - layerStart).count()});
        }
    }
    const auto updateEnd = tracing ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};

    // Render all layers
    for (const auto& layer : m_LayerStack)
    {
        guardLayerCall(layer, "onRender", [&] { layer->onRender(); });
    }
    const auto renderEnd = tracing ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};

    // Post-render (for ImGui frame end, etc.)
    for (const auto& layer : m_LayerStack)
    {
        const auto layerStart = tracingInteractionFrame ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        guardLayerCall(layer, "onPostRender", [&] { layer->onPostRender(); });
        if (tracingInteractionFrame)
        {
            const auto layerEnd = std::chrono::steady_clock::now();
            postLayerDurations.emplace_back(LayerPhaseDuration{
                .name = layer->getName(), .durationMs = std::chrono::duration<double, std::milli>(layerEnd - layerStart).count()});
        }
    }
    const auto postRenderEnd = tracing ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};

    m_Window->swapBuffers();
    const auto swapEnd = tracing ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};

    if (updateMs != nullptr)
    {
        *updateMs = std::chrono::duration<double, std::milli>(updateEnd - updateStart).count();
    }
    if (renderMs != nullptr)
    {
        *renderMs = std::chrono::duration<double, std::milli>(renderEnd - updateEnd).count();
    }
    if (postRenderMs != nullptr)
    {
        *postRenderMs = std::chrono::duration<double, std::milli>(postRenderEnd - renderEnd).count();
    }
    if (swapMs != nullptr)
    {
        *swapMs = std::chrono::duration<double, std::milli>(swapEnd - postRenderEnd).count();
    }

    if (tracingInteractionFrame)
    {
        const double measuredUpdateMs = std::chrono::duration<double, std::milli>(updateEnd - updateStart).count();
        const double measuredPostMs = std::chrono::duration<double, std::milli>(postRenderEnd - renderEnd).count();
        const double measuredSwapMs = std::chrono::duration<double, std::milli>(swapEnd - postRenderEnd).count();
        const double measuredRenderMs = std::chrono::duration<double, std::milli>(renderEnd - updateEnd).count();
        const bool slowUpdate = measuredUpdateMs >= RESIZE_PERF_TRACE_SLOW_COMPUTE_THRESHOLD_MS;
        const bool slowRender = measuredRenderMs >= RESIZE_PERF_TRACE_SLOW_COMPUTE_THRESHOLD_MS;
        const bool slowPost = measuredPostMs >= RESIZE_PERF_TRACE_SLOW_COMPUTE_THRESHOLD_MS;
        // Swap threshold is higher: Wayland compositor hold routinely adds 8-15 ms on
        // smooth frames. Only flag genuine stalls that exceed a full frame budget.
        const bool slowSwap = measuredSwapMs >= RESIZE_PERF_TRACE_SLOW_SWAP_THRESHOLD_MS;

        if (slowUpdate || slowRender || slowPost || slowSwap)
        {
            const auto logTopLayers = [](const std::vector<LayerPhaseDuration>& durations, const std::string_view phase)
            {
                if (durations.empty())
                {
                    return;
                }

                std::vector<LayerPhaseDuration> sorted = durations;
                std::ranges::sort(sorted,
                                  [](const LayerPhaseDuration& a, const LayerPhaseDuration& b) { return a.durationMs > b.durationMs; });

                const auto count = std::min<std::size_t>(static_cast<std::size_t>(RESIZE_PERF_TRACE_TOP_LAYER_COUNT), sorted.size());
                for (std::size_t i = 0; i < count; ++i)
                {
                    spdlog::info("ResizePerfSlowLayer[{}]: rank={} layer='{}' duration={:.3f} ms",
                                 phase,
                                 i + 1,
                                 sorted[i].name,
                                 sorted[i].durationMs);
                }
            };

            // Main frame header fires only for slow compute phases (CPU work we control).
            // Swap has its own higher threshold and its own log line below.
            if (slowUpdate || slowRender || slowPost)
            {
                spdlog::info("ResizePerfSlowFrame: resizeFrame={} update={:.3f} ms render={:.3f} ms post={:.3f} ms swap={:.3f} ms",
                             resizeTriggeredFrame,
                             measuredUpdateMs,
                             measuredRenderMs,
                             measuredPostMs,
                             measuredSwapMs);
            }

            if (slowUpdate)
            {
                logTopLayers(updateLayerDurations, "update");
            }
            if (slowRender)
            {
                spdlog::info("ResizePerfSlowFrame[render]: {:.3f} ms (ImGui draw; consider reducing content complexity at this size)",
                             measuredRenderMs);
            }
            if (slowPost)
            {
                logTopLayers(postLayerDurations, "post");
            }
            if (slowSwap)
            {
                spdlog::info("ResizePerfSlowFrame[swap]: {:.3f} ms (compositor hold or vsync stall)", measuredSwapMs);
            }
        }
    }
}

void Application::stop()
{
    m_Running = false;
}

void Application::raiseEvent(Event& event)
{
    // Dispatch to layers in reverse order (topmost first)
    for (auto& layer : std::views::reverse(m_LayerStack))
    {
        guardLayerCall(layer, "onEvent", [&] { layer->onEvent(event); });
        if (event.isHandled())
        {
            break;
        }
    }
}

Application& Application::get()
{
    // Support both unique_ptr-managed (via setInstance) and stack-allocated (tests) instances
    // Check s_Instance first (set via setInstance()).
    // Note: setInstance() clears g_StackApplicationInstance, so at most one is set.
    if (s_Instance)
    {
        return *s_Instance;
    }
    if (g_StackApplicationInstance)
    {
        return g_StackApplicationInstance->get();
    }
    // No instance in either ownership model
    throw std::runtime_error("Application does not exist!");
}

float Application::getTime()
{
    // SDL_GetTicks returns milliseconds as Uint64
    return static_cast<float>(SDL_GetTicks()) / 1000.0F;
}

/// Set the global application instance for initialization or cleanup.
/// This ensures the Application is managed by std::unique_ptr with proper RAII cleanup.
/// Called from main() during initialization, and with nullptr from tests for cleanup/reset.
///
/// THREAD-SAFETY: This method MUST ONLY be called from the main thread during initialization,
/// before any other threads access Application::get(). No synchronization is performed.
void Application::setInstance(std::unique_ptr<Application> app)
{
    // Prevent overwriting an existing s_Instance (programming error)
    if (s_Instance && app != nullptr)
    {
        throw std::logic_error("setInstance() called when an instance already exists. Call setInstance(nullptr) first to clear.");
    }

    // When taking ownership via unique_ptr, verify no *different* stack instance exists.
    // If a stack-allocated instance exists and matches this app, that's OK (e.g., from constructor).
    // If it's different, that's a programming error - we can't safely manage both.
    if (app != nullptr)
    {
        if (g_StackApplicationInstance.has_value() && &g_StackApplicationInstance->get() != app.get())
        {
            throw std::logic_error("setInstance() called while a different stack-allocated Application instance exists");
        }
        // Clear the stack-tracked instance since we're now taking unique_ptr ownership.
        // This prevents dangling pointer access later if someone clears the singleton.
        g_StackApplicationInstance.reset();
    }
    else
    {
        // When clearing (app == nullptr), also clear any stack-tracked instance.
        // This ensures get() won't try to access a potentially dead stack instance.
        g_StackApplicationInstance.reset();
    }

    Application::s_Instance = std::move(app);
}

} // namespace Core
