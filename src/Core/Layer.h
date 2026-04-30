#pragma once

#include "Event.h"

#include <string>

union SDL_Event;

namespace Core
{

class Layer
{
  public:
    explicit Layer(std::string name = "Layer") : m_Name(std::move(name))
    {}

    virtual ~Layer() = default;

    Layer(const Layer&) = default;
    Layer& operator=(const Layer&) = default;
    Layer(Layer&&) = default;
    Layer& operator=(Layer&&) = default;

    virtual void onAttach()
    {}
    virtual void onDetach()
    {}
    virtual void onUpdate([[maybe_unused]] float deltaTime)
    {}
    virtual void onRender()
    {}
    virtual void onPostRender()
    {}

    /// Handle application events (mouse, keyboard, window)
    /// Return true from your handler to mark the event as handled and stop propagation
    virtual void onEvent([[maybe_unused]] Event& event)
    {}

    /// Handle raw SDL events (for ImGui passthrough)
    virtual void onSDLEvent([[maybe_unused]] SDL_Event* event)
    {}

    [[nodiscard]] const std::string& getName() const
    {
        return m_Name;
    }

  protected:
    std::string m_Name;
};

} // namespace Core
