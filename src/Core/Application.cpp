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
} // namespace

Application::Application(ApplicationSpecification spec) : m_Spec(std::move(spec))
{
    // For stack-allocated instances (tests), track in thread-local.
    // Only set if neither ownership mechanism is already active.
    if (!s_Instance && !g_StackApplicationInstance.has_value())
    {
        g_StackApplicationInstance = std::ref(*this);
    }

    spdlog::info("Initializing {} application", m_Spec.Name);

    // Initialize SDL video subsystem
    if (!SDL_Init(SDL_INIT_VIDEO))
    {
        spdlog::critical("Failed to initialize SDL: {}", SDL_GetError());
        throw std::runtime_error("Failed to initialize SDL");
    }

    spdlog::info("SDL initialized: {}", SDL_GetRevision());

    WindowSpecification windowSpec;
    windowSpec.Title = m_Spec.Name;
    windowSpec.Width = m_Spec.Width;
    windowSpec.Height = m_Spec.Height;
    windowSpec.VSync = m_Spec.VSync;
    windowSpec.Borderless = true; // Enable custom title bar

    m_Window = std::make_unique<Window>(windowSpec);
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

    spdlog::info("Entering main loop");

    while (m_Running)
    {
        // Process SDL events
        SDL_Event sdlEvent;
        while (SDL_PollEvent(&sdlEvent))
        {
            // Let layers handle raw SDL events (for ImGui integration and input handling)
            for (const auto& layer : m_LayerStack)
            {
                layer->onSDLEvent(&sdlEvent);
            }

            // Only translate window close events to our event system for clean shutdown coordination
            // All other input events are handled via SDL directly
            if (sdlEvent.type == SDL_EVENT_QUIT || sdlEvent.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
            {
                WindowCloseEvent event;
                raiseEvent(event);
                if (!event.isHandled())
                {
                    stop();
                }
            }
        }

        if (m_Window->shouldClose())
        {
            stop();
            break;
        }

        const float currentTime = getTime();
        float deltaTime = currentTime - lastTime;
        lastTime = currentTime;

        // Clamp delta time to avoid huge jumps
        constexpr float MAX_DELTA_TIME = 0.1F;
        deltaTime = std::min(deltaTime, MAX_DELTA_TIME);

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

    spdlog::info("Exiting main loop");
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
