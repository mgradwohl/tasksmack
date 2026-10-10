#include "UI/IconLoader.h"

#include "Core/Utf8Path.h"

#include <glad/gl.h>
#include <spdlog/spdlog.h>

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wsign-conversion"
#pragma clang diagnostic ignored "-Wcast-align"
#pragma clang diagnostic ignored "-Wdouble-promotion"
#pragma clang diagnostic ignored "-Wimplicit-int-conversion"
#pragma clang diagnostic ignored "-Wimplicit-fallthrough"
#endif

#define STB_IMAGE_IMPLEMENTATION
#ifdef _WIN32
// stbi_load() takes a UTF-8 file name and opens it with _wfopen, never through the code page (#1648).
#define STBI_WINDOWS_UTF8
#endif
// NOLINTNEXTLINE(misc-include-cleaner) - stb_image.h macros generate scanner errors
#include <stb_image.h>

#ifdef __clang__
#pragma clang diagnostic pop
#endif

#include <filesystem>
#include <utility>

namespace UI
{
namespace
{
[[nodiscard]] auto bindTexture(GLuint textureId) -> GLuint
{
    // glGetIntegerv expects GLint*, but texture IDs are GLuint.
    // Use intermediate variable to avoid reinterpret_cast.
    GLint temp = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &temp);
    const auto previous = static_cast<GLuint>(temp);
    glBindTexture(GL_TEXTURE_2D, textureId);
    return previous;
}
} // namespace

Texture::~Texture()
{
    destroy();
}

Texture::Texture(Texture&& other) noexcept
    : m_Id(std::exchange(other.m_Id, 0U)), m_Width(std::exchange(other.m_Width, 0)), m_Height(std::exchange(other.m_Height, 0))
{}

Texture& Texture::operator=(Texture&& other) noexcept
{
    if (this != &other)
    {
        destroy();
        m_Id = std::exchange(other.m_Id, 0U);
        m_Width = std::exchange(other.m_Width, 0);
        m_Height = std::exchange(other.m_Height, 0);
    }
    return *this;
}

void Texture::destroy() noexcept
{
    if (m_Id != 0U)
    {
        glDeleteTextures(1, &m_Id);
        m_Id = 0U;
    }
    m_Width = 0;
    m_Height = 0;
}

auto loadTexture(const std::filesystem::path& path) -> Texture
{
    if (path.empty())
    {
        return {};
    }

    int width = 0;
    int height = 0;
    int channels = 0;
    // UTF-8, not path.string(): with STBI_WINDOWS_UTF8 (above) stb_image converts the name from
    // UTF-8 and opens it with _wfopen. A narrowed name went through the active code page, so an
    // install under a non-ASCII directory failed in every binary without the UTF-8 manifest (#1648).
    stbi_uc* pixels = stbi_load(Core::pathToUtf8(path).c_str(), &width, &height, &channels, STBI_rgb_alpha);
    if (pixels == nullptr)
    {
        spdlog::warn("Failed to load icon {}: {}", Core::pathToUtf8(path), stbi_failure_reason());
        return {};
    }

    GLuint textureId = 0U;
    glGenTextures(1, &textureId);
    if (textureId == 0U)
    {
        stbi_image_free(pixels);
        spdlog::warn("Failed to allocate OpenGL texture for icon");
        return {};
    }

    const GLuint previousBinding = bindTexture(textureId);

    // Mipmapped, so an icon drawn well below its size -- the About dialog's 256 px icon at 72-96 px --
    // is filtered from a level near the drawn size instead of aliasing from single samples of the
    // full image (#1169).
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glGenerateMipmap(GL_TEXTURE_2D);

    glBindTexture(GL_TEXTURE_2D, previousBinding);
    stbi_image_free(pixels);

    Texture texture;
    texture.m_Id = textureId;
    texture.m_Width = width;
    texture.m_Height = height;
    return texture;
}

} // namespace UI
