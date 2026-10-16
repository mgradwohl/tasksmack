#pragma once

// The icon beside each process's name (#1800): the Processes table's Name column and the Process
// Details tab, and beside each program the Startup and Services tabs list (drawFileInline). Composes the Platform icon source (handed in by
// the panel, the composition root), the Domain cache that loads icons off the UI thread, and the UI texture cache that turns them into
// textures on it. A process with no icon -- or whose icon is still loading -- shows a generic glyph
// in the same slot, so names stay aligned.
//
// Per row and frame: one cache lookup (a map find and a list splice under a short lock) and, for a
// loaded icon, one texture-map find; no allocation once the process has been seen.

#include "Domain/ProcessIconCache.h"
#include "Platform/IProcessIconSource.h"
#include "UI/IconTextureCache.h"

#include <imgui.h>

#include <cstdint>
#include <memory>
#include <string_view>

namespace Domain
{
struct ProcessSnapshot;
} // namespace Domain

namespace App
{

class ProcessIconView
{
  public:
    /// The generic icon: a blank window, like Task Manager's for a program without its own.
    static const char* const GENERIC_GLYPH;
    /// A service's generic icon: a gear, for a service without its own (svchost's, or unreadable).
    static const char* const SERVICE_GLYPH;

    /// @p backend makes the textures (OpenGL's by default; tests and benchmarks pass a fake), and
    /// @p cacheConfig bounds the Domain cache (tests and benchmarks run it without its worker).
    explicit ProcessIconView(UI::IconTextureBackend backend = UI::IconTextureBackend::openGL(),
                             Domain::ProcessIconCacheConfig cacheConfig = {});
    ~ProcessIconView();

    ProcessIconView(const ProcessIconView&) = delete;
    ProcessIconView& operator=(const ProcessIconView&) = delete;
    ProcessIconView(ProcessIconView&&) = delete;
    ProcessIconView& operator=(ProcessIconView&&) = delete;

    /// Starts loading icons through @p source. nullptr: every process shows the generic icon.
    void attach(std::unique_ptr<Platform::IProcessIconSource> source);

    /// Stops the loader and deletes every texture. Call while the GL context is current.
    void detach();

    /// The Settings toggle (Show process icons): off, nothing is drawn and nothing is loaded.
    void setEnabled(bool enabled) noexcept
    {
        m_Enabled = enabled;
    }

    [[nodiscard]] bool enabled() const noexcept
    {
        return m_Enabled;
    }

    /// Once a frame, before the rows: the icon's size is the current font's line height, so a row
    /// keeps its height (re-measured each frame; it follows font size and display scale changes).
    void beginFrame();

    /// The icon's side in pixels, as of the last beginFrame().
    [[nodiscard]] float iconSize() const noexcept
    {
        return m_IconSize;
    }

    /// Draws @p proc's icon (or the generic one) at the cursor as an item, then SameLine(), so the
    /// name follows it. Draws nothing while disabled.
    void drawInline(const Domain::ProcessSnapshot& proc);

    /// @p proc's icon texture at the current size; ImTextureID_Invalid when it has none, is still
    /// loading, or icons are off.
    [[nodiscard]] ImTextureID textureFor(const Domain::ProcessSnapshot& proc);

    /// Draws the icon of the file at @p path (a startup entry's or service's program, or a packaged
    /// app's logo), filed under @p fileKey (Domain::iconFileKey(path)), at the cursor as an item, then
    /// SameLine(); @p genericGlyph while it loads, when it has none and for an empty path. @p name is
    /// the row's name, for a platform that finds icons by name (Linux). Draws nothing while disabled.
    void drawFileInline(std::uint64_t fileKey, std::string_view path, std::string_view name, const char* genericGlyph = GENERIC_GLYPH);

    /// The file's icon texture at the current size; ImTextureID_Invalid when it has none, is still
    /// loading, @p path is empty, or icons are off.
    [[nodiscard]] ImTextureID textureForFile(std::uint64_t fileKey, std::string_view path, std::string_view name);

    /// The Domain cache, for its statistics (benchmarks); nullptr before attach() with a source.
    [[nodiscard]] Domain::ProcessIconCache* cache() noexcept
    {
        return m_Cache.get();
    }

  private:
    static void drawGeneric(ImDrawList& drawList, ImVec2 min, float size, const char* glyph = GENERIC_GLYPH);
    /// @p texture, or @p genericGlyph when it is ImTextureID_Invalid, at the cursor, then SameLine().
    void drawTextureInline(ImTextureID texture, const char* genericGlyph) const;
    /// The texture of a loaded icon (made the first time it is drawn); ImTextureID_Invalid for nullptr.
    [[nodiscard]] ImTextureID textureOf(const std::shared_ptr<const Domain::ProcessIconImage>& icon);

    UI::IconTextureBackend m_Backend;
    Domain::ProcessIconCacheConfig m_CacheConfig;
    std::unique_ptr<Domain::ProcessIconCache> m_Cache;
    std::unique_ptr<UI::IconTextureCache> m_Textures;
    bool m_Enabled = true;
    float m_IconSize = 16.0F;
    int m_IconSizePx = 16;
};

} // namespace App
