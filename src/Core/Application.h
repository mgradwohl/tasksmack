#pragma once

#include "Layer.h"
#include "Window.h"

#include <memory>
#include <string>
#include <vector>

namespace Core
{

struct ApplicationSpecification
{
    std::string Name = "Application";
    uint32_t Width = 1280;
    uint32_t Height = 720;
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

    template<typename T, typename... Args>
        requires std::is_base_of_v<Layer, T>
    void pushLayer(Args&&... args)
    {
        auto& layer = m_LayerStack.emplace_back(std::make_unique<T>(std::forward<Args>(args)...));
        layer->onAttach();
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
