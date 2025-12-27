#include "Window.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <bit>
#include <cassert>
#include <string_view>
#include <utility>

// clang-format off
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <glad/gl.h>
// clang-format on

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#define GLFW_EXPOSE_NATIVE_WIN32

#include <GLFW/glfw3native.h>
#endif

namespace Core
{

namespace
{
[[nodiscard]] int clampWindowDimension(const int value) noexcept
{
    return std::clamp(value, 1, 16384);
}

[[nodiscard]] std::string_view glString(GLenum name)
{
    const auto* bytes = glGetString(name);
    if (bytes == nullptr)
    {
        return "<unknown>";
    }

    // OpenGL returns a byte pointer (GLubyte*); treat it as a C-string for logging only.
    const auto* chars = reinterpret_cast<const char*>(bytes); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
    return std::string_view(chars);
}
} // namespace

#ifdef _WIN32
namespace
{
// Set window icon from embedded resource on Windows
// This sets both the small icon (title bar, Alt+Tab) and large icon (taskbar)
[[nodiscard]] auto loadIconFromResource(HINSTANCE instance, int width, int height) -> HANDLE
{
    // LoadImage returns HANDLE; with IMAGE_ICON the returned handle is an icon handle.
    return LoadImage(instance, MAKEINTRESOURCE(1), IMAGE_ICON, width, height, LR_DEFAULTCOLOR);
}

void setWindowIcon(HWND hwnd, WPARAM iconType, HANDLE icon)
{
    // Win32 SendMessage takes LPARAM; HICON is pointer-sized and is passed opaquely.
    SendMessage(hwnd, WM_SETICON, iconType, std::bit_cast<LPARAM>(icon));
}

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
    HANDLE hIconSmall = loadIconFromResource(hInstance, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON));

    // Load large icon (32x32 or larger) for taskbar
    HANDLE hIconBig = loadIconFromResource(hInstance, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON));

    if (hIconSmall != nullptr)
    {
        setWindowIcon(hwnd, ICON_SMALL, hIconSmall);
        spdlog::debug("Set small window icon ({}x{})", GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON));
    }
    else
    {
        spdlog::warn("Failed to load small icon from resource");
    }

    if (hIconBig != nullptr)
    {
        setWindowIcon(hwnd, ICON_BIG, hIconBig);
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

    m_Spec.Width = clampWindowDimension(m_Spec.Width);
    m_Spec.Height = clampWindowDimension(m_Spec.Height);

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifndef NDEBUG
    glfwWindowHint(GLFW_OPENGL_DEBUG_CONTEXT, GLFW_TRUE);
#endif

    // NOLINTNEXTLINE(cppcoreguidelines-prefer-member-initializer) - glfwWindowHint must be called before glfwCreateWindow
    m_Handle = glfwCreateWindow(m_Spec.Width, m_Spec.Height, m_Spec.Title.c_str(), nullptr, nullptr);

    if (m_Handle == nullptr)
    {
        spdlog::critical("Failed to create GLFW window");
        assert(false);
        return;
    }

    glfwMakeContextCurrent(m_Handle);

    // Load OpenGL functions using GLAD
    const int version = gladLoadGL(glfwGetProcAddress);
    if (version == 0)
    {
        spdlog::critical("Failed to initialize GLAD");
        assert(false);
        return;
    }

    spdlog::info("OpenGL Info:");
    spdlog::info("  Vendor: {}", glString(GL_VENDOR));
    spdlog::info("  Renderer: {}", glString(GL_RENDERER));
    spdlog::info("  Version: {}", glString(GL_VERSION));

    glfwSwapInterval(m_Spec.VSync ? 1 : 0);

    glfwSetWindowUserPointer(m_Handle, this);

    // Framebuffer resize callback
    glfwSetFramebufferSizeCallback(m_Handle,
                                   [](GLFWwindow* window, int width, int height)
                                   {
                                       // GLFW stores the user pointer as void*; we set it to Window* in this constructor.
                                       auto* self = static_cast<Window*>(glfwGetWindowUserPointer(window));
                                       const int clampedWidth = clampWindowDimension(width);
                                       const int clampedHeight = clampWindowDimension(height);
                                       self->m_Spec.Width = clampedWidth;
                                       self->m_Spec.Height = clampedHeight;
                                       glViewport(0, 0, clampedWidth, clampedHeight);
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

void Window::setPosition(int x, int y) const
{
    if (m_Handle == nullptr)
    {
        return;
    }
    glfwSetWindowPos(m_Handle, x, y);
}

auto Window::getPosition() const -> std::pair<int, int>
{
    if (m_Handle == nullptr)
    {
        return {0, 0};
    }

    int x = 0;
    int y = 0;
    glfwGetWindowPos(m_Handle, &x, &y);
    return {x, y};
}

void Window::setSize(int width, int height)
{
    if (m_Handle == nullptr)
    {
        return;
    }

    const int clampedWidth = clampWindowDimension(width);
    const int clampedHeight = clampWindowDimension(height);
    glfwSetWindowSize(m_Handle, clampedWidth, clampedHeight);
    m_Spec.Width = clampedWidth;
    m_Spec.Height = clampedHeight;
}

auto Window::getSize() const -> std::pair<int, int>
{
    if (m_Handle == nullptr)
    {
        return {0, 0};
    }

    int width = 0;
    int height = 0;
    glfwGetWindowSize(m_Handle, &width, &height);
    return {width, height};
}

bool Window::isMaximized() const
{
    if (m_Handle == nullptr)
    {
        return false;
    }

    return glfwGetWindowAttrib(m_Handle, GLFW_MAXIMIZED) != 0;
}

void Window::maximize() const
{
    if (m_Handle == nullptr)
    {
        return;
    }

    glfwMaximizeWindow(m_Handle);
}

} // namespace Core
