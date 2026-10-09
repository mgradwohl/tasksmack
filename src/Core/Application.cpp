#include "Application.h"

#include "Core/AnimationRequest.h"
#include "Core/EnvUtils.h"
#include "Core/Event.h"
#include "Core/FramePacing.h"
#include "Core/Layer.h"
#include "Core/ResizePerfOperation.h"
#include "Core/ResizePerfTrace.h"
#include "Core/VideoBackend.h"
#include "Core/Window.h"
#include "Core/WindowEventRouting.h"
#include "Core/WindowEvents.h"
#include "Platform/ThreadName.h"
#include "version.h"

#include <SDL3/SDL.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
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
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <system_error>

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
// Animation only: the FPS readout gets the unclamped interval (lastFrameIntervalSeconds(), #1152).
constexpr float MAX_DELTA_TIME = 0.1F;

// When no SDL events arrive, render the next frame this long after the previous one started
// (FramePacing::computeIdleWaitMs, #1276). This holds the idle render rate at 20 fps, reducing CPU usage when the display
// hasn't changed. Mouse movement and keyboard events wake the sleep immediately,
// so interactive frame rate is unaffected.
constexpr int IDLE_FRAME_SLEEP_MS = 50;
// The frame rate that idle sleep gives without input. Motion that needs no more than this is left
// to the idle path rather than paced (FramePacing::computeAnimationRate, #1125).
constexpr double IDLE_FRAME_RATE = 1000.0 / IDLE_FRAME_SLEEP_MS;

// The fastest the loop paces frames, whatever asks: a visible chart or NowBar moving fast enough
// (Core::AnimationRequest, #1037/#1125) or input (#1153). A whole number of display refreshes never
// slower than this (FramePacing::vblanksPerFrame, #1126): 60 fps at 60/120 Hz, 75 at 75 Hz, 72 at
// 144 Hz, 82.5 at 165 Hz. A move/resize interaction is capped at the display rate instead.
constexpr double MAX_FRAME_RATE = 60.0;

// The refresh rate assumed when SDL does not report the display's (#1126).
constexpr double FALLBACK_REFRESH_HZ = 60.0;

// When the window is minimized there is nothing visible to render, so the sleep
// is extended to ~5 fps. Any event (e.g. SDL_EVENT_WINDOW_RESTORED) wakes
// immediately, so restore latency is unaffected.
constexpr int MINIMIZED_FRAME_SLEEP_MS = 200;

// During interactive move/resize, event delivery can be bursty depending on
// compositor/window-manager behavior. Keep redraw active for a short grace
// window after each relevant window event so the framebuffer stays responsive
// without forcing continuous high-rate rendering when idle.
constexpr double INTERACTION_REDRAW_GRACE_SECONDS = 0.35;
// Whether an OS maximize of the borderless window is replaced by the client-side one
// (Window::adoptSystemMaximize(), #1208): on Windows only; see the SystemMaximized case in run().
#ifdef _WIN32
constexpr bool ADOPT_SYSTEM_MAXIMIZE = true;
#else
constexpr bool ADOPT_SYSTEM_MAXIMIZE = false;
#endif
constexpr const char* RESIZE_PERF_TRACE_ENV = "TASKSMACK_TRACE_RESIZE_PERF";
constexpr double RESIZE_PERF_TRACE_LOG_INTERVAL_SECONDS = 0.5;
// Idle/steady-state frames are logged on a much longer cadence than interaction frames: an
// interaction is a short, bounded burst where frequent logging is useful, but idle frames run
// indefinitely while the app just sits open, so 0.5s would spam the log forever (perf-plan #843
// phase 0 — idle-time performance is priority 1, but that doesn't mean logging it every tick).
constexpr double IDLE_PERF_TRACE_LOG_INTERVAL_SECONDS = 5.0;
constexpr int RESIZE_PERF_TRACE_TOP_LAYER_COUNT = 3;

// P0: Break the event drain loop if wall-clock drain exceeds this threshold.
// On Wayland, individual SDL_PollEvent calls can stall on compositor protocol
// (xdg_surface configure handshake). Capping drain limits per-frame stall time.
constexpr double DRAIN_BUDGET_MS = 8.0;

// P3: If total drain time exceeds a full 60 fps frame budget, skip rendering
// this frame. The event queue is fully processed; rendering catches up next frame.
constexpr double DRAIN_SKIP_RENDER_MS = 16.0;

// Keep routine budget misses in periodic percentile summaries; synchronous per-layer
// logging at 8 ms can itself dominate a capture. Detail only exceptional stalls.
constexpr double RESIZE_PERF_TRACE_SLOW_COMPUTE_THRESHOLD_MS = 100.0;
constexpr double RESIZE_PERF_TRACE_SLOW_SWAP_THRESHOLD_MS = 100.0;

void logResizePerfAnchor()
{
    const auto counterBefore = SDL_GetPerformanceCounter();
    const auto utcNs = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    const auto counterAfter = SDL_GetPerformanceCounter();
#ifdef _WIN32
    const auto pid = GetCurrentProcessId();
    const auto tid = GetCurrentThreadId();
    constexpr std::string_view CLOCK_NAME = "QPC";
#else
    const auto pid = getpid();
    const auto tid = SDL_GetCurrentThreadID();
    constexpr std::string_view CLOCK_NAME = "SDL-performance-counter";
#endif
    spdlog::info("ResizePerfAnchor: pid={} uiTid={} clock={} frequency={} counterBefore={} utcUnixNs={} counterAfter={} "
                 "commit={} configureSourceState={} version={} buildType={} compiler={}-{} configuredUTC='{} {}'",
                 pid,
                 tid,
                 CLOCK_NAME,
                 SDL_GetPerformanceFrequency(),
                 counterBefore,
                 utcNs,
                 counterAfter,
                 TASKSMACK_GIT_FULL_COMMIT,
                 TASKSMACK_GIT_SOURCE_STATE,
                 TASKSMACK_VERSION,
                 TASKSMACK_BUILD_TYPE,
                 TASKSMACK_COMPILER_ID,
                 TASKSMACK_COMPILER_VERSION,
                 TASKSMACK_BUILD_DATE,
                 TASKSMACK_BUILD_TIME);
}

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
        resizePerfOperations() = {};
        spdlog::info("Initializing {} application", m_Spec.Name);
        if (m_ResizePerfTraceEnabled)
        {
            spdlog::info("Resize performance tracing enabled via {}", RESIZE_PERF_TRACE_ENV);
            logResizePerfAnchor();
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

        // A headless application (#880) stops here: no video subsystem, no window, no GL context.
        // SDL's clock (getTime()) needs no initialization, and raiseEvent() needs only the layers.
        if (m_Spec.Headless)
        {
            spdlog::info("Headless: no window is created");
            return;
        }

        // Initialize SDL video subsystem
#ifndef _WIN32
        ensureXdgRuntimeDir();
#endif
        // Closing the last window must not also post SDL_EVENT_QUIT (SDL's default). With it, one
        // Alt+F4 raised two WindowCloseEvents, and since run() treats SDL_EVENT_QUIT as a
        // non-vetoable termination request (SIGINT/SIGTERM, logout), it would also override a
        // layer's veto of the close request. The window's close request is handled on its own
        // (#1150).
        // Override priority: at normal priority an SDL_QUIT_ON_LAST_WINDOW_CLOSE environment
        // variable wins and SDL_SetHint() returns false, which would bring the double close back.
        if (!SDL_SetHintWithPriority(SDL_HINT_QUIT_ON_LAST_WINDOW_CLOSE, "0", SDL_HINT_OVERRIDE))
        {
            spdlog::warn("Could not disable SDL_HINT_QUIT_ON_LAST_WINDOW_CLOSE: {}", SDL_GetError());
        }
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
    // Detach layers in reverse order (topmost first). Guarded like every other per-layer callback
    // (#778): a layer that throws while detaching must not stop the rest being torn down (#1124).
    for (auto& layer : std::views::reverse(m_LayerStack))
    {
        guardLayerCall(layer, "onDetach", [&] { layer->onDetach(); });
    }
    m_LayerStack.clear();
}

void Application::run()
{
    if (!m_Window)
    {
        throw std::logic_error("Application::run() needs a window; a headless application is driven by its owner (#880)");
    }
    m_Running = true;
    // The UI thread is whichever thread runs the loop. A no-op on Linux, where the main thread's
    // name is the process name (see Platform::setMainThreadName).
    Platform::setMainThreadName(Platform::UI_THREAD_NAME);

    double lastTime = getTime();

    const auto computeDeltaTime = [this, &lastTime]() -> float
    {
        const double currentTime = getTime();
        const float deltaTime = FramePacing::frameDeltaSeconds(lastTime, currentTime, MAX_DELTA_TIME);
        m_LastFrameIntervalSeconds = FramePacing::frameIntervalSeconds(lastTime, currentTime);
        lastTime = currentTime;
        return deltaTime;
    };

    // The display's refresh rate, which frame pacing rounds to (#1126); re-read on a display change.
    const auto refreshDisplayRate = [this]()
    {
        const double queried = m_Window->getDisplayRefreshRate();
        const double effective = FramePacing::effectiveRefreshHz(queried, FALLBACK_REFRESH_HZ);
        // Logged only for a real change: SDL can report the same mode again with a slightly
        // different rational rate (59.97 then 59.98 Hz) when the window first lands on a display.
        constexpr double LOG_CHANGE_HZ = 0.5;
        if (std::abs(effective - m_DisplayRefreshHz) >= LOG_CHANGE_HZ)
        {
            spdlog::info(
                "Frame pacing: display refresh {:.2f} Hz{}", effective, FramePacing::isUsableRefreshHz(queried) ? "" : " (assumed)");
        }
        m_DisplayRefreshHz = effective;
    };
    refreshDisplayRate();

    m_InteractionRedrawUntil = 0.0;

    ResizePerfTraceStats resizeTraceStats;
    bool wasTracingInteraction = false;
    bool wasInteracting = false;
    double lastResizeTraceLogTime = getTime();
    // Snapshot of whether TitleBarLayer changed window geometry (position/size) during
    // the PREVIOUS frame's onUpdate. Used to gate grace-period sleep: if no geometry
    // changed last frame, we allow the idle sleep even inside the interaction grace window,
    // preventing wasted renders when the window is stationary post-interaction.
    bool geometryChangedLastFrame = false;
    // The highest frame rate the previous frame's moving content asked for (Core::AnimationRequest,
    // 0 = nothing moved visibly), and when the last frame started: together they pace the next
    // frame (#1037, #1125).
    double requestedAnimationFps = 0.0;
    double lastFrameStart = getTime();
    // The event that woke the idle wait, taken off the queue by SDL_WaitEventTimeout(): the next
    // iteration's drain dispatches it before anything else, and before that iteration renders (#1409).
    std::optional<SDL_Event> idleWakeEvent;
    // P3 skips in a row, for the FramePacing::MAX_CONSECUTIVE_SKIPPED_RENDERS bound (#1410).
    std::uint32_t consecutiveSkippedRenders = 0;
    std::uint64_t loopStart = 0;
    ResizePerfLoopTiming loopTiming;
    const auto finishTracedLoop = [&](std::uint64_t end)
    {
        if (loopStart == 0)
        {
            return;
        }
        const double wallMs = resizePerfElapsedMs(loopStart, end);
        resizePerfOperations().loops.record(wallMs);
        if (wallMs > 100.0)
        {
            spdlog::info("ResizePerfLoop: beginCounter={} endCounter={} wall={:.3f} ms drain={:.3f} ms wait={:.3f} ms "
                         "vsync={:.3f} ms framePhases={:.3f} ms other={:.3f} ms",
                         loopStart,
                         end,
                         wallMs,
                         loopTiming.drainMs,
                         loopTiming.waitMs,
                         loopTiming.vsyncMs,
                         loopTiming.frameMs,
                         loopTiming.otherMs(wallMs));
        }
    };

    // Deliver-to-deliver loop intervals (#843 measurement kit): renderFrame() stamps each presented
    // frame's end (recordResizePerfFrameEnd), and the gap from the previous one is the cadence the
    // user sees, skipped renders included. The previous frame's end is kept in resizeTraceStats, so
    // the `= {}` resets at idle<->interaction transitions drop the interval that spans both states.
    const auto recordDeliveredFrame = [&]()
    {
        resizeTraceStats.recordDeliveredFrameEnd(resizePerfOperations().previousFrameEnd, SDL_GetPerformanceFrequency());
    };

    // The framebuffer size of the last WindowResizedEvent, so an SDL_EVENT_WINDOW_EXPOSED can tell
    // a repaint (same size) from a resize that surfaced only as an expose (#1154).
    std::pair<int, int> lastResizePixelSize = m_Window->getSizeInPixels();

    spdlog::info("Entering main loop");

    while (m_Running)
    {
        if (m_ResizePerfTraceEnabled)
        {
            const auto now = SDL_GetPerformanceCounter();
            finishTracedLoop(now);
            loopStart = now;
            loopTiming = {};
        }
        // Process SDL events
        bool needsResizeRedraw = false;
        std::uint32_t resizeEventCount = 0;
        SDL_Event sdlEvent;
        const bool traceResizePerfThisFrame = m_ResizePerfTraceEnabled;
        // Snapshot geometry-changed flag from previous frame, then reset for this frame.
        geometryChangedLastFrame = m_WindowGeometryChangedThisFrame;
        m_WindowGeometryChangedThisFrame = false;
        // Always capture drain start: used by P0 budget check and P3 skip-render decision
        // regardless of whether tracing is active.
        const auto eventDrainStart = std::chrono::steady_clock::now();
        // Handle the next event, false once the queue is empty. The event that woke the previous
        // iteration's idle wait comes first: SDL_WaitEventTimeout() took it off the queue (#1409).
        const auto pollAndDispatch = [&]() -> bool
        {
            if (idleWakeEvent.has_value())
            {
                sdlEvent = *idleWakeEvent;
                idleWakeEvent.reset();
            }
            else if (!SDL_PollEvent(&sdlEvent))
            {
                return false;
            }
            // Let layers handle raw SDL events (for ImGui integration and input handling)
            for (const auto& layer : m_LayerStack)
            {
                guardLayerCall(layer, "onSDLEvent", [&] { layer->onSDLEvent(&sdlEvent); });
            }

            // Raise a WindowResizedEvent for a new framebuffer size and start the interaction
            // grace period that keeps a live resize responsive.
            const auto handleResize = [&](std::pair<int, int> pixelSize)
            {
                // A 0x0 framebuffer (minimised) is not a resize: neither raised nor counted as an
                // interaction, so it can't start the interaction pacing.
                if (!WindowEventRouting::isRealPixelSize(pixelSize))
                {
                    return;
                }
                ++resizeEventCount;
                lastResizePixelSize = pixelSize;
                WindowResizedEvent resizeEvent(pixelSize.first, pixelSize.second);
                raiseEvent(resizeEvent);
                needsResizeRedraw = true;
                m_InteractionRedrawUntil = getTime() + INTERACTION_REDRAW_GRACE_SECONDS;
            };

            switch (WindowEventRouting::classify(sdlEvent.type))
            {
            case WindowEventRouting::Action::VetoableClose:
                // A close request becomes a WindowCloseEvent. A layer that handles it vetoes the
                // close; unhandled, the app stops (contract in WindowEvents.h).
                if (closeRequestAccepted())
                {
                    stop();
                }
                break;
            case WindowEventRouting::Action::Quit:
                // SIGINT/SIGTERM and OS logout/shutdown: always stops, and is not raised as a
                // WindowCloseEvent, so no layer can veto it (#1150).
                stop();
                break;
            // Drive viewport updates from resize-related events. On Windows, interactive border
            // drag can surface WINDOW_RESIZED/EXPOSED before (or instead of)
            // WINDOW_PIXEL_SIZE_CHANGED in some paths, so the physical pixel size is queried when
            // the event does not carry it.
            case WindowEventRouting::Action::PixelSizeChanged:
                handleResize({sdlEvent.window.data1, sdlEvent.window.data2});
                break;
            case WindowEventRouting::Action::Resized:
                // The normal size a later maximize from outside the app restores to (#1250).
                m_Window->handleGeometryChanged();
                handleResize(m_Window->getSizeInPixels());
                break;
            case WindowEventRouting::Action::Exposed:
                // A plain redraw request (#1154): having drained an event already rules out the
                // idle sleep, so the regular frame below repaints. Only an expose that reveals a
                // new size counts as a resize.
                if (const auto pixelSize = m_Window->getSizeInPixels();
                    WindowEventRouting::exposeChangesSize(lastResizePixelSize, pixelSize))
                {
                    handleResize(pixelSize);
                }
                break;
            case WindowEventRouting::Action::Moved:
                m_Window->handleGeometryChanged(); // As for Resized (#1250)
                ++resizeEventCount;
                m_InteractionRedrawUntil = getTime() + INTERACTION_REDRAW_GRACE_SECONDS;
                break;
            case WindowEventRouting::Action::DisplayChanged:
                refreshDisplayRate();
                break;
            case WindowEventRouting::Action::SystemMaximized:
                // On Windows, Win+Up, snap to the top edge or ShowWindow(SW_MAXIMIZE) on the
                // borderless window is replaced by the title-bar button's maximize, whose resize
                // events follow (#1208): the quarter-screen maximize is SDL's Win32
                // WM_GETMINMAXINFO sizing. X11/XWayland window managers and Wayland compositors size
                // a maximized window themselves, and the asynchronous restore/maximize round trip is
                // untested there, so on Linux the maximize is kept as it is -- but tracked, so the
                // rectangle saved as the normal size is the one from before it, not the maximized
                // one (#1250).
                m_Window->handleSystemMaximized(ADOPT_SYSTEM_MAXIMIZE);
                break;
            case WindowEventRouting::Action::SystemRestored:
                // A window-manager or compositor restore of an OS maximize (#1250).
                m_Window->handleSystemRestored();
                break;
            case WindowEventRouting::Action::Minimized:
                // Win+Down on the client-side maximized window restores it, as it does a native
                // maximized one (#1279). The window's subclass procedure normally catches it before
                // the minimize happens; this is for a minimize that did not pass through it.
                static_cast<void>(m_Window->restoreForShellMinimize());
                break;
            case WindowEventRouting::Action::DisplayScaleChanged:
                // Refresh the normal geometry's scale, as a move or resize would (#1250).
                m_Window->handleGeometryChanged();
                break;
            case WindowEventRouting::Action::None:
                break;
            }

            return true;
        };
        // P0: Drain time budget, checked after every event (#1410). On Wayland, individual
        // SDL_PollEvent calls can stall on compositor protocol; breaking here caps the combined
        // per-frame drain stall.
        const auto drain = FramePacing::drainEventsWithinBudget(
            pollAndDispatch,
            [&] { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - eventDrainStart).count(); },
            DRAIN_BUDGET_MS);
        const bool hadEvents = drain.eventCount > 0;
        // Always capture end time; used for both trace recording and P3 skip-render decision.
        const auto eventDrainEnd = std::chrono::steady_clock::now();
        const double totalDrainMs = std::chrono::duration<double, std::milli>(eventDrainEnd - eventDrainStart).count();
        if (traceResizePerfThisFrame)
        {
            loopTiming.drainMs = totalDrainMs;
        }
        // recordEventBatch() for THIS drain is deferred until after the interaction-transition
        // reset below (isInteracting depends on resizeEventCount/needsResizeRedraw from this
        // same drain), so the batch that triggers a transition lands in the correctly-reset
        // accumulator for its own state instead of being recorded into the old state's
        // accumulator and then immediately wiped (or misattributed into the other state's
        // boundary log) by that reset. The shouldClose() check that used to sit here is moved
        // below, after recordEventBatch() actually runs: otherwise a shutdown request arriving
        // in this drain would break out before recordEventBatch() executes at all, silently
        // dropping the final drain from the "shutdown" summary.

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
        const auto vsyncStart = traceResizePerfThisFrame ? SDL_GetPerformanceCounter() : 0;
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
        if (traceResizePerfThisFrame)
        {
            loopTiming.vsyncMs = resizePerfElapsedMs(vsyncStart, SDL_GetPerformanceCounter());
        }

        if (m_ResizePerfTraceEnabled && wasTracingInteraction && !tracingInteraction)
        {
            logResizePerfTraceSummary(resizeTraceStats, "interaction-end");
            resizeTraceStats = {};
            // Restart the cadence here too (mirroring the idle->interaction reset below):
            // lastResizeTraceLogTime is at most one 0.5s interaction-progress tick stale, so
            // without this the first post-interaction idle-progress window comes up short of a
            // full IDLE_PERF_TRACE_LOG_INTERVAL_SECONDS, rather than starting fresh at the
            // moment idle begins.
            lastResizeTraceLogTime = getTime();
        }
        // Flush the partial idle interval before resetting, so rare stalls and tail counts
        // are not discarded when the user begins the next interaction.
        if (m_ResizePerfTraceEnabled && !wasTracingInteraction && tracingInteraction)
        {
            logResizePerfTraceSummary(resizeTraceStats, "idle-end");
            resizeTraceStats = {};
            lastResizeTraceLogTime = getTime();
        }

        // Record THIS drain now that any transition reset above has run, so it's classified
        // into (and counted by) the accumulator for its own state, not the state that just
        // ended.
        if (traceResizePerfThisFrame)
        {
            resizeTraceStats.recordEventBatch(
                drain.eventCount, resizeEventCount, totalDrainMs, drain.maxSingleEventMs, drain.budgetExceeded);
        }

        // Deferred from just after the drain (see comment above) so this frame's batch is
        // always recorded -- including the final one before shutdown -- before we might break.
        // Window::requestClose() (the custom title bar's Close button and system menu) is a close
        // request like Alt+F4, so it goes through the same WindowCloseEvent veto (#1077).
        if (m_Window->shouldClose())
        {
            m_Window->clearCloseRequest();
            if (closeRequestAccepted())
            {
                stop();
                break;
            }
        }

        // P3: If drain severely exceeded a full-frame budget, skip rendering this
        // frame to avoid compounding the stall with render+swap time. Events are
        // fully processed; the display catches up on the next frame.
        // Bounded at MAX_CONSECUTIVE_SKIPPED_RENDERS in a row, so a sustained slow drain can't
        // freeze the display (#1410).
        const bool skipRenderThisFrame = FramePacing::computeSkipRenderThisFrame(totalDrainMs,
                                                                                 DRAIN_SKIP_RENDER_MS,
                                                                                 m_Window->isMinimized(),
                                                                                 consecutiveSkippedRenders,
                                                                                 FramePacing::MAX_CONSECUTIVE_SKIPPED_RENDERS);
        // Counted regardless of interaction state (not just tracingInteraction): an idle drain
        // that overruns the budget also skips rendering, and the idle-progress/shutdown
        // summaries should reflect that instead of always reporting skippedFrames=0 for idle.
        if (skipRenderThisFrame && traceResizePerfThisFrame)
        {
            ++resizeTraceStats.skippedRenderFrames;
        }

        // Minimized or covered (#1125): nothing on screen to animate, so no paced frames and the
        // longer hidden idle sleep.
        const bool isHidden = m_Window->isMinimized() || m_Window->isOccluded();

        // isHidden, not only minimized: an occluded window that gets a move/resize event must not render
        // at the display rate through the interaction grace period either (#1125).
        if ((needsResizeRedraw || forceInteractionRedraw) && !isHidden && !skipRenderThisFrame)
        {
            // Vsync is off during an interaction, so a stream of mouse events would otherwise render
            // unbounded (#1153): cap it at the display rate, which vsync would have given.
            const double interactionWaitSeconds = FramePacing::computeFrameWaitSeconds(
                getTime() - lastFrameStart, FramePacing::framePeriodSeconds(m_DisplayRefreshHz, AnimationRequest::FULL_RATE));
            if (interactionWaitSeconds > 0.0)
            {
                const auto waitStart = traceResizePerfThisFrame ? SDL_GetPerformanceCounter() : 0;
                SDL_DelayPrecise(static_cast<Uint64>(interactionWaitSeconds * 1.0e9));
                if (traceResizePerfThisFrame)
                {
                    loopTiming.waitMs = resizePerfElapsedMs(waitStart, SDL_GetPerformanceCounter());
                }
            }
            lastFrameStart = getTime();
            double updateMs = 0.0;
            double renderMs = 0.0;
            double postRenderMs = 0.0;
            double swapMs = 0.0;
            renderFrame(computeDeltaTime(),
                        true,
                        traceResizePerfThisFrame ? &updateMs : nullptr,
                        traceResizePerfThisFrame ? &renderMs : nullptr,
                        traceResizePerfThisFrame ? &postRenderMs : nullptr,
                        traceResizePerfThisFrame ? &swapMs : nullptr);
            if (traceResizePerfThisFrame)
            {
                resizeTraceStats.recordFrame(true, updateMs, renderMs, postRenderMs, swapMs);
                recordDeliveredFrame();
                loopTiming.frameMs = updateMs + renderMs + postRenderMs + swapMs;
            }
            didImmediateResizeRedraw = true;
        }

        // Pace the regular frame:
        // - Something moving visibly asked for more frames than idling gives, or input arrived:
        //   cap the frame rate (FramePacing::computeFrameRateCap) at a whole number of display
        //   refreshes. Charts keep a steady rate whatever the input (#1037), only as fast as their
        //   motion needs (#1125); input frames are capped too (#1153). Events that arrive during
        //   the wait are handled by the next drain, at most one period later.
        // - Otherwise, with the event queue empty, decide whether to sleep or render immediately:
        //   - Inside the grace period: skip the sleep and fall through to renderFrame so the
        //     display stays current during burst gaps between resize/move events.
        //   - Outside the grace period: sleep briefly (~20 fps idle, 5 fps minimized or covered)
        //     to reduce CPU/GPU usage when the display hasn't changed. Any SDL event wakes the
        //     sleep immediately, keeping interactive frame rate unaffected. The waking event is
        //     dispatched before the next frame renders (#1409): see idleWakeEvent.
        const double animationFps = FramePacing::computeAnimationRate(requestedAnimationFps, IDLE_FRAME_RATE, MAX_FRAME_RATE);
        const double frameRateCap = FramePacing::computeFrameRateCap(animationFps, hadEvents, isHidden, MAX_FRAME_RATE);
        if (frameRateCap > 0.0 && !isInteracting)
        {
            const double waitSeconds = FramePacing::computeFrameWaitSeconds(
                getTime() - lastFrameStart, FramePacing::framePeriodSeconds(m_DisplayRefreshHz, frameRateCap));
            if (waitSeconds > 0.0)
            {
                const auto waitStart = traceResizePerfThisFrame ? SDL_GetPerformanceCounter() : 0;
                SDL_DelayPrecise(static_cast<Uint64>(waitSeconds * 1.0e9));
                if (traceResizePerfThisFrame)
                {
                    loopTiming.waitMs = resizePerfElapsedMs(waitStart, SDL_GetPerformanceCounter());
                }
            }
        }
        else if (!hadEvents)
        {
            const bool keepInteractionRedrawActive = FramePacing::isWithinInteractionGrace(getTime(), m_InteractionRedrawUntil);
            // During the grace period, allow sleep if window geometry did not change last frame.
            // This prevents ~20 wasted renders after the user releases mouse while the window
            // is stationary. SDL_WaitEventTimeout wakes immediately on any event, so
            // responsiveness is unaffected. geometryChangedLastFrame reflects the previous
            // frame's onUpdate result (1-frame lag is intentional and benign).
            if (FramePacing::computeShouldSleepWhenIdle(keepInteractionRedrawActive, geometryChangedLastFrame))
            {
                const int sleepMs =
                    FramePacing::computeIdleWaitMs(isHidden, IDLE_FRAME_SLEEP_MS, MINIMIZED_FRAME_SLEEP_MS, getTime() - lastFrameStart);
                const auto waitStart = traceResizePerfThisFrame ? SDL_GetPerformanceCounter() : 0;
                // Wait with a real SDL_Event, not nullptr: the event that wakes the loop is taken
                // off the queue here and handed to the next iteration's drain, which dispatches it
                // before rendering (#1409). With nullptr the event stayed queued and the frame below
                // rendered straight after the wake, showing the state from before the input.
                // A wait that times out polls once more (#1450): a wake from another thread can
                // reach it late, with the event already queued, and that event is a wake too.
                SDL_Event wakeEvent;
                const auto waitOutcome = FramePacing::waitForIdleEvent(
                    wakeEvent,
                    [sleepMs](SDL_Event& event) { return SDL_WaitEventTimeout(&event, sleepMs); },
                    [](SDL_Event& event) { return SDL_PollEvent(&event); });
                if (waitOutcome != FramePacing::IdleWaitOutcome::TimedOut)
                {
                    idleWakeEvent = wakeEvent;
                }
                ++m_IdleWaitCount;
                m_LastIdleWaitWoke = waitOutcome == FramePacing::IdleWaitOutcome::Woke;
                m_LastIdleWaitPolledEvent = waitOutcome == FramePacing::IdleWaitOutcome::PolledAfterTimeout;
                if (traceResizePerfThisFrame)
                {
                    loopTiming.waitMs = resizePerfElapsedMs(waitStart, SDL_GetPerformanceCounter());
                }
            }
        }

        // A wake on an event skips this render: the next iteration drains the event first, then
        // renders, paced as any input-driven frame (#1153) (#1409).
        const bool renderRegularFrame =
            FramePacing::computeShouldRenderRegularFrame(idleWakeEvent.has_value(), didImmediateResizeRedraw, skipRenderThisFrame);
        if (renderRegularFrame)
        {
            lastFrameStart = getTime();
            double updateMs = 0.0;
            double renderMs = 0.0;
            double postRenderMs = 0.0;
            double swapMs = 0.0;
            renderFrame(computeDeltaTime(),
                        false,
                        traceResizePerfThisFrame ? &updateMs : nullptr,
                        traceResizePerfThisFrame ? &renderMs : nullptr,
                        traceResizePerfThisFrame ? &postRenderMs : nullptr,
                        traceResizePerfThisFrame ? &swapMs : nullptr);
            if (traceResizePerfThisFrame)
            {
                resizeTraceStats.recordFrame(false, updateMs, renderMs, postRenderMs, swapMs);
                recordDeliveredFrame();
                loopTiming.frameMs = updateMs + renderMs + postRenderMs + swapMs;
            }
        }

        // Log periodically regardless of interaction state (perf-plan #843 phase 0): idle
        // frames use a longer cadence than interaction frames (see
        // IDLE_PERF_TRACE_LOG_INTERVAL_SECONDS above), and both share the same accumulator/p95
        // logging so idle and interactive numbers are directly comparable.
        const double perfTraceLogIntervalSeconds =
            isInteracting ? RESIZE_PERF_TRACE_LOG_INTERVAL_SECONDS : IDLE_PERF_TRACE_LOG_INTERVAL_SECONDS;
        if (traceResizePerfThisFrame && ((getTime() - lastResizeTraceLogTime) >= perfTraceLogIntervalSeconds))
        {
            logResizePerfTraceSummary(resizeTraceStats, isInteracting ? "interaction-progress" : "idle-progress");
            // Reset only the per-interval counters, NOT the rolling percentile sample windows:
            // a periodic log within the same idle/interaction state should let those windows
            // keep accumulating past PERCENTILE_WINDOW_SIZE's threshold for a meaningful p99,
            // rather than restarting from an empty window (and a degenerate p99==max) every
            // single interval. The full `= {}` reset above/below at actual state transitions
            // still clears everything, including the rolling windows.
            resizeTraceStats.resetIntervalCounters();
            lastResizeTraceLogTime = getTime();
        }

        // Read after this iteration's render(s): what they drew decides how the next frame is paced.
        // A skipped render (or a wake that defers it) leaves the previous answer standing.
        const bool renderedFrame = didImmediateResizeRedraw || renderRegularFrame;
        if (renderedFrame)
        {
            requestedAnimationFps = AnimationRequest::consume();
        }
        consecutiveSkippedRenders =
            FramePacing::nextConsecutiveSkippedRenders(consecutiveSkippedRenders, skipRenderThisFrame, renderedFrame);

        wasTracingInteraction = tracingInteraction;
        wasInteracting = isInteracting;
    }

    if (m_ResizePerfTraceEnabled)
    {
        finishTracedLoop(SDL_GetPerformanceCounter());
        logResizePerfTraceSummary(resizeTraceStats, "shutdown");
        logResizePerfOperationSummary();
        logResizePerfAnchor();
    }

    spdlog::info("Exiting main loop");
}

void Application::renderFrame(
    float deltaTime, bool resizeTriggeredFrame, double* updateMs, double* renderMs, double* postRenderMs, double* swapMs)
{
    // Single flag now governs both the aggregate phase timings below AND the detailed
    // per-layer breakdown (previously a separate tracingInteractionFrame parameter gated the
    // per-layer breakdown by interaction state alone, which meant idle frames skipped the
    // extra per-layer clock reads that interaction frames paid for -- silently inflating
    // interaction-progress phase timings relative to idle-progress ones and undermining the
    // "same accumulator, directly comparable" idle-vs-interactive design). One flag, tied to
    // whether tracing is enabled at all, keeps both frame kinds paying the same instrumentation
    // overhead.
    const bool tracing = (updateMs != nullptr);

    struct LayerPhaseDuration
    {
        std::string name;
        double durationMs = 0.0;
    };

    std::vector<LayerPhaseDuration> updateLayerDurations;
    std::vector<LayerPhaseDuration> postLayerDurations;
    if (tracing)
    {
        updateLayerDurations.reserve(m_LayerStack.size());
        postLayerDurations.reserve(m_LayerStack.size());
    }

    const auto updateStart = tracing ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    // Update all layers
    for (const auto& layer : m_LayerStack)
    {
        const auto layerStart = tracing ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        guardLayerCall(layer, "onUpdate", [&] { layer->onUpdate(deltaTime); });
        if (tracing)
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
        const auto layerStart = tracing ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        guardLayerCall(layer, "onPostRender", [&] { layer->onPostRender(); });
        if (tracing)
        {
            const auto layerEnd = std::chrono::steady_clock::now();
            postLayerDurations.emplace_back(LayerPhaseDuration{
                .name = layer->getName(), .durationMs = std::chrono::duration<double, std::milli>(layerEnd - layerStart).count()});
        }
    }
    const auto postRenderEnd = tracing ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};

    m_Window->swapBuffers();
    const auto swapEnd = tracing ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    const auto frameEndCounter = tracing ? SDL_GetPerformanceCounter() : 0;

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

    if (tracing)
    {
        recordResizePerfFrameEnd(frameEndCounter);
        const double measuredUpdateMs = std::chrono::duration<double, std::milli>(updateEnd - updateStart).count();
        const double measuredPostMs = std::chrono::duration<double, std::milli>(postRenderEnd - renderEnd).count();
        const double measuredSwapMs = std::chrono::duration<double, std::milli>(swapEnd - postRenderEnd).count();
        const double measuredRenderMs = std::chrono::duration<double, std::milli>(renderEnd - updateEnd).count();
        const bool slowUpdate = measuredUpdateMs >= RESIZE_PERF_TRACE_SLOW_COMPUTE_THRESHOLD_MS;
        const bool slowRender = measuredRenderMs >= RESIZE_PERF_TRACE_SLOW_COMPUTE_THRESHOLD_MS;
        const bool slowPost = measuredPostMs >= RESIZE_PERF_TRACE_SLOW_COMPUTE_THRESHOLD_MS;
        // Wall durations include scheduler/driver waits; these are not CPU utilization.
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

            // Swap has its own detail line below.
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
                spdlog::info("ResizePerfSlowFrame[render]: {:.3f} ms (layer onRender wall time)", measuredRenderMs);
            }
            if (slowPost)
            {
                logTopLayers(postLayerDurations, "post");
            }
            if (slowSwap)
            {
                spdlog::info("ResizePerfSlowFrame[swap]: {:.3f} ms (SDL_GL_SwapWindow wall time)", measuredSwapMs);
            }
        }
    }
}

void Application::stop()
{
    m_Running = false;
}

bool Application::closeRequestAccepted()
{
    WindowCloseEvent event;
    raiseEvent(event);
    return !event.isHandled();
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

double Application::getTime()
{
    return FramePacing::ticksNsToSeconds(SDL_GetTicksNS());
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
