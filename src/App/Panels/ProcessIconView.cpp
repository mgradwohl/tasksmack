#include "ProcessIconView.h"

#include "Domain/ProcessIconCache.h"
#include "Domain/ProcessSnapshot.h"
#include "Platform/IProcessIconSource.h"
#include "UI/IconTextureCache.h"
#include "UI/IconsFontAwesome6.h"
#include "UI/Theme.h"

#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>

namespace App
{

const char* const ProcessIconView::GENERIC_GLYPH = ICON_FA_WINDOW_MAXIMIZE;

namespace
{

/// The generic glyph's size as a share of the icon's: a little smaller, as the glyph fills its em
/// square where most program icons leave a margin.
constexpr float GENERIC_GLYPH_SCALE = 0.8F;

/// Icon sides, in pixels, the cache is asked for: at least this, so a tiny font still gets a
/// legible icon, and at most this.
constexpr int MIN_ICON_PX = 8;
constexpr int MAX_ICON_PX = 128;

} // namespace

ProcessIconView::ProcessIconView(UI::IconTextureBackend backend, Domain::ProcessIconCacheConfig cacheConfig)
    : m_Backend(std::move(backend)), m_CacheConfig(cacheConfig)
{}

ProcessIconView::~ProcessIconView() = default;

void ProcessIconView::attach(std::unique_ptr<Platform::IProcessIconSource> source)
{
    detach();
    if (source == nullptr)
    {
        return;
    }
    m_Cache = std::make_unique<Domain::ProcessIconCache>(std::move(source), m_CacheConfig);
    m_Textures = std::make_unique<UI::IconTextureCache>(UI::IconTextureCache::DEFAULT_CAPACITY, m_Backend);
}

void ProcessIconView::detach()
{
    m_Cache.reset(); // Joins the loader
    if (m_Textures != nullptr)
    {
        m_Textures->clear();
        m_Textures.reset();
    }
}

void ProcessIconView::beginFrame()
{
    m_IconSizePx = std::clamp(static_cast<int>(std::lround(ImGui::GetTextLineHeight())), MIN_ICON_PX, MAX_ICON_PX);
    m_IconSize = static_cast<float>(m_IconSizePx);
}

ImTextureID ProcessIconView::textureFor(const Domain::ProcessSnapshot& proc)
{
    if (!m_Enabled || m_Cache == nullptr || m_Textures == nullptr)
    {
        return ImTextureID_Invalid;
    }
    const std::shared_ptr<const Domain::ProcessIconImage> icon = m_Cache->lookup(proc.uniqueKey, proc.pid, proc.name, m_IconSizePx);
    if (icon == nullptr)
    {
        return ImTextureID_Invalid;
    }
    const std::uint32_t texture = m_Textures->texture(icon->id, icon->sizePx, icon->sizePx, std::span<const std::uint8_t>(icon->rgba));
    // A GL texture name is an integer; ImTextureID is ImU64 in this build (imconfig default).
    return static_cast<ImTextureID>(texture);
}

void ProcessIconView::drawGeneric(ImDrawList& drawList, ImVec2 min, float size)
{
    ImFont* font = ImGui::GetFont();
    const float glyphSize = size * GENERIC_GLYPH_SCALE;
    const ImVec2 glyph = font->CalcTextSizeA(glyphSize, FLT_MAX, 0.0F, GENERIC_GLYPH);
    const ImVec2 pos(std::floor(min.x + ((size - glyph.x) * 0.5F)), std::floor(min.y + ((size - glyph.y) * 0.5F)));
    drawList.AddText(font, glyphSize, pos, ImGui::ColorConvertFloat4ToU32(UI::Theme::get().scheme().textMuted), GENERIC_GLYPH);
}

void ProcessIconView::drawAt(ImDrawList& drawList, ImVec2 min, float size, const Domain::ProcessSnapshot& proc)
{
    if (const ImTextureID texture = textureFor(proc); texture != ImTextureID_Invalid)
    {
        drawList.AddImage(ImTextureRef(texture), min, ImVec2(min.x + size, min.y + size));
        return;
    }
    drawGeneric(drawList, min, size);
}

void ProcessIconView::drawInline(const Domain::ProcessSnapshot& proc)
{
    if (!m_Enabled)
    {
        return;
    }
    if (const ImTextureID texture = textureFor(proc); texture != ImTextureID_Invalid)
    {
        ImGui::Image(ImTextureRef(texture), ImVec2(m_IconSize, m_IconSize));
    }
    else
    {
        const ImVec2 min = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(m_IconSize, m_IconSize));
        if (ImGui::IsItemVisible())
        {
            drawGeneric(*ImGui::GetWindowDrawList(), min, m_IconSize);
        }
    }
    ImGui::SameLine();
}

} // namespace App
