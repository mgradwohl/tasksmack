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
    /// User's UserConfig::forceNativeWindowDecorationsOnWayland preference, threaded through
    /// here (rather than Application reading UserConfig directly) to respect the
    /// Platform/Domain/Core/UI/App layering: Core must not depend on App. Only has an effect
    /// combined with VideoBackend::isWayland() at window-creation time (see #745).
    bool ForceNativeDecorationsOnWayland = false;
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
        try
        {
            layer->onAttach();
        }
        catch (...)
        {
            // onAttach() can throw after partially initializing the layer (e.g. UILayer creates
            // ImGui/ImPlot contexts before font/theme loading that can throw), so it must not be
            // treated as attached. Pop it back off without calling onDetach() -- onDetach()
            // assumes full initialization and could touch backends/resources onAttach() never
            // reached. The exception is rethrown so callers still see the failure (#779).
            m_LayerStack.pop_back();
            throw;
        }
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

    /// Called by layers (e.g. TitleBarLayer) whenever they actually move or resize the
    /// window. Gating grace-period renders on geometry changes avoids rendering identical
    /// frames during the post-interaction grace window (see m_WindowGeometryChangedThisFrame).
    void signalWindowGeometryChanged() noexcept
    {
        m_WindowGeometryChangedThisFrame = true;
    }

    [[nodiscard]] static Application& get();
    [[nodiscard]] static float getTime();
    static void setInstance(std::unique_ptr<Application> app);

  private:
    // Test-only accessor: lets unit tests observe m_WindowGeometryChangedThisFrame after
    // calling signalWindowGeometryChanged(), which has no other observable effect (it's
    // noexcept and only consumed internally by run()'s idle-sleep gate). Same pattern as
    // NVMLGPUProbeTestAccessor in Platform/Windows/NVMLGPUProbe.h. Production code never
    // touches this -- only test_Application.cpp uses it.
    friend struct ApplicationTestAccessor;

    ApplicationSpecification m_Spec;
    PathService m_Paths;
    std::unique_ptr<Window> m_Window;
    std::vector<std::unique_ptr<Layer>> m_LayerStack;
    bool m_Running = false;
    float m_InteractionRedrawUntil = 0.0F;
    bool m_ResizePerfTraceEnabled = false;
    /// True while vsync has been temporarily disabled for an active resize/move interaction.
    /// Restored to the original setting (adaptive vsync) when the interaction ends.
    bool m_VsyncDisabledForInteraction = false;
    /// Set to true by signalWindowGeometryChanged() (called from TitleBarLayer) whenever
    /// the window position or size actually changes during a frame's onUpdate pass.
    /// Cleared at the top of each main-loop iteration so it reflects only the PREVIOUS
    /// frame's geometry activity. Used to gate grace-period sleep: we skip the idle sleep
    /// only when geometry was in flux last frame, avoiding ~20 wasted renders after
    /// the user releases the mouse while the window is stationary.
    bool m_WindowGeometryChangedThisFrame = false;

    /// Run one update+render+swapBuffers cycle. Extracted so the main loop and
    /// the immediate-repaint-on-resize path share identical rendering logic.
    void renderFrame(float deltaTime,
                     bool tracingInteractionFrame,
                     bool resizeTriggeredFrame,
                     double* updateMs = nullptr,
                     double* renderMs = nullptr,
                     double* postRenderMs = nullptr,
                     double* swapMs = nullptr);

    // Global application instance (managed via setInstance)
    // Note: Using std::unique_ptr allows main() to control initialization order
    // and ensure proper cleanup with SDL_Quit
    static std::unique_ptr<Application> s_Instance;
};

} // namespace Core
