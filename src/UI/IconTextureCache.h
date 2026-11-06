#pragma once

// GPU textures for decoded icons (#1800): Domain::ProcessIconCache decodes on its worker, and this
// makes each icon's texture once, on the UI thread that owns the GL context, keyed by the icon's id.
// At most `capacity` textures are kept; the least recently drawn is deleted to make room, and every
// one is deleted by clear() and the destructor. The texture calls go through a backend (OpenGL's by
// default) so the tests count creations and deletions without a GL context
// (tests/UI/test_IconTextureCache.cpp). After a GPU reset replaces the OpenGL context
// (Core::graphicsContextGeneration() moves on, #3484) the textures it holds went with the old one:
// the next texture() or clear() forgets them, without deleting names the new context may already
// use, and texture() makes each icon's texture again as it is drawn.

#include "Domain/LruMap.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>

namespace UI
{

/// Makes and deletes RGBA textures.
struct IconTextureBackend
{
    /// A texture of @p width x @p height straight-alpha RGBA pixels; 0 when it can't be made.
    std::function<std::uint32_t(int width, int height, std::span<const std::uint8_t> rgba)> create;
    /// Deletes a texture create() made.
    std::function<void(std::uint32_t texture)> destroy;

    /// OpenGL: GL_RGBA8, linear filtering, no mipmaps (icons are drawn at the size they are made at).
    [[nodiscard]] static IconTextureBackend openGL();
};

class IconTextureCache
{
  public:
    static constexpr std::size_t DEFAULT_CAPACITY = 512;

    explicit IconTextureCache(std::size_t capacity = DEFAULT_CAPACITY, IconTextureBackend backend = IconTextureBackend::openGL());
    ~IconTextureCache();

    IconTextureCache(const IconTextureCache&) = delete;
    IconTextureCache& operator=(const IconTextureCache&) = delete;
    IconTextureCache(IconTextureCache&&) = delete;
    IconTextureCache& operator=(IconTextureCache&&) = delete;

    /// The texture for icon @p iconId, made from @p rgba (@p width x @p height) the first time it is
    /// asked for; 0 when it could not be made (and is not tried again until it is evicted). UI
    /// thread; no allocation once the texture exists.
    [[nodiscard]] std::uint32_t texture(std::uint64_t iconId, int width, int height, std::span<const std::uint8_t> rgba);

    /// Deletes every texture. Call while the GL context is current (the owner's onDetach()).
    void clear();

    [[nodiscard]] std::size_t size() const noexcept
    {
        return m_Textures.size();
    }

  private:
    /// Forgets every texture, deleting none, when the context they were made in has been replaced.
    void forgetIfContextReplaced();

    IconTextureBackend m_Backend;
    Domain::LruMap<std::uint64_t, std::uint32_t> m_Textures;
    std::uint64_t m_ContextGeneration; // Core::graphicsContextGeneration() the textures were made in
};

} // namespace UI
