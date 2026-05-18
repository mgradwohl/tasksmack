#pragma once

#include <SDL3/SDL_video.h>

#include <string>
#include <utility>

namespace Core
{

/// Minimum window dimension (width or height) in pixels.
/// Must match the clamp bounds in main.cpp and UserConfig.cpp.
constexpr int WINDOW_MIN_DIMENSION = 200;

struct WindowSpecification
{
    std::string Title = "Window";
    int Width = 1280;
    int Height = 720;
    bool VSync = true;
    bool Borderless = true; // Enable custom title bar by default
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

    [[nodiscard]] bool shouldClose() const noexcept;
    void requestClose() noexcept;
    void clearCloseRequest() noexcept; // Reset close flag after handling WindowCloseEvent

    /// Return the current logical width in screen coordinates.
    /// Queries SDL directly so the value stays accurate after user-initiated drag-resizes.
    [[nodiscard]] int getWidth() const noexcept;

    /// Return the current logical height in screen coordinates.
    /// Queries SDL directly so the value stays accurate after user-initiated drag-resizes.
    [[nodiscard]] int getHeight() const noexcept;

    /// Return the current framebuffer size in physical pixels.
    /// Use this for OpenGL calls (e.g. glViewport) on HiDPI displays.
    [[nodiscard]] auto getSizeInPixels() const noexcept -> std::pair<int, int>;

    [[nodiscard]] SDL_Window* getHandle() const noexcept
    {
        return m_Handle;
    }

    [[nodiscard]] SDL_GLContext getGLContext() const noexcept
    {
        return m_GLContext;
    }

    void setPosition(int x, int y) const;
    [[nodiscard]] auto getPosition() const -> std::pair<int, int>;

    void setSize(int width, int height);
    /// Return the current logical size as {width, height} in screen coordinates.
    /// Returns {m_Spec.Width, m_Spec.Height} when the window handle has not yet
    /// been created. Callers that need both dimensions should prefer this over
    /// separate getWidth()/getHeight() calls to avoid issuing two SDL queries.
    [[nodiscard]] auto getSize() const noexcept -> std::pair<int, int>;

    [[nodiscard]] bool isMaximized() const;
    [[nodiscard]] bool isMinimized() const noexcept;
    void maximize();
    void restore();
    void minimize() const;

    // Custom title bar support
    [[nodiscard]] bool isBorderless() const noexcept
    {
        return m_Spec.Borderless;
    }
    // Set a hit test callback for custom window dragging/resizing
    void setHitTestCallback(SDL_HitTest callback, void* callbackData) const;

  private:
    WindowSpecification m_Spec;
    SDL_Window* m_Handle = nullptr;
    SDL_GLContext m_GLContext = nullptr;
    bool m_ShouldClose = false;

    // For borderless window maximize/restore tracking
    bool m_IsMaximizedBorderless = false;
    int m_RestoreX = 0;
    int m_RestoreY = 0;
    int m_RestoreWidth = 0;
    int m_RestoreHeight = 0;
};

} // namespace Core
