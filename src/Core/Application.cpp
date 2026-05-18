#include "Application.h"

#include "Core/Event.h"
#include "Core/Window.h"
#include "Core/WindowEvents.h"

#include <SDL3/SDL.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cassert>
#include <functional>
#include <memory>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <utility>

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
        spdlog::info("Initializing {} application", m_Spec.Name);

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

    float forceInteractionRedrawUntil = 0.0F;

    spdlog::info("Entering main loop");

    while (m_Running)
    {
        // Process SDL events
        bool hadEvents = false;
        bool needsResizeRedraw = false;
        SDL_Event sdlEvent;
        while (SDL_PollEvent(&sdlEvent))
        {
            hadEvents = true;
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
                WindowResizedEvent resizeEvent(sdlEvent.window.data1, sdlEvent.window.data2);
                raiseEvent(resizeEvent);
                needsResizeRedraw = true;
                forceInteractionRedrawUntil = getTime() + INTERACTION_REDRAW_GRACE_SECONDS;
            }
            else if (sdlEvent.type == SDL_EVENT_WINDOW_RESIZED || sdlEvent.type == SDL_EVENT_WINDOW_EXPOSED)
            {
                const auto [pixelW, pixelH] = m_Window->getSizeInPixels();
                if (pixelW > 0 && pixelH > 0)
                {
                    WindowResizedEvent resizeEvent(pixelW, pixelH);
                    raiseEvent(resizeEvent);
                    needsResizeRedraw = true;
                    forceInteractionRedrawUntil = getTime() + INTERACTION_REDRAW_GRACE_SECONDS;
                }
            }
            else if (sdlEvent.type == SDL_EVENT_WINDOW_MOVED)
            {
                forceInteractionRedrawUntil = getTime() + INTERACTION_REDRAW_GRACE_SECONDS;
            }
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
        const bool forceInteractionRedraw = getTime() < forceInteractionRedrawUntil;
        if ((needsResizeRedraw || forceInteractionRedraw) && !m_Window->isMinimized())
        {
            renderFrame(computeDeltaTime());
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
            const bool keepInteractionRedrawActive = getTime() < forceInteractionRedrawUntil;
            if (!keepInteractionRedrawActive)
            {
                const int sleepMs = m_Window->isMinimized() ? MINIMIZED_FRAME_SLEEP_MS : IDLE_FRAME_SLEEP_MS;
                SDL_WaitEventTimeout(nullptr, sleepMs);
            }
        }

        if (!didImmediateResizeRedraw)
        {
            renderFrame(computeDeltaTime());
        }
    }

    spdlog::info("Exiting main loop");
}

void Application::renderFrame(float deltaTime)
{
    // Update all layers
    for (const auto& layer : m_LayerStack)
    {
        layer->onUpdate(deltaTime);
    }

    // Render all layers
    for (const auto& layer : m_LayerStack)
    {
        layer->onRender();
    }

    // Post-render (for ImGui frame end, etc.)
    for (const auto& layer : m_LayerStack)
    {
        layer->onPostRender();
    }

    m_Window->swapBuffers();
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
