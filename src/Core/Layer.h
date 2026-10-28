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

    // Deleted rather than left default: Layer is a polymorphic base (used only through
    // pointers/references, e.g. Application's std::vector<std::unique_ptr<Layer>>), and a
    // permissive copy/move here would allow object slicing through a Layer&. Every current
    // subclass already deletes these itself; this makes that safety structural instead of a
    // convention each new subclass has to remember.
    Layer(const Layer&) = delete;
    Layer& operator=(const Layer&) = delete;
    Layer(Layer&&) = delete;
    Layer& operator=(Layer&&) = delete;

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

    /// Called instead of a frame while the window is hidden and the frame loop draws nothing
    /// (#3156): once per wake of its event wait, at least every WINDOW_HIDDEN_WAIT_TIMEOUT. For
    /// the little that can't wait for the next frame, such as taking in published samples that
    /// would otherwise be dropped. No ImGui frame is open: nothing may draw, and onUpdate()'s
    /// per-frame work waits for the next frame.
    virtual void onHiddenUpdate()
    {}

    /// Handle Core::Event broadcasts (window and application events; raw input arrives through
    /// onSDLEvent). Application::raiseEvent() calls layers top of the stack first. A dispatch
    /// handler that returns true marks the event handled and stops it reaching the layers below,
    /// so a handler that only reacts to a broadcast must return false. For WindowCloseEvent,
    /// handled means "veto the close" (see WindowEvents.h).
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
