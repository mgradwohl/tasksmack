#pragma once

#include <string>
#include <utility>

namespace App
{

/// Base class for all UI panels in the application.
/// Panels are ImGui-based windows that can be shown/hidden and managed by ShellLayer.
/// New panels should inherit from this class and implement at minimum render().
class Panel
{
  public:
    explicit Panel(std::string name) : m_Name(std::move(name))
    {
    }

    virtual ~Panel() = default;

    Panel(const Panel&) = delete;
    Panel& operator=(const Panel&) = delete;
    Panel(Panel&&) = default;
    Panel& operator=(Panel&&) = default;

    /// Called when the panel is first added to the application.
    /// Use for initialization, resource allocation, starting background tasks.
    virtual void onAttach()
    {
    }

    /// Called when the panel is removed from the application.
    /// Use for cleanup, stopping background tasks, releasing resources.
    virtual void onDetach()
    {
    }

    /// Called every frame before render.
    /// @param deltaTime Time since last frame in seconds.
    virtual void onUpdate([[maybe_unused]] float deltaTime)
    {
    }

    /// Render the panel. Must be implemented by derived classes.
    /// Should call ImGui::Begin/End with the panel name.
    /// @param open Pointer to visibility flag. Set to false to hide panel.
    ///             If nullptr, the close button is not shown.
    virtual void render(bool* open) = 0;

    /// Get the panel's display name.
    [[nodiscard]] const std::string& name() const noexcept
    {
        return m_Name;
    }

    /// Check if the panel is currently visible.
    [[nodiscard]] bool isVisible() const noexcept
    {
        return m_Visible;
    }

    /// Set panel visibility.
    void setVisible(bool visible) noexcept
    {
        m_Visible = visible;
    }

    /// Toggle panel visibility.
    void toggleVisible() noexcept
    {
        m_Visible = !m_Visible;
    }

  protected:
    std::string m_Name;
    bool m_Visible = true;
};

} // namespace App
