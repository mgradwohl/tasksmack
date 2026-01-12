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

Application* Application::s_Instance = nullptr;

Application::Application(ApplicationSpecification spec) : m_Spec(std::move(spec))
{
    assert(s_Instance == nullptr && "Application already exists!");
    s_Instance = this;

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

    s_Instance = nullptr;
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
    assert(s_Instance != nullptr && "Application does not exist!");
    return *s_Instance;
}

float Application::getTime()
{
    // SDL_GetTicks returns milliseconds as Uint64
    return static_cast<float>(SDL_GetTicks()) / 1000.0F;
}
} // namespace Core
