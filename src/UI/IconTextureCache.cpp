#include "IconTextureCache.h"

#include "Domain/LruMap.h"

#include <glad/gl.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

namespace UI
{

IconTextureBackend IconTextureBackend::openGL()
{
    return IconTextureBackend{
        .create = [](int width, int height, std::span<const std::uint8_t> rgba) -> std::uint32_t
        {
            const auto bytes = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U;
            if (width <= 0 || height <= 0 || rgba.size() != bytes)
            {
                return 0U;
            }
            // No GL loaded -- a headless test of a panel, which still runs the real icon source: no
            // texture, so the generic icon is drawn.
            if (glGenTextures == nullptr || glTexImage2D == nullptr)
            {
                return 0U;
            }
            GLuint texture = 0U;
            glGenTextures(1, &texture);
            if (texture == 0U)
            {
                return 0U;
            }
            // glGetIntegerv takes a GLint*, texture names are GLuint: read it into a GLint.
            GLint previous = 0;
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous);
            glBindTexture(GL_TEXTURE_2D, texture);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
            glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previous));
            return texture;
        },
        .destroy =
            [](std::uint32_t texture)
        {
            if (glDeleteTextures == nullptr)
            {
                return;
            }
            const GLuint name = texture;
            glDeleteTextures(1, &name);
        },
    };
}

IconTextureCache::IconTextureCache(std::size_t capacity, IconTextureBackend backend) : m_Backend(std::move(backend)), m_Textures(capacity)
{}

IconTextureCache::~IconTextureCache()
{
    try
    {
        clear();
    }
    catch (...) // NOLINT(bugprone-empty-catch) - a destructor must not throw; the textures die with the context
    {}
}

std::uint32_t IconTextureCache::texture(std::uint64_t iconId, int width, int height, std::span<const std::uint8_t> rgba)
{
    if (const std::uint32_t* existing = m_Textures.find(iconId); existing != nullptr)
    {
        return *existing;
    }
    const std::uint32_t made = m_Backend.create ? m_Backend.create(width, height, rgba) : 0U;
    // A failure is remembered as 0, so it is not retried every frame.
    if (auto dropped = m_Textures.put(iconId, made); dropped.has_value() && dropped->second != 0U && m_Backend.destroy)
    {
        m_Backend.destroy(dropped->second);
    }
    return made;
}

void IconTextureCache::clear()
{
    if (m_Backend.destroy)
    {
        m_Textures.forEach(
            [this](std::uint64_t /*iconId*/, std::uint32_t texture)
            {
                if (texture != 0U)
                {
                    m_Backend.destroy(texture);
                }
            });
    }
    m_Textures.clear();
}

} // namespace UI
