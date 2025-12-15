#pragma once

#include <cstdint>
#include <string>

struct GLFWwindow;

namespace Core
{

struct WindowSpecification
{
    std::string Title = "Window";
    uint32_t Width = 1280;
    uint32_t Height = 720;
    bool VSync = true;
};

class Window
{
  public:
    explicit Window(WindowSpecification spec = WindowSpecification());
    ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;
    Window(Window&&) = delete;
    Window& operator=(Window&&) = delete;

    void swapBuffers() const;

    [[nodiscard]] bool shouldClose() const;

    [[nodiscard]] uint32_t getWidth() const
    {
        return m_Spec.Width;
    }
    [[nodiscard]] uint32_t getHeight() const
    {
        return m_Spec.Height;
    }

    [[nodiscard]] GLFWwindow* getHandle() const
    {
        return m_Handle;
    }

  private:
    WindowSpecification m_Spec;
    GLFWwindow* m_Handle = nullptr;
};

} // namespace Core
