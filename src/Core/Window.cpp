#include "Window.h"

#include <spdlog/spdlog.h>

#include <cassert>

#include <GLFW/glfw3.h>
#include <glad/gl.h>

namespace Core
{

Window::Window(const WindowSpecification& spec) : m_Spec(spec)
{
    spdlog::info("Creating window: {} ({}x{})", m_Spec.Title, m_Spec.Width, m_Spec.Height);

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifndef NDEBUG
    glfwWindowHint(GLFW_OPENGL_DEBUG_CONTEXT, GLFW_TRUE);
#endif

    m_Handle = glfwCreateWindow(static_cast<int>(m_Spec.Width), static_cast<int>(m_Spec.Height), m_Spec.Title.c_str(), nullptr, nullptr);

    if (m_Handle == nullptr)
    {
        spdlog::critical("Failed to create GLFW window");
        assert(false);
        return;
    }

    glfwMakeContextCurrent(m_Handle);

    // Load OpenGL functions using GLAD
    int version = gladLoadGL(glfwGetProcAddress);
    if (version == 0)
    {
        spdlog::critical("Failed to initialize GLAD");
        assert(false);
        return;
    }

    spdlog::info("OpenGL Info:");
    spdlog::info("  Vendor: {}", reinterpret_cast<const char*>(glGetString(GL_VENDOR)));
    spdlog::info("  Renderer: {}", reinterpret_cast<const char*>(glGetString(GL_RENDERER)));
    spdlog::info("  Version: {}", reinterpret_cast<const char*>(glGetString(GL_VERSION)));

    glfwSwapInterval(m_Spec.VSync ? 1 : 0);

    glfwSetWindowUserPointer(m_Handle, this);

    // Framebuffer resize callback
    glfwSetFramebufferSizeCallback(m_Handle,
                                   [](GLFWwindow* window, int width, int height)
                                   {
                                       auto* self = static_cast<Window*>(glfwGetWindowUserPointer(window));
                                       self->m_Spec.Width = static_cast<uint32_t>(width);
                                       self->m_Spec.Height = static_cast<uint32_t>(height);
                                       glViewport(0, 0, width, height);
                                   });
}

Window::~Window()
{
    if (m_Handle != nullptr)
    {
        glfwDestroyWindow(m_Handle);
        m_Handle = nullptr;
    }
}

void Window::swapBuffers() const
{
    glfwSwapBuffers(m_Handle);
}

bool Window::shouldClose() const
{
    return glfwWindowShouldClose(m_Handle) != 0;
}

} // namespace Core
