#pragma once

#include <imgui.h>

#include <filesystem>
#include <utility>

// Forward-declare GLuint without dragging in GLFW headers here.
using GLuint = unsigned int;

namespace UI
{

class Texture
{
  public:
    Texture() = default;
    ~Texture();

    Texture(const Texture&) = delete;
    Texture& operator=(const Texture&) = delete;

    Texture(Texture&& other) noexcept;
    Texture& operator=(Texture&& other) noexcept;

    [[nodiscard]] bool valid() const noexcept
    {
        return m_Id != 0U;
    }

    [[nodiscard]] GLuint id() const noexcept
    {
        return m_Id;
    }

    [[nodiscard]] ImVec2 size() const noexcept
    {
        return ImVec2(static_cast<float>(m_Width), static_cast<float>(m_Height));
    }

  private:
    void destroy() noexcept;

    GLuint m_Id = 0U;
    int m_Width = 0;
    int m_Height = 0;

    friend class TextureLoader;
    friend auto loadTexture(const std::filesystem::path& path) -> Texture;
};

[[nodiscard]] auto loadTexture(const std::filesystem::path& path) -> Texture;

} // namespace UI
