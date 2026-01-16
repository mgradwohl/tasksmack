#include "Application.h"

#include "Core/Event.h"
#include "Core/Window.h"
#include "Core/WindowEvents.h"

#include <SDL3/SDL.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cassert>
#include <memory>
#include <ranges>
#include <stdexcept>
#include <utility>

namespace Core
{

std::unique_ptr<Application> Application::s_Instance = nullptr;

// Track stack-allocated Application for tests and fallback access.
// When setInstance() is called, this stays null (s_Instance owns the instance).
// When Application is created on the stack (tests), this points to it.
thread_local Application* g_StackApplicationInstance = nullptr;

Application::Application(ApplicationSpecification spec) : m_Spec(std::move(spec))
{
    // For stack-allocated instances (tests), track in thread-local.
    // setInstance() will populate s_Instance instead and set g_StackApplicationInstance = nullptr.
    if (!s_Instance && !g_StackApplicationInstance)
    {
        g_StackApplicationInstance = this;
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
    // Detach layers in reverse order
    for (auto& layer : std::views::reverse(m_LayerStack))
    {
        layer->onDetach();
    }
    m_LayerStack.clear();
    m_Window.reset();

    SDL_Quit();

    // Clear thread-local pointer if this was a stack-allocated instance
    // Note: Assumes Application is destroyed on the same thread it was created (SDL requirement)
    if (g_StackApplicationInstance == this)
    {
        g_StackApplicationInstance = nullptr;
    }
    // s_Instance will be reset by unique_ptr destructor; no manual reset needed
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
    if (s_Instance)
    {
        return *s_Instance;
    }
    if (g_StackApplicationInstance)
    {
        return *g_StackApplicationInstance;
    }
    // No Application instance found
    assert((s_Instance != nullptr || g_StackApplicationInstance != nullptr) && "Application does not exist!");
    throw std::runtime_error("Application does not exist!");
}

float Application::getTime()
{
    // SDL_GetTicks returns milliseconds as Uint64
    return static_cast<float>(SDL_GetTicks()) / 1000.0F;
}

/// Set the global application instance (called from main()).
/// This ensures the Application is managed by std::unique_ptr with proper RAII cleanup.
///
/// THREAD-SAFETY: This method MUST ONLY be called from the main thread during initialization,
/// before any other threads access Application::get(). No synchronization is performed.
void Application::setInstance(std::unique_ptr<Application> app)
{
    // Clear any stack-allocated instance pointer when taking unique_ptr ownership
    g_StackApplicationInstance = nullptr;
    Application::s_Instance = std::move(app);
}

} // namespace Core
