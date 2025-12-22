#pragma once

// clang-format off
#include <glad/gl.h>
// clang-format on
#include <imgui.h>

#include <filesystem>
#include <utility>

namespace UI
{

/// RAII wrapper for OpenGL texture resources.
/// Move-only type that automatically releases the texture on destruction.
/// Typical usage: auto tex = loadTexture("path/to/icon.png"); ImGui::Image(tex.id(), tex.size());
/// Default-constructed instances are invalid (valid() returns false).
class Texture
{
  public:
    Texture() = default;
    ~Texture();

    Texture(const Texture&) = delete;
    Texture& operator=(const Texture&) = delete;

    Texture(Texture&& other) noexcept;
    Texture& operator=(Texture&& other) noexcept;

    /// Returns true if this texture holds a valid OpenGL texture ID.
    /// Default-constructed or moved-from textures return false.
    [[nodiscard]] bool valid() const noexcept
    {
        return m_Id != 0U;
    }

    /// Returns the OpenGL texture ID for use with ImGui::Image() or raw OpenGL calls.
    /// Returns 0 if the texture is invalid.
    [[nodiscard]] GLuint id() const noexcept
    {
        return m_Id;
    }

    /// Returns the texture dimensions as an ImVec2 for convenience with ImGui.
    /// Returns (0, 0) if the texture is invalid.
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

/// Loads an image file into an OpenGL texture.
/// Supports common formats via stb_image (PNG, JPEG, BMP, TGA, etc.).
/// Returns an invalid Texture (valid() == false) if the file cannot be loaded.
/// The returned Texture owns the OpenGL resource and will release it on destruction.
[[nodiscard]] auto loadTexture(const std::filesystem::path& path) -> Texture;

} // namespace UI
