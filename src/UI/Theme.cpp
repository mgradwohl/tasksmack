#include "Theme.h"

#include "ColorContrast.h"
#include "Core/Utf8Path.h"
#include "DpiScale.h"
#include "FallbackTheme.h"
#include "StyleScale.h"
#include "ThemeCatalog.h"
#include "ThemeLoader.h"

#include <imgui.h>
#include <implot.h>
#include <spdlog/spdlog.h>

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace UI
{

namespace
{

/// Id of the built-in theme the constructor installs before any theme file is read.
constexpr std::string_view FALLBACK_THEME_ID = "fallback";

[[nodiscard]] constexpr auto fontSizeIndex(FontSize size) noexcept -> std::size_t
{
    return static_cast<std::size_t>(std::to_underlying(size));
}

} // namespace

auto Theme::get() -> Theme&
{
    static Theme instance;
    return instance;
}

Theme::Theme()
{
    initializeFontSizes();
    loadDefaultFallbackTheme();
}

void Theme::loadDefaultFallbackTheme()
{
    // Arctic Fire's own file, embedded at build time (FallbackTheme.h): the fallback had 8 identical
    // accents and drew network in the CPU and memory colours (#1196). It parses at every start-up and
    // a unit test loads it, so it cannot fail here; should it ever, a default scheme still installs.
    ColorScheme fallback = ThemeLoader::loadThemeFromString(FALLBACK_THEME_TOML, "built-in fallback theme").value_or(ColorScheme{});
    fallback.name = "Fallback";

    // Add as the initial theme
    DiscoveredTheme fallbackInfo;
    fallbackInfo.id = FALLBACK_THEME_ID;
    fallbackInfo.name = "Fallback";
    fallbackInfo.description = "Built-in fallback theme";

    m_DiscoveredThemes.push_back(std::move(fallbackInfo));
    m_LoadedSchemes.push_back(std::move(fallback));
}

void Theme::loadThemes(const std::filesystem::path& themesDir)
{
    spdlog::info("Loading themes from: {}", Core::pathToUtf8(themesDir));

    auto discovered = ThemeLoader::discoverThemes(themesDir);

    if (discovered.empty())
    {
        spdlog::warn("No themes found in {}, using fallback", Core::pathToUtf8(themesDir));
        return; // Keep the fallback theme
    }

    // Load into temporary buffers so the existing (fallback) scheme remains available
    // while parsing themes (useful for defaults/error colors).
    std::vector<DiscoveredTheme> discoveredThemes;
    std::vector<ColorScheme> loadedSchemes;
    discoveredThemes.reserve(discovered.size());
    loadedSchemes.reserve(discovered.size());

    for (auto& info : discovered)
    {
        if (auto scheme = ThemeLoader::loadTheme(info.path))
        {
            discoveredThemes.push_back(DiscoveredTheme{
                .id = info.id,
                .name = info.name,
                .description = info.description,
                .path = info.path,
            });
            loadedSchemes.push_back(std::move(*scheme));
        }
    }

    if (loadedSchemes.empty())
    {
        // Keep whatever is already loaded: the built-in fallback from the constructor, or the
        // built-in themes when this was the user directory. Appending another fallback here
        // produced a duplicate "Fallback" entry (#1127).
        spdlog::error("Failed to load any themes from {}", Core::pathToUtf8(themesDir));
        return;
    }

    const std::string currentId = m_DiscoveredThemes.empty() ? std::string{} : m_DiscoveredThemes[m_CurrentThemeIndex].id;
    const bool onlyFallbackLoaded = (m_DiscoveredThemes.size() == 1) && (m_DiscoveredThemes.front().id == FALLBACK_THEME_ID);
    if (onlyFallbackLoaded)
    {
        m_DiscoveredThemes = std::move(discoveredThemes);
        m_LoadedSchemes = std::move(loadedSchemes);
    }
    else
    {
        // A later directory (the user's) is layered over what is loaded, overriding by id (#1127).
        ThemeCatalog::mergeById(m_DiscoveredThemes, m_LoadedSchemes, std::move(discoveredThemes), std::move(loadedSchemes));
    }

    // Keep the current theme if it is still loaded; otherwise prefer arctic-fire, then the first.
    m_CurrentThemeIndex = ThemeCatalog::indexOfId(m_DiscoveredThemes, currentId)
                              .or_else([this] { return ThemeCatalog::indexOfId(m_DiscoveredThemes, "arctic-fire"); })
                              .value_or(0);

    spdlog::info("Loaded {} themes, current: {}", m_LoadedSchemes.size(), m_DiscoveredThemes[m_CurrentThemeIndex].name);
}

void Theme::initializeFontSizes()
{
    m_FontSizes = FONT_SIZE_PRESETS;
}

auto Theme::currentThemeId() const -> const std::string&
{
    return m_DiscoveredThemes[m_CurrentThemeIndex].id;
}

void Theme::setTheme(std::size_t index)
{
    if (index >= m_LoadedSchemes.size())
    {
        spdlog::warn("Invalid theme index: {}", index);
        return;
    }
    // Defer the theme change to next frame to avoid mid-frame style changes
    // that would leave already-rendered widgets with stale colors
    m_PendingThemeIndex = index;
    spdlog::debug("Theme change queued: index={}", index);
}

auto Theme::applyPendingStyleChanges() -> bool
{
    if (!m_PendingThemeIndex.has_value())
    {
        // A font-size or display-scale change still needs the style rebuilt, just without the
        // theme-changed signal.
        if (m_StyleDirty)
        {
            m_StyleDirty = false;
            applyImGuiStyle();
        }
        return false;
    }
    m_StyleDirty = false;

    const std::size_t index = m_PendingThemeIndex.value();
    m_PendingThemeIndex.reset();

    if (index >= m_LoadedSchemes.size())
    {
        spdlog::warn("Pending theme index out of range: {}", index);
        return false;
    }

    m_CurrentThemeIndex = index;
    applyImGuiStyle();
    spdlog::info("Applied pending theme: index={}", index);
    return true;
}

void Theme::setThemeById(std::string_view id)
{
    for (std::size_t i = 0; i < m_DiscoveredThemes.size(); ++i)
    {
        if (m_DiscoveredThemes[i].id == id)
        {
            setTheme(i);
            return;
        }
    }
    spdlog::warn("Theme not found: {}", id);
}

void Theme::applyImGuiStyle() const
{
    const auto& s = scheme();

    // Determine if this is a light or dark theme based on window background luminance
    // Y = 0.299*R + 0.587*G + 0.114*B (standard luminance formula)
    const float luminance = (0.299F * s.windowBg.x) + (0.587F * s.windowBg.y) + (0.114F * s.windowBg.z);
    constexpr float LIGHT_THRESHOLD = 0.5F;
    const bool isLightTheme = luminance > LIGHT_THRESHOLD;

    spdlog::info(
        "Applying theme '{}' (luminance={:.2f}, isLight={})", m_DiscoveredThemes[m_CurrentThemeIndex].name, luminance, isLightTheme);

    // Reset to appropriate base style first to ensure ALL color indices are initialized
    // This is critical because ImGui has more color indices than we explicitly set
    if (isLightTheme)
    {
        ImGui::StyleColorsLight();
    }
    else
    {
        ImGui::StyleColorsDark();
    }

    ImGuiStyle& style = ImGui::GetStyle();

    // Now override with our theme colors
    style.Colors[ImGuiCol_Text] = s.textPrimary;
    style.Colors[ImGuiCol_TextDisabled] = s.textDisabled;
    style.Colors[ImGuiCol_WindowBg] = s.windowBg;
    style.Colors[ImGuiCol_ChildBg] = s.childBg;
    // Popups are drawn opaque whatever alpha the theme gives popup_background. Every bundled theme
    // sets it to 94%, and the 6% that showed through a modal or a combo's drop-down was whatever lay
    // underneath -- usually dense table text, legible right through the dialog's own labels (#969).
    //
    // The colour is flattened over the backdrop a modal is normally seen against (the window
    // background under the modal dim layer) rather than just given alpha 1: see flattenOver() for
    // why that distinction keeps buttons and combos visible in the light themes. Done here rather
    // than in the theme files so user themes are covered too.
    style.Colors[ImGuiCol_PopupBg] = ColorContrast::flattenOver(s.popupBg, ColorContrast::flattenOver(s.modalWindowDimBg, s.windowBg));
    style.Colors[ImGuiCol_Border] = s.border;
    style.Colors[ImGuiCol_BorderShadow] = s.borderShadow;
    style.Colors[ImGuiCol_FrameBg] = s.frameBg;
    style.Colors[ImGuiCol_FrameBgHovered] = s.frameBgHovered;
    style.Colors[ImGuiCol_FrameBgActive] = s.frameBgActive;
    style.Colors[ImGuiCol_TitleBg] = s.titleBg;
    style.Colors[ImGuiCol_TitleBgActive] = s.titleBgActive;
    style.Colors[ImGuiCol_TitleBgCollapsed] = s.titleBgCollapsed;
    style.Colors[ImGuiCol_MenuBarBg] = s.menuBarBg;
    style.Colors[ImGuiCol_ScrollbarBg] = s.scrollbarBg;
    style.Colors[ImGuiCol_ScrollbarGrab] = s.scrollbarGrab;
    style.Colors[ImGuiCol_ScrollbarGrabHovered] = s.scrollbarGrabHovered;
    style.Colors[ImGuiCol_ScrollbarGrabActive] = s.scrollbarGrabActive;
    style.Colors[ImGuiCol_CheckMark] = s.checkMark;
    style.Colors[ImGuiCol_SliderGrab] = s.sliderGrab;
    style.Colors[ImGuiCol_SliderGrabActive] = s.sliderGrabActive;
    style.Colors[ImGuiCol_Button] = s.button;
    style.Colors[ImGuiCol_ButtonHovered] = s.buttonHovered;
    style.Colors[ImGuiCol_ButtonActive] = s.buttonActive;
    style.Colors[ImGuiCol_Header] = s.header;
    style.Colors[ImGuiCol_HeaderHovered] = s.headerHovered;
    style.Colors[ImGuiCol_HeaderActive] = s.headerActive;
    style.Colors[ImGuiCol_Separator] = s.separator;
    style.Colors[ImGuiCol_SeparatorHovered] = s.separatorHovered;
    style.Colors[ImGuiCol_SeparatorActive] = s.separatorActive;
    style.Colors[ImGuiCol_ResizeGrip] = s.resizeGrip;
    style.Colors[ImGuiCol_ResizeGripHovered] = s.resizeGripHovered;
    style.Colors[ImGuiCol_ResizeGripActive] = s.resizeGripActive;
    style.Colors[ImGuiCol_Tab] = s.tab;
    style.Colors[ImGuiCol_TabHovered] = s.tabHovered;
    style.Colors[ImGuiCol_TabSelected] = s.tabSelected;
    style.Colors[ImGuiCol_TabSelectedOverline] = s.tabSelectedOverline;
    style.Colors[ImGuiCol_TabDimmed] = s.tabDimmed;
    style.Colors[ImGuiCol_TabDimmedSelected] = s.tabDimmedSelected;
    style.Colors[ImGuiCol_TabDimmedSelectedOverline] = s.tabDimmedSelectedOverline;
    style.Colors[ImGuiCol_DockingPreview] = s.dockingPreview;
    style.Colors[ImGuiCol_DockingEmptyBg] = s.dockingEmptyBg;
    style.Colors[ImGuiCol_PlotLines] = s.plotLines;
    style.Colors[ImGuiCol_PlotLinesHovered] = s.plotLinesHovered;
    style.Colors[ImGuiCol_PlotHistogram] = s.plotHistogram;
    style.Colors[ImGuiCol_PlotHistogramHovered] = s.plotHistogramHovered;
    style.Colors[ImGuiCol_TableHeaderBg] = s.tableHeaderBg;
    style.Colors[ImGuiCol_TableBorderStrong] = s.tableBorderStrong;
    style.Colors[ImGuiCol_TableBorderLight] = s.tableBorderLight;
    style.Colors[ImGuiCol_TableRowBg] = s.tableRowBg;
    style.Colors[ImGuiCol_TableRowBgAlt] = s.tableRowBgAlt;
    style.Colors[ImGuiCol_TextSelectedBg] = s.textSelectedBg;
    style.Colors[ImGuiCol_DragDropTarget] = s.dragDropTarget;
    style.Colors[ImGuiCol_NavHighlight] = s.navHighlight;
    style.Colors[ImGuiCol_NavWindowingHighlight] = s.navWindowingHighlight;
    style.Colors[ImGuiCol_NavWindowingDimBg] = s.navWindowingDimBg;
    style.Colors[ImGuiCol_ModalWindowDimBg] = s.modalWindowDimBg;

    // Style settings (consistent across themes).
    //
    // The literals below are authored for the Medium preset on a 1.0 display scale and multiplied
    // by `scale`, so padding, spacing, indents, scrollbars, grab sizes and corner radii track both
    // the Font Size setting and the display's density (#936). Without that, text grew with the font
    // setting while the chrome around it stayed frozen at these pixels, and a scaled display -- the
    // common case on Windows -- got proportionally undersized chrome.
    //
    // Each field is re-assigned from its literal on every call, so the scale cannot compound. See
    // computeStyleScale() for why this is done here rather than with ImGuiStyle::ScaleAllSizes().
    const float scale = styleScale();

    style.WindowRounding = 4.0F * scale;
    style.ChildRounding = 4.0F * scale;
    style.FrameRounding = 2.0F * scale;
    style.PopupRounding = 4.0F * scale;
    style.ScrollbarRounding = 4.0F * scale;
    style.GrabRounding = 2.0F * scale;
    style.TabRounding = 4.0F * scale;

    // Borders are hairlines and are deliberately left unscaled: a window border reads as an edge,
    // not as a proportion of the content, and on both platforms the system chrome keeps it at one
    // pixel regardless of density.
    style.WindowBorderSize = 1.0F;
    style.ChildBorderSize = 1.0F;
    style.PopupBorderSize = 1.0F;
    style.FrameBorderSize = 0.0F;
    style.TabBorderSize = 0.0F;

    style.WindowPadding = ImVec2(8.0F * scale, 8.0F * scale);
    style.FramePadding = ImVec2(FRAME_PADDING_X * scale, FRAME_PADDING_Y * scale);
    style.ItemSpacing = ImVec2(8.0F * scale, 4.0F * scale);
    style.ItemInnerSpacing = ImVec2(4.0F * scale, 4.0F * scale);
    // Authored here rather than left at ImGui's default, which is this same ImVec2(4, 2) -- so the
    // look is unchanged at the reference configuration, but the value now scales. It is the one
    // style field this application reads without authoring, and it is the second most read of them
    // all: thirteen sites depend on it, and not for cosmetics but for layout arithmetic. ChartGrid
    // subtracts rows*CellPadding.y*2 from the available height to decide whether a scrollbar is
    // needed (#823), CpuCoresSection and StorageSection fold it into their cell-height floors, and
    // ProcessDetailsPanel sizes columns by it. Several of those cache their result keyed on
    // CellPadding.y changing, so they already assume it tracks the style -- leaving it unscaled
    // while ItemSpacing beside it scaled would have made that assumption quietly wrong.
    style.CellPadding = ImVec2(4.0F * scale, 2.0F * scale);
    style.IndentSpacing = 20.0F * scale;
    style.ScrollbarSize = 14.0F * scale;
    style.GrabMinSize = 10.0F * scale;

    // ImGui's own defaults for the remaining sizes the app visibly uses, authored so they scale
    // with the rest instead of staying at 1x beside it (#1169). The scrollbar grab's inset grows
    // with the scrollbar. The selected-tab overline (every tab bar draws it) and the tab bar's
    // underline are accent strokes, not edges, so they thicken with the tabs, in whole pixels; so
    // does the text caret. The docking splitter and SeparatorText() rule are authored for the same
    // reason, though nothing draws them today. WindowBorderHoverPadding stays at ImGui's default: it
    // also widens which window counts as hovered, and no ImGui window here is resizable.
    style.ScrollbarPadding = 2.0F * scale;
    style.TabBarBorderSize = scaledStrokePx(1.0F, scale);
    style.TabBarOverlineSize = scaledStrokePx(1.0F, scale);
    style.InputTextCursorSize = scaledStrokePx(1.0F, scale);
    style.SeparatorTextBorderSize = scaledStrokePx(3.0F, scale);
    style.DockingSeparatorSize = scaledStrokePx(2.0F, scale);

    spdlog::info("ImGui style scaled by {:.2f} ({} preset at {:.2f} display scale)", scale, fontConfig().name, m_DisplayScale);

    // Apply ImPlot style colors from theme
    // StyleColorsAuto() derives colors from current ImGui style
    ImPlot::StyleColorsAuto();

    // Override specific ImPlot colors to match our theme exactly
    ImPlotStyle& plotStyle = ImPlot::GetStyle();
    plotStyle.Colors[ImPlotCol_LegendText] = s.textPrimary;
    plotStyle.Colors[ImPlotCol_InlayText] = s.textPrimary;
    plotStyle.Colors[ImPlotCol_AxisText] = s.textMuted;
    plotStyle.Colors[ImPlotCol_AxisTick] = s.textMuted;
    plotStyle.Colors[ImPlotCol_AxisGrid] = s.plotGrid;
    plotStyle.Colors[ImPlotCol_TitleText] = s.textPrimary;
    plotStyle.Colors[ImPlotCol_PlotBg] = s.childBg;
    plotStyle.Colors[ImPlotCol_FrameBg] = s.frameBg;
    plotStyle.Colors[ImPlotCol_LegendBg] = s.popupBg;
    plotStyle.Colors[ImPlotCol_LegendBorder] = s.border;

    // ImPlot's sizes, scaled like the ImGui style above (#971): these are ImPlot's own defaults,
    // authored here so they track the font and display instead of staying 5-10px beside text up to
    // 2.8 times the reference size. Re-assigned from the literals on every call, so they cannot
    // compound. Left at ImPlot's pixel defaults on purpose:
    // - PlotPadding and LabelPadding come straight out of the plotting area. Scaled, they took about
    //   60px from a chart's data area at Extra Large on 175% (170px -> 110px), squeezing the axis
    //   labels together, and the fill layout's chart heights have no room to give.
    // - MajorTickLen and MinorTickLen: ticks are drawn inside the plotting area, over the data.
    //   Scaled, a major tick was 28px tall at Extra Large on 175% and cut through the band where
    //   low values are drawn.
    // - Line thicknesses (border, ticks, grid) are hairlines, as the ImGui borders are.
    // - PlotMinSize and PlotDefaultSize: every chart passes its own height, and a scaled 150px
    //   minimum would override the fill layout's heights at large scales.
    plotStyle.PlotPadding = ImVec2(10.0F, 10.0F);
    plotStyle.LabelPadding = ImVec2(5.0F, 5.0F);
    plotStyle.MajorTickLen = ImVec2(10.0F, 10.0F);
    plotStyle.MinorTickLen = ImVec2(5.0F, 5.0F);
    plotStyle.LegendPadding = ImVec2(10.0F * scale, 10.0F * scale);
    plotStyle.LegendInnerPadding = ImVec2(5.0F * scale, 5.0F * scale);
    plotStyle.LegendSpacing = ImVec2(5.0F * scale, 0.0F);
    plotStyle.MousePosPadding = ImVec2(10.0F * scale, 10.0F * scale);
    plotStyle.AnnotationPadding = ImVec2(2.0F * scale, 2.0F * scale);

    // CRITICAL: Bust ImPlot's color cache to force re-read of style colors
    // ImPlot caches colors when SetupAxis() is called - without this,
    // runtime theme changes won't update existing plots' axis labels/ticks
    ImPlot::BustColorCache();
}

auto Theme::styleScale() const -> float
{
    return computeStyleScale(fontConfig().regularPt, m_DisplayScale);
}

auto Theme::scheme() const -> const ColorScheme&
{
    return m_LoadedSchemes[m_CurrentThemeIndex];
}

auto Theme::themeName(std::size_t index) const -> std::string_view
{
    if (index >= m_DiscoveredThemes.size())
    {
        return "Unknown";
    }
    return m_DiscoveredThemes[index].name;
}

auto Theme::accentColor(std::size_t index) const -> ImVec4
{
    return scheme().accents[index % accentCount()];
}

// ============ Font Management ============

void Theme::setFontSize(FontSize size)
{
    if (size == m_CurrentFontSize)
    {
        return;
    }
    m_CurrentFontSize = size;
    spdlog::info("Font size changed to: {}", fontConfig().name);

    // Queue a style rebuild so padding, spacing and scrollbars follow the new font size; without it
    // the chrome keeps the previous preset's proportions until the next theme change (#936). Queued
    // rather than applied because the settings dialog calls this from inside a live frame --
    // see applyPendingStyleChanges().
    m_StyleDirty = true;
}

void Theme::setDisplayScale(float scale)
{
    // Compared with a tolerance rather than ==, and ignoring an unusable scale (SDL reports 0.0 on
    // failure) -- see displayScaleChanged().
    if (!displayScaleChanged(m_DisplayScale, scale))
    {
        return;
    }
    m_DisplayScale = scale;
    spdlog::info("Display scale set to: {:.2f}", scale);
    m_StyleDirty = true;
}

auto Theme::fontConfig() const -> const FontSizeConfig&
{
    return m_FontSizes[fontSizeIndex(m_CurrentFontSize)];
}

auto Theme::fontConfig(FontSize size) const -> const FontSizeConfig&
{
    return m_FontSizes[fontSizeIndex(size)];
}

auto Theme::increaseFontSize() -> bool
{
    switch (m_CurrentFontSize)
    {
    case FontSize::Small:
        setFontSize(FontSize::Medium);
        return true;
    case FontSize::Medium:
        setFontSize(FontSize::Large);
        return true;
    case FontSize::Large:
        setFontSize(FontSize::ExtraLarge);
        return true;
    case FontSize::ExtraLarge:
        setFontSize(FontSize::Huge);
        return true;
    case FontSize::Huge:
        setFontSize(FontSize::EvenHuger);
        return true;
    case FontSize::EvenHuger:
    case FontSize::Count:
        return false;
    }

    return false;
}

auto Theme::decreaseFontSize() -> bool
{
    switch (m_CurrentFontSize)
    {
    case FontSize::Small:
    case FontSize::Count:
        return false;
    case FontSize::Medium:
        setFontSize(FontSize::Small);
        return true;
    case FontSize::Large:
        setFontSize(FontSize::Medium);
        return true;
    case FontSize::ExtraLarge:
        setFontSize(FontSize::Large);
        return true;
    case FontSize::Huge:
        setFontSize(FontSize::ExtraLarge);
        return true;
    case FontSize::EvenHuger:
        setFontSize(FontSize::Huge);
        return true;
    }

    return false;
}

auto Theme::regularFont() const -> ImFont*
{
    return m_Fonts[fontSizeIndex(m_CurrentFontSize)].regular;
}

auto Theme::largeFont() const -> ImFont*
{
    return m_Fonts[fontSizeIndex(m_CurrentFontSize)].large;
}

auto Theme::boldFont() const -> ImFont*
{
    const auto& fonts = m_Fonts[fontSizeIndex(m_CurrentFontSize)];
    return fonts.bold != nullptr ? fonts.bold : fonts.regular;
}

auto Theme::monospaceFont() const -> ImFont*
{
    const auto& fonts = m_Fonts[fontSizeIndex(m_CurrentFontSize)];
    return fonts.monospace != nullptr ? fonts.monospace : fonts.regular;
}

auto Theme::chartFont() const -> ImFont*
{
    return m_Fonts[fontSizeIndex(chartFontSize(m_CurrentFontSize))].regular;
}

void Theme::registerFonts(FontSize size, ImFont* regular, ImFont* large, ImFont* monospace)
{
    m_Fonts[fontSizeIndex(size)] = {.regular = regular, .large = large, .monospace = monospace, .bold = nullptr};
}

void Theme::registerBoldFont(FontSize size, ImFont* bold)
{
    m_Fonts[fontSizeIndex(size)].bold = bold;
}

void Theme::registerTitleFont(ImFont* font)
{
    m_TitleFont = font;
}

void Theme::clearFontRegistrations()
{
    m_Fonts.fill(FontPair{});
    m_TitleFont = nullptr;
    m_ChromeIconFont = nullptr;
    ++m_FontGeneration;
}

void Theme::setTitleBarHeightPx(float heightPx)
{
    if (heightPx > 0.0F)
    {
        m_TitleBarHeightPx = heightPx;
    }
}

void Theme::registerChromeIconFont(ImFont* font, float sizePx)
{
    m_ChromeIconFont = font;
    if (sizePx > 0.0F)
    {
        m_ChromeIconFontSizePx = sizePx;
    }
}

auto Theme::titleFont() const -> ImFont*
{
    return m_TitleFont;
}

} // namespace UI
