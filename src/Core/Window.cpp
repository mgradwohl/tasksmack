#include "Window.h"

#include <spdlog/spdlog.h>

#include <cassert>

// clang-format off
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <glad/gl.h>
// clang-format on

#ifdef _WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>

#include <windows.h>
#endif

namespace Core
{

#ifdef _WIN32
namespace
{
// Set window icon from embedded resource on Windows
// This sets both the small icon (title bar, Alt+Tab) and large icon (taskbar)
void setWindowIconFromResource(GLFWwindow* window)
{
    HWND hwnd = glfwGetWin32Window(window);
    if (hwnd == nullptr)
    {
        spdlog::warn("Failed to get Win32 window handle for icon");
        return;
    }

    // Get the module handle for this executable
    HINSTANCE hInstance = GetModuleHandle(nullptr);

    // Load small icon (16x16) for title bar and Alt+Tab
    // MAKEINTRESOURCE(1) refers to IDI_ICON1 (resource ID 1) defined in the .rc file
    HICON hIconSmall = static_cast<HICON>(LoadImage(
        hInstance, MAKEINTRESOURCE(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));

    // Load large icon (32x32 or larger) for taskbar
    HICON hIconBig = static_cast<HICON>(
        LoadImage(hInstance, MAKEINTRESOURCE(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR));

    if (hIconSmall != nullptr)
    {
        SendMessage(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(hIconSmall));
        spdlog::debug("Set small window icon ({}x{})", GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON));
    }
    else
    {
        spdlog::warn("Failed to load small icon from resource");
    }

    if (hIconBig != nullptr)
    {
        SendMessage(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(hIconBig));
        spdlog::debug("Set large window icon ({}x{})", GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON));
    }
    else
    {
        spdlog::warn("Failed to load large icon from resource");
    }
}
} // namespace
#endif

Window::Window(WindowSpecification spec) : m_Spec(std::move(spec))
{
    spdlog::info("Creating window: {} ({}x{})", m_Spec.Title, m_Spec.Width, m_Spec.Height);

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifndef NDEBUG
    glfwWindowHint(GLFW_OPENGL_DEBUG_CONTEXT, GLFW_TRUE);
#endif

    // NOLINTNEXTLINE(cppcoreguidelines-prefer-member-initializer) - glfwWindowHint must be called before glfwCreateWindow
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

#ifdef _WIN32
    // Set window icon from embedded resource (title bar and taskbar)
    setWindowIconFromResource(m_Handle);
#endif
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
