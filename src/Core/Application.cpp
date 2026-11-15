#include "Application.h"

#include "Core/EnvRead.h"
#include "Core/EnvUtils.h"
#include "Core/Event.h"
#include "Core/FrameLoopState.h"
#include "Core/FramePacing.h"
#include "Core/FrameWake.h"
#include "Core/GraphicsReset.h"
#ifdef _WIN32
#include "Core/IdleWaitTimer.h" // Only Windows paces its idle waits with it (#3712)
#endif
#include "Core/Layer.h"
#include "Core/ResizePerfOperation.h"
#include "Core/ResizePerfTrace.h"
#ifndef _WIN32
#include "Core/RuntimeDir.h"
#endif
#include "Core/VideoBackend.h"
#include "Core/Window.h"
#include "Core/WindowEvents.h"
#include "Core/WindowVisibility.h"
#include "Core/X11ConnectionLoss.h"
#include "Domain/PublicationWake.h"
#include "Platform/ThreadName.h"
#include "version.h"

#include <SDL3/SDL.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
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
// At least the static floor's period (#3156): a static tab draws only every STATIC_FRAME_FLOOR_INTERVAL,
// and what counts deltaTime as a clock -- a result line's timeout, an open Process Details section's
// re-read interval -- must see that time pass, not 0.1 s of it a frame. Easing toward a sample with a
// longer delta only settles further, as it would have in real time.
constexpr float MAX_DELTA_TIME = 1.0F;

// The refresh rate assumed when SDL does not report the display's (#1126).
constexpr double FALLBACK_REFRESH_HZ = 60.0;

// The idle, hidden and static floor periods, the frame rate caps, the drain's budgets, the graces and
// the trace's log cadence: see FrameLoopState (#3791).
static_assert(static_cast<double>(MAX_DELTA_TIME) * 1000.0 >= FrameLoopState::STATIC_FRAME_FLOOR_MS,
              "deltaTime covers a floor frame's real time");

// Whether an OS maximize of the borderless window is replaced by the client-side one
// (Window::adoptSystemMaximize(), #1208): on Windows only; see the SystemMaximized case in
// FrameLoopState::drain().
#ifdef _WIN32
constexpr bool ADOPT_SYSTEM_MAXIMIZE = true;
#else
constexpr bool ADOPT_SYSTEM_MAXIMIZE = false;
#endif
constexpr int RESIZE_PERF_TRACE_TOP_LAYER_COUNT = 3;

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
                 TASKSMACK_VERSION_FULL,
                 TASKSMACK_BUILD_TYPE,
                 TASKSMACK_COMPILER_ID,
                 TASKSMACK_COMPILER_VERSION,
                 TASKSMACK_BUILD_DATE,
                 TASKSMACK_BUILD_TIME);
}

#ifndef _WIN32
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
        if (!existingPath.empty() && RuntimeDir::isUsableRuntimeDir(existingPath))
        {
            return; // valid path already provided by session manager
        }

        spdlog::warn("Ignoring invalid XDG_RUNTIME_DIR='{}'; using a safe fallback", existingPath);
    }

    const uid_t uid = getuid();
    const std::string systemdPath = std::format("/run/user/{}", uid);
    const std::string fallbackPath = std::format("/tmp/runtime-{}", uid);

    const auto chosen = RuntimeDir::chooseRuntimeDir(systemdPath, fallbackPath);
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
// them (#778). Returns whether the call completed, for the one caller that must know: a layer
// failing to rebuild after a GPU reset fails the context replacement (#3484).
template<typename Call> bool guardLayerCall(const std::unique_ptr<Layer>& layer, std::string_view phase, const Call& call)
{
    try
    {
        call();
        return true;
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
    return false;
}

// The publication wake (#3156 stage 2): Domain::PublicationWake's handler while run() runs. A sampler
// or a background task that publishes calls it from its own thread; it pushes one SDL user event,
// coalesced (WakeCoalescer), which wakes the loop's idle wait for the frame that shows the new data.
// Process-wide, as the hook is: one loop runs at a time.
struct PublicationWakeTarget
{
    std::atomic<std::uint32_t> eventType{0}; // 0 while no loop runs, or SDL had no event type to give
    WakeCoalescer coalescer;
};

PublicationWakeTarget& publicationWakeTarget() noexcept
{
    static PublicationWakeTarget target;
    return target;
}

void pushPublicationWake() noexcept
{
    PublicationWakeTarget& target = publicationWakeTarget();
    const std::uint32_t eventType = target.eventType.load();
    if (eventType == 0)
    {
        return;
    }
    // SDL_PushEvent() is thread-safe, and wakes a wait in SDL_WaitEventTimeout() on the loop thread.
    static_cast<void>(target.coalescer.notify(
        [eventType]
        {
            SDL_Event event{};
            event.type = eventType;
            return SDL_PushEvent(&event);
        }));
}

// Installs the publication wake for one run() of the loop, and removes it however run() ends, before
// the layers detach and stop their samplers. A notify() already past the hook's load may still push
// once after that: harmless, since SDL keeps its queue until SDL_Quit(), which runs only after the
// layers' samplers and tasks have stopped (~Application()).
class PublicationWakeInstall
{
  public:
    explicit PublicationWakeInstall(std::uint32_t eventType) noexcept
    {
        PublicationWakeTarget& target = publicationWakeTarget();
        target.coalescer.reset();
        target.eventType.store(eventType);
        static_cast<void>(Domain::PublicationWake::setHandler(&pushPublicationWake));
    }

    ~PublicationWakeInstall()
    {
        static_cast<void>(Domain::PublicationWake::setHandler(nullptr));
        publicationWakeTarget().eventType.store(0);
    }

    PublicationWakeInstall(const PublicationWakeInstall&) = delete;
    PublicationWakeInstall& operator=(const PublicationWakeInstall&) = delete;
    PublicationWakeInstall(PublicationWakeInstall&&) = delete;
    PublicationWakeInstall& operator=(PublicationWakeInstall&&) = delete;
};

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
#ifdef _WIN32
        // SDL raises the whole machine's timer to 1 ms (timeBeginPeriod(1)) when its clock starts,
        // unless this hint says otherwise (#3712): a cost to every core's idle states and to the
        // battery, for as long as the window is visible. The frame loop's waits end on time without it
        // (IdleWaitTimer). Set before anything starts SDL's clock: getTime() does, headless too. At
        // normal priority, so an SDL_TIMER_RESOLUTION environment variable still wins.
        if (!SDL_SetHint(SDL_HINT_TIMER_RESOLUTION, "0"))
        {
            spdlog::info("SDL_HINT_TIMER_RESOLUTION is set by the environment; SDL keeps that timer resolution");
        }
#endif
        m_ResizePerfTraceEnabled = resizePerfTraceRequested();
        resizePerfOperations() = {};
        spdlog::info("Initializing {} application", m_Spec.Name);
        if (m_ResizePerfTraceEnabled)
        {
            spdlog::info("Resize performance tracing enabled via {}", TRACE_RESIZE_PERF_ENV_VAR);
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
        // Losing the X connection ends the process without running static destructors under the
        // sampler threads, instead of Xlib's exit() (#2657)
        if ((VideoBackend::isX11() || VideoBackend::isXWaylandFallback()) && !X11ConnectionLoss::install())
        {
            spdlog::warn("Could not install the X connection-loss handler; libX11 isn't loaded");
        }

        WindowSpecification windowSpec;
        windowSpec.Title = m_Spec.Name;
        windowSpec.Width = m_Spec.Width;
        windowSpec.Height = m_Spec.Height;
        windowSpec.VSync = m_Spec.VSync;
        windowSpec.Hidden = m_Spec.StartHidden;
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
    for (const auto& layer : std::views::reverse(m_LayerStack))
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

// What run()'s loop (FrameLoopState) calls out to (#3791): the clock, SDL's paced delay, event queue
// and wait, renderFrame() with the frame's deltaTime, the layers, the window, the vsync setting, the GPU
// recovery and the publication wake.
class Application::LoopHost final : public IFrameLoopHost
{
  public:
    LoopHost(Application& app, const double lastTime, const std::uint32_t publicationWakeType, WakeCoalescer& wakeCoalescer)
        : m_App(app), m_Clock(lastTime), m_PublicationWakeType(publicationWakeType), m_WakeCoalescer(wakeCoalescer)
    {
#ifdef _WIN32
        if (!m_IdleWaitTimer.active())
        {
            spdlog::warn("Frame pacing: no high-resolution timer; idle waits end on the system timer's tick");
        }
#endif
    }

    [[nodiscard]] double now() override
    {
        return getTime();
    }

    void delayPreciseNs(const std::uint64_t nanoseconds) override
    {
        SDL_DelayPrecise(nanoseconds);
    }

    bool renderFrame(const bool resizeTriggeredFrame, FrameTimes* times) override
    {
        return m_App.renderFrame(computeDeltaTime(),
                                 resizeTriggeredFrame,
                                 times != nullptr ? &times->updateMs : nullptr,
                                 times != nullptr ? &times->renderMs : nullptr,
                                 times != nullptr ? &times->postRenderMs : nullptr,
                                 times != nullptr ? &times->swapMs : nullptr);
    }

    // On Windows the wait's timeout is ended on time by a high-resolution timer (#3712): SDL's own
    // ends on a system timer tick, 15.625 ms apart now that TaskSmack no longer raises the timer to
    // 1 ms (see SDL_HINT_TIMER_RESOLUTION in the constructor).
    FramePacing::IdleWaitOutcome waitForEvent(SDL_Event& event, const int timeoutMs) override
    {
        const auto wait = [timeoutMs](SDL_Event& waited)
        {
            return SDL_WaitEventTimeout(&waited, timeoutMs);
        };
        const auto poll = [](SDL_Event& polled)
        {
            return SDL_PollEvent(&polled);
        };
#ifdef _WIN32
        return FramePacing::waitForIdleEventOnTime(m_IdleWaitTimer, timeoutMs, event, wait, poll);
#else
        return FramePacing::waitForIdleEvent(event, wait, poll);
#endif
    }

    [[nodiscard]] int boundWaitMs(const int waitMs, const double nowSeconds) override
    {
        return m_App.m_GraphicsRecovery.boundWaitMs(waitMs, nowSeconds);
    }

    void hiddenUpdate() override
    {
        for (const auto& layer : m_App.m_LayerStack)
        {
            guardLayerCall(layer, "onHiddenUpdate", [&] { layer->onHiddenUpdate(); });
        }
    }

    void noteIdleWait(const FramePacing::IdleWaitOutcome outcome) override
    {
        m_App.m_IdleWaits.record(outcome);
    }

    [[nodiscard]] std::chrono::steady_clock::time_point drainClock() override
    {
        return std::chrono::steady_clock::now();
    }

    bool pollEvent(SDL_Event& event) override
    {
        return SDL_PollEvent(&event);
    }

    bool takePublicationWake(const SDL_Event& event) override
    {
        if (m_PublicationWakeType == 0 || event.type != m_PublicationWakeType)
        {
            return false;
        }
        m_WakeCoalescer.taken();
        ++m_App.m_PublicationWakesTaken;
        return true;
    }

    void dispatchToLayers(SDL_Event& event) override
    {
        for (const auto& layer : m_App.m_LayerStack)
        {
            guardLayerCall(layer, "onSDLEvent", [&] { layer->onSDLEvent(&event); });
        }
    }

    void closeOnRequest() override
    {
        if (m_App.closeRequestAccepted())
        {
            m_App.stop();
        }
    }

    void quit() override
    {
        m_App.stop();
    }

    [[nodiscard]] std::pair<int, int> windowSizeInPixels() override
    {
        return m_App.m_Window->getSizeInPixels();
    }

    void raiseResized(const std::pair<int, int> pixelSize) override
    {
        WindowResizedEvent resizeEvent(pixelSize.first, pixelSize.second);
        m_App.raiseEvent(resizeEvent);
    }

    void windowGeometryChanged() override
    {
        m_App.m_Window->handleGeometryChanged();
    }

    void displayChanged() override
    {
        m_App.refreshDisplayRate();
    }

    void systemMaximized() override
    {
        m_App.m_Window->handleSystemMaximized(ADOPT_SYSTEM_MAXIMIZE);
    }

    void systemRestored() override
    {
        m_App.m_Window->handleSystemRestored();
    }

    void shellMinimized() override
    {
        static_cast<void>(m_App.m_Window->restoreForShellMinimize());
    }

    [[nodiscard]] double interactionRedrawUntil() override
    {
        return m_App.m_InteractionRedrawUntil;
    }

    void setInteractionRedrawUntil(const double seconds) override
    {
        m_App.m_InteractionRedrawUntil = seconds;
    }

    [[nodiscard]] bool takeGeometryChanged() override
    {
        return std::exchange(m_App.m_WindowGeometryChangedThisFrame, false);
    }

    [[nodiscard]] double displayRefreshHz() override
    {
        return m_App.m_DisplayRefreshHz;
    }

    [[nodiscard]] bool vsyncRequested() override
    {
        return m_App.m_Spec.VSync;
    }

    [[nodiscard]] bool vsyncDisabledForInteraction() override
    {
        return m_App.m_VsyncDisabledForInteraction;
    }

    // While drawing is suspended after a GPU reset (#3484) there is no working context to set it on:
    // only the wanted state is kept, and the replacement applies it.
    void setVsyncDisabledForInteraction(const bool disabled) override
    {
        if (!m_App.drawingSuspended())
        {
            Window::setVSync(!disabled);
        }
        m_App.m_VsyncDisabledForInteraction = disabled;
    }

    bool closeOnWindowRequest() override
    {
        if (!m_App.m_Window->shouldClose())
        {
            return false;
        }
        m_App.m_Window->clearCloseRequest();
        if (!m_App.closeRequestAccepted())
        {
            return false;
        }
        m_App.stop();
        return true;
    }

    [[nodiscard]] bool isMinimized() override
    {
        return m_App.m_Window->isMinimized();
    }

    void pollGraphicsRecovery() override
    {
        m_App.pollGraphicsRecovery();
    }

    [[nodiscard]] WindowVisibility::HiddenState pollHiddenState() override
    {
        m_App.m_Window->pollVisibility();
        return m_App.m_Window->hiddenState();
    }

    void pausePublicationWakes(const bool paused) override
    {
        m_WakeCoalescer.setPaused(paused);
    }

  private:
    float computeDeltaTime()
    {
        const FramePacing::FrameTick frame = m_Clock.tick(getTime(), MAX_DELTA_TIME);
        m_App.m_LastFrameIntervalSeconds = frame.intervalSeconds;
        return frame.deltaSeconds;
    }

    // NOLINTNEXTLINE(cppcoreguidelines-avoid-const-or-ref-data-members) - lives only inside run(), which the Application outlives
    Application& m_App;
    FramePacing::FrameClock m_Clock;
    std::uint32_t m_PublicationWakeType;
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-const-or-ref-data-members) - run()'s PublicationWakeInstall outlives the host
    WakeCoalescer& m_WakeCoalescer;
#ifdef _WIN32
    IdleWaitTimer m_IdleWaitTimer;
#endif
};

void Application::refreshDisplayRate()
{
    const double queried = m_Window->getDisplayRefreshRate();
    const double effective = FramePacing::effectiveRefreshHz(queried, FALLBACK_REFRESH_HZ);
    // Logged only for a real change: SDL can report the same mode again with a slightly
    // different rational rate (59.97 then 59.98 Hz) when the window first lands on a display.
    constexpr double LOG_CHANGE_HZ = 0.5;
    if (std::abs(effective - m_DisplayRefreshHz) >= LOG_CHANGE_HZ)
    {
        spdlog::info("Frame pacing: display refresh {:.2f} Hz{}", effective, FramePacing::isUsableRefreshHz(queried) ? "" : " (assumed)");
    }
    m_DisplayRefreshHz = effective;
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

    const double lastTime = getTime();

    // The display's refresh rate, which frame pacing rounds to (#1126); re-read on a display change.
    refreshDisplayRate();

    // The publication wake (#3156 stage 2): a static frame waits for input, for this or for the slow
    // floor. Without an event type (SDL has a few thousand), wakes are off and the floor still draws.
    const std::uint32_t publicationWakeType = SDL_RegisterEvents(1);
    if (publicationWakeType == 0)
    {
        spdlog::warn("Frame pacing: no SDL event type for the publication wake; static tabs redraw at the floor: {}", SDL_GetError());
    }
    const PublicationWakeInstall publicationWake(publicationWakeType);

    m_InteractionRedrawUntil = 0.0;

    // Everything the loop keeps between and within iterations (#3791); each iteration is FrameLoopState::iterate().
    FrameLoopState loop;
    loop.lastResizeTraceLogTime = getTime();
    loop.lastFrameStart = getTime();
    LoopHost host(*this, lastTime, publicationWakeType, publicationWakeTarget().coalescer);

    loop.lastResizePixelSize = m_Window->getSizeInPixels();
    // Windows only (see FrameLoopState::resizePresentMinInterval).
    loop.resizePresentMinInterval = VideoBackend::isWindows() ? FramePacing::RESIZE_PRESENT_MIN_INTERVAL_SECONDS : 0.0;

    spdlog::info("Entering main loop");

    // m_ResizePerfTraceEnabled is set once, by the constructor.
    while (m_Running)
    {
        // False when a close the window asked for stopped the app: nothing more this iteration.
        if (!loop.iterate(host, m_ResizePerfTraceEnabled))
        {
            break;
        }
    }

    if (m_ResizePerfTraceEnabled)
    {
        loop.finishTracedLoop(SDL_GetPerformanceCounter());
        logResizePerfTraceSummary(loop.resizeTraceStats, "shutdown");
        logResizePerfOperationSummary();
        logResizePerfAnchor();
    }

    spdlog::info("Exiting main loop");
}

bool Application::renderFrame(
    float deltaTime, bool resizeTriggeredFrame, double* updateMs, double* renderMs, double* postRenderMs, double* swapMs)
{
    // After a GPU reset whose context replacement failed (#3484) the layers have no renderer: no
    // frame, so no GL call, while run() keeps pumping events and retries the replacement.
    if (drawingSuspended())
    {
        return false;
    }

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
                .name = layer->getName(),
                .durationMs = std::chrono::duration<double, std::milli>(layerEnd - layerStart).count(),
            });
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
                .name = layer->getName(),
                .durationMs = std::chrono::duration<double, std::milli>(layerEnd - layerStart).count(),
            });
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
    return true;
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

void Application::checkGraphicsReset()
{
    const GraphicsRecoveryState state = m_GraphicsRecovery.state();
    if ((state != GraphicsRecoveryState::Rendering && state != GraphicsRecoveryState::Settling) || !m_Window->reportsGraphicsReset())
    {
        return;
    }
    // LCOV_EXCL_START - needs a real GPU reset; GraphicsResetMonitor's decisions are unit-tested
    const std::uint32_t status = m_Window->graphicsResetStatus();
    const GraphicsResetAction action = m_GraphicsReset.observe(status, getTime());
    if (action == GraphicsResetAction::None)
    {
        return;
    }
    if (action == GraphicsResetAction::Wait)
    {
        if (status != GL_RESET_STATUS_NO_ERROR && m_GraphicsResetsLogged != m_GraphicsReset.resets())
        {
            m_GraphicsResetsLogged = m_GraphicsReset.resets();
            spdlog::warn("GPU reset reported ({} context reset); drawing paused until it finishes and the OpenGL context is replaced",
                         graphicsResetStatusName(status));
        }
        // Drawing, and every GL call frames and events make, stops now: the context is lost.
        m_GraphicsRecovery.lost(graphicsRecoverySteps());
        return;
    }
    spdlog::warn("Replacing the OpenGL context after GPU reset {} ({} context reset)",
                 m_GraphicsReset.resets(),
                 graphicsResetStatusName(m_GraphicsReset.lostStatus()));
    noteGraphicsRecovery(m_GraphicsRecovery.replace(getTime(), graphicsRecoverySteps()));
    // LCOV_EXCL_STOP
}

// LCOV_EXCL_START - runs only after a GPU reset; GraphicsContextRecovery's decisions are unit-tested
GraphicsContextRecoverySteps Application::graphicsRecoverySteps()
{
    return GraphicsContextRecoverySteps{
        .loseLayers =
            [this]
        {
            for (const auto& layer : std::views::reverse(m_LayerStack))
            {
                guardLayerCall(layer, "onGraphicsContextLost", [&] { layer->onGraphicsContextLost(); });
            }
        },
        .recreateContext = [this] { return m_Window->recreateGLContext(); },
        .advanceGeneration = [] { advanceGraphicsContextGeneration(); },
        .restoreLayers =
            [this]
        {
            // The swap interval belongs to the context: the one the frame loop wants now, which it
            // only remembered while drawing was suspended.
            Window::setVSync(m_Spec.VSync && !m_VsyncDisabledForInteraction);
            bool allRestored = true;
            for (const auto& layer : m_LayerStack)
            {
                // A layer that throws (the ImGui renderer failing to start) leaves the frame with no
                // renderer: the whole replacement has failed, not just that layer's call.
                allRestored =
                    guardLayerCall(layer, "onGraphicsContextRestored", [&] { layer->onGraphicsContextRestored(); }) && allRestored;
            }
            return allRestored;
        },
    };
}

void Application::noteGraphicsRecovery(const GraphicsRecoveryOutcome outcome)
{
    // Only what an attempt did: a check that found nothing due yet has nothing new to say.
    if (!outcome.attempted)
    {
        return;
    }
    switch (outcome.state)
    {
    case GraphicsRecoveryState::Rendering:
        spdlog::info("OpenGL context replaced and every layer rebuilt (attempt {})", m_GraphicsRecovery.attempts());
        break;
    case GraphicsRecoveryState::Settling: // an attempt never ends there: only lost() enters it
        break;
    case GraphicsRecoveryState::Suspended:
        spdlog::error("Could not replace the OpenGL context (attempt {}); drawing is suspended and it will be tried again",
                      m_GraphicsRecovery.attempts());
        break;
    case GraphicsRecoveryState::GaveUp:
        spdlog::critical("Could not replace the OpenGL context after a GPU reset in {} attempts; TaskSmack is closing",
                         m_GraphicsRecovery.attempts());
        stop(); // Not a close request a layer could veto: there is no renderer to carry on with
        break;
    }
}

// LCOV_EXCL_STOP

void Application::pollGraphicsRecovery()
{
    if (m_GraphicsRecovery.state() == GraphicsRecoveryState::Suspended)
    // LCOV_EXCL_START - only after a failed replacement; GraphicsContextRecovery's retries are unit-tested
    {
        noteGraphicsRecovery(m_GraphicsRecovery.retryIfDue(getTime(), graphicsRecoverySteps()));
        return;
    }
    // LCOV_EXCL_STOP
    checkGraphicsReset(); // Rendering: a new reset; Settling: whether it is over
}

bool Application::drawingSuspended() const noexcept
{
    return !m_GraphicsRecovery.drawing();
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
