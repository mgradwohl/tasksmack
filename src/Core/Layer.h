#pragma once

#include <string>

namespace Core
{

class Layer
{
  public:
    explicit Layer(std::string name = "Layer") : m_Name(std::move(name))
    {
    }

    virtual ~Layer() = default;

    Layer(const Layer&) = default;
    Layer& operator=(const Layer&) = default;
    Layer(Layer&&) = default;
    Layer& operator=(Layer&&) = default;

    virtual void onAttach()
    {
    }
    virtual void onDetach()
    {
    }
    virtual void onUpdate([[maybe_unused]] float deltaTime)
    {
    }
    virtual void onRender()
    {
    }
    virtual void onPostRender()
    {
    }

    [[nodiscard]] const std::string& getName() const
    {
        return m_Name;
    }

  protected:
    std::string m_Name;
};

} // namespace Core
