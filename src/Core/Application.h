#pragma once

#include "Event.h"
#include "Layer.h"
#include "PathService.h"
#include "Window.h"

#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace Core
{

struct ApplicationSpecification
{
    std::string Name = "Application";
    int Width = 1280;
    int Height = 720;
    bool VSync = true;
};

class Application
{
  public:
    explicit Application(ApplicationSpecification spec = ApplicationSpecification());
    ~Application();

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;
    Application(Application&&) = delete;
    Application& operator=(Application&&) = delete;

    void run();
    void stop();

    /// Detach all layers in reverse order (topmost first).
    /// This should be called before destroying the Application to allow layers
    /// to access Application::get() during cleanup.
    void detachAllLayers();

    /// Dispatch an event to all layers (in reverse order)
    /// Stops when a layer marks the event as handled
    void raiseEvent(Event& event);

    /// Push a new layer onto the layer stack and return a reference to it.
    /// The returned reference is valid for the lifetime of the Application.
    /// Use the returned reference for immediate configuration (e.g., calling setters after construction).
    /// Example: auto& shell = app.pushLayer<ShellLayer>(); shell.setContentYOffset(height);
    template<typename T, typename... Args>
        requires std::is_base_of_v<Layer, T>
    T& pushLayer(Args&&... args) // NOLINT(cpp/unused-local-variable,cpp/unused-static-variable) - args used in std::forward pack expansion
    {
        auto& layer = m_LayerStack.emplace_back(std::make_unique<T>(std::forward<Args>(args)...));
        layer->onAttach();
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast) - Type is guaranteed by template
        return static_cast<T&>(*layer);
    }

    [[nodiscard]] Window& getWindow() const
    {
        return *m_Window;
    }

    [[nodiscard]] const PathService& paths() const noexcept
    {
        return m_Paths;
    }

    [[nodiscard]] bool isInteractionRedrawActive() const noexcept
    {
        return getTime() < m_InteractionRedrawUntil;
    }

    [[nodiscard]] static Application& get();
    [[nodiscard]] static float getTime();
    static void setInstance(std::unique_ptr<Application> app);

  private:
    ApplicationSpecification m_Spec;
    PathService m_Paths;
    std::unique_ptr<Window> m_Window;
    std::vector<std::unique_ptr<Layer>> m_LayerStack;
    bool m_Running = false;
    float m_InteractionRedrawUntil = 0.0F;
    bool m_ResizePerfTraceEnabled = false;

    /// Run one update+render+swapBuffers cycle. Extracted so the main loop and
    /// the immediate-repaint-on-resize path share identical rendering logic.
    void renderFrame(
        float deltaTime, double* updateMs = nullptr, double* renderMs = nullptr, double* postRenderMs = nullptr, double* swapMs = nullptr);

    // Global application instance (managed via setInstance)
    // Note: Using std::unique_ptr allows main() to control initialization order
    // and ensure proper cleanup with SDL_Quit
    static std::unique_ptr<Application> s_Instance;
};

} // namespace Core
