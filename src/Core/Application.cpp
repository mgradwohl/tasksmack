#include "Application.h"

#include "Core/Event.h"
#include "Core/Window.h"
#include "Core/WindowEvents.h"

#include <SDL3/SDL.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string_view>
#include <utility>

#ifndef _WIN32
#include <sys/stat.h>
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

[[nodiscard]] bool isEnvFlagEnabled(const char* value) noexcept
{
    if (value == nullptr)
    {
        return false;
    }

    const std::string_view text(value);
    if (text.empty())
    {
        return false;
    }

    // Case-insensitive check: "0", "false", "off", "no" (any casing) are treated as disabled.
    const auto ciEqual = [](const std::string_view a, const std::string_view b) noexcept
    {
        if (a.size() != b.size())
        {
            return false;
        }
        for (std::size_t i = 0; i < a.size(); ++i)
        {
            if (std::tolower(static_cast<unsigned char>(a[i])) != b[i])
            {
                return false;
            }
        }
        return true;
    };

    // b must already be lowercase
    return !ciEqual(text, "0") && !ciEqual(text, "false") && !ciEqual(text, "off") && !ciEqual(text, "no");
}

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

    void recordEventBatch(std::uint32_t eventCount, std::uint32_t resizeEventCount, double durationMs) noexcept
    {
        ++eventBatches;
        drainedEvents += eventCount;
        resizeEvents += resizeEventCount;
        maxEventsPerBatch = std::max(maxEventsPerBatch, eventCount);
        drainMs += durationMs;
        maxDrainMs = std::max(maxDrainMs, durationMs);
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

void logResizePerfTraceSummary(const ResizePerfTraceStats& stats, const std::string_view reason)
{
    if (!stats.hasSamples())
    {
        return;
    }

    const auto avg = [](const double total, const std::uint32_t count) noexcept -> double
    {
        return (count == 0) ? 0.0 : (total / static_cast<double>(count));
    };

    spdlog::info(
        "ResizePerf[{}]: batches={} events={} resizeEvents={} maxBatchEvents={} frames={} resizeFrames={} drain avg/max={:.3f}/{:.3f} ms "
        "update avg/max={:.3f}/{:.3f} ms render avg/max={:.3f}/{:.3f} ms post avg/max={:.3f}/{:.3f} ms swap avg/max={:.3f}/{:.3f} ms",
        reason,
        stats.eventBatches,
        stats.drainedEvents,
        stats.resizeEvents,
        stats.maxEventsPerBatch,
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

#ifndef _WIN32
[[nodiscard]] bool hasSafeOwnerOnlyPermissions(const std::filesystem::perms perms)
{
    constexpr auto forbidden = std::filesystem::perms::group_all | std::filesystem::perms::others_all;
    return (perms & forbidden) == std::filesystem::perms::none;
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
    // NOLINTNEXTLINE(concurrency-mt-unsafe) - called once before any threads start
    ::setenv("XDG_RUNTIME_DIR", chosen->c_str(), 1);
    spdlog::info("Using XDG_RUNTIME_DIR='{}'", *chosen);
}
#endif
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

        WindowSpecification windowSpec;
        windowSpec.Title = m_Spec.Name;
        windowSpec.Width = m_Spec.Width;
        windowSpec.Height = m_Spec.Height;
        windowSpec.VSync = m_Spec.VSync;
        windowSpec.Borderless = true; // Enable custom title bar

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
    float lastResizeTraceLogTime = getTime();

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
        const auto eventDrainStart = traceResizePerfThisFrame ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        while (SDL_PollEvent(&sdlEvent))
        {
            hadEvents = true;
            ++drainedEventCount;
            // Let layers handle raw SDL events (for ImGui integration and input handling)
            for (const auto& layer : m_LayerStack)
            {
                layer->onSDLEvent(&sdlEvent);
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
        }
        const auto eventDrainEnd = traceResizePerfThisFrame ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        if (traceResizePerfThisFrame)
        {
            resizeTraceStats.recordEventBatch(
                drainedEventCount, resizeEventCount, std::chrono::duration<double, std::milli>(eventDrainEnd - eventDrainStart).count());
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
        const bool forceInteractionRedraw = getTime() < m_InteractionRedrawUntil;
        const bool tracingInteraction = m_ResizePerfTraceEnabled && (needsResizeRedraw || forceInteractionRedraw || (resizeEventCount > 0));
        if (m_ResizePerfTraceEnabled && wasTracingInteraction && !tracingInteraction)
        {
            logResizePerfTraceSummary(resizeTraceStats, "interaction-end");
            resizeTraceStats = {};
        }
        if ((needsResizeRedraw || forceInteractionRedraw) && !m_Window->isMinimized())
        {
            double updateMs = 0.0;
            double renderMs = 0.0;
            double postRenderMs = 0.0;
            double swapMs = 0.0;
            renderFrame(computeDeltaTime(),
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
            const bool keepInteractionRedrawActive = getTime() < m_InteractionRedrawUntil;
            if (!keepInteractionRedrawActive)
            {
                const int sleepMs = m_Window->isMinimized() ? MINIMIZED_FRAME_SLEEP_MS : IDLE_FRAME_SLEEP_MS;
                SDL_WaitEventTimeout(nullptr, sleepMs);
            }
        }

        if (!didImmediateResizeRedraw)
        {
            double updateMs = 0.0;
            double renderMs = 0.0;
            double postRenderMs = 0.0;
            double swapMs = 0.0;
            renderFrame(computeDeltaTime(),
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
    }

    if (m_ResizePerfTraceEnabled)
    {
        logResizePerfTraceSummary(resizeTraceStats, "shutdown");
    }

    spdlog::info("Exiting main loop");
}

void Application::renderFrame(float deltaTime, double* updateMs, double* renderMs, double* postRenderMs, double* swapMs)
{
    const bool tracing = (updateMs != nullptr);
    const auto updateStart = tracing ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    // Update all layers
    for (const auto& layer : m_LayerStack)
    {
        layer->onUpdate(deltaTime);
    }
    const auto updateEnd = tracing ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};

    // Render all layers
    for (const auto& layer : m_LayerStack)
    {
        layer->onRender();
    }
    const auto renderEnd = tracing ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};

    // Post-render (for ImGui frame end, etc.)
    for (const auto& layer : m_LayerStack)
    {
        layer->onPostRender();
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
        layer->onEvent(event);
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
