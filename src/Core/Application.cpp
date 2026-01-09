#include "Application.h"

#include "Core/Window.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cassert>
#include <memory>
#include <ranges>
#include <stdexcept>
#include <utility>

// clang-format off
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
// clang-format on

namespace Core
{

Application* Application::s_Instance = nullptr;

namespace
{
void glfwErrorCallback(int error, const char* description)
{
    spdlog::error("[GLFW Error {}]: {}", error, description);
}
} // namespace

Application::Application(ApplicationSpecification spec) : m_Spec(std::move(spec))
{
    assert(s_Instance == nullptr && "Application already exists!");
    s_Instance = this;

    spdlog::info("Initializing {} application", m_Spec.Name);

    glfwSetErrorCallback(glfwErrorCallback);

    if (glfwInit() == GLFW_FALSE)
    {
        spdlog::critical("Failed to initialize GLFW");
        throw std::runtime_error("Failed to initialize GLFW");
    }

    WindowSpecification windowSpec;
    windowSpec.Title = m_Spec.Name;
    windowSpec.Width = m_Spec.Width;
    windowSpec.Height = m_Spec.Height;
    windowSpec.VSync = m_Spec.VSync;

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

    glfwTerminate();

    s_Instance = nullptr;
}

void Application::run()
{
    m_Running = true;

    float lastTime = getTime();

    spdlog::info("Entering main loop");

    while (m_Running)
    {
        glfwPollEvents();

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

Application& Application::get()
{
    assert(s_Instance != nullptr && "Application does not exist!");
    return *s_Instance;
}

float Application::getTime()
{
    return static_cast<float>(glfwGetTime());
}

} // namespace Core
