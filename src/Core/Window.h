#pragma once

#include <string>
#include <utility>

struct GLFWwindow;

namespace Core
{

struct WindowSpecification
{
    std::string Title = "Window";
    int Width = 1280;
    int Height = 720;
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

    [[nodiscard]] int getWidth() const
    {
        return m_Spec.Width;
    }
    [[nodiscard]] int getHeight() const
    {
        return m_Spec.Height;
    }

    [[nodiscard]] GLFWwindow* getHandle() const
    {
        return m_Handle;
    }

    void setPosition(int x, int y) const;
    [[nodiscard]] auto getPosition() const -> std::pair<int, int>;

    void setSize(int width, int height);
    [[nodiscard]] auto getSize() const -> std::pair<int, int>;

    [[nodiscard]] bool isMaximized() const;
    void maximize() const;

  private:
    WindowSpecification m_Spec;
    GLFWwindow* m_Handle = nullptr;
};

} // namespace Core
