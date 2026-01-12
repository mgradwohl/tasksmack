#pragma once

#include "Event.h"
#include "Layer.h"
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

    /// Dispatch an event to all layers (in reverse order)
    /// Stops when a layer marks the event as handled
    void raiseEvent(Event& event);

    template<typename T, typename... Args>
        requires std::is_base_of_v<Layer, T>
    T& pushLayer(Args&&... args)
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

    [[nodiscard]] static Application& get();
    [[nodiscard]] static float getTime();

  private:
    ApplicationSpecification m_Spec;
    std::unique_ptr<Window> m_Window;
    std::vector<std::unique_ptr<Layer>> m_LayerStack;
    bool m_Running = false;

    static Application* s_Instance;
};

} // namespace Core
