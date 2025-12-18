#include "Theme.h"

#include "ThemeLoader.h"

#include <spdlog/spdlog.h>

#include <algorithm>

namespace UI
{

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
    // Create a minimal fallback theme in case TOML files aren't found
    ColorScheme fallback;
    fallback.name = "Fallback";

    // Basic gray/blue colors
    const auto gray = ImVec4(0.5F, 0.5F, 0.5F, 1.0F);
    const auto blue = ImVec4(0.26F, 0.59F, 0.98F, 1.0F);
    const auto darkBg = ImVec4(0.1F, 0.1F, 0.1F, 1.0F);

    fallback.heatmap = {blue, blue, gray, gray, gray};
    fallback.accents = {blue, blue, blue, blue, blue, blue, blue, blue};
    fallback.progressLow = blue;
    fallback.progressMedium = gray;
    fallback.progressHigh = ImVec4(1.0F, 0.0F, 0.0F, 1.0F);

    fallback.textMuted = gray;
    fallback.textError = ImVec4(1.0F, 0.0F, 0.0F, 1.0F);
    fallback.textWarning = ImVec4(1.0F, 1.0F, 0.0F, 1.0F);
    fallback.textSuccess = ImVec4(0.0F, 1.0F, 0.0F, 1.0F);
    fallback.textInfo = blue;

    fallback.statusRunning = ImVec4(0.0F, 1.0F, 0.0F, 1.0F);
    fallback.statusStopped = ImVec4(1.0F, 0.0F, 0.0F, 1.0F);
    fallback.statusSleeping = ImVec4(1.0F, 1.0F, 0.0F, 1.0F);

    fallback.chartCpu = blue;
    fallback.chartMemory = ImVec4(0.0F, 1.0F, 0.0F, 1.0F);
    fallback.chartIo = ImVec4(1.0F, 0.5F, 0.0F, 1.0F);

    fallback.cpuUser = blue;
    fallback.cpuSystem = ImVec4(1.0F, 0.5F, 0.0F, 1.0F);
    fallback.cpuIowait = ImVec4(1.0F, 1.0F, 0.0F, 1.0F);
    fallback.cpuIdle = gray;
    fallback.cpuSteal = ImVec4(1.0F, 0.0F, 0.0F, 1.0F);

    fallback.dangerButton = ImVec4(0.8F, 0.0F, 0.0F, 1.0F);
    fallback.dangerButtonHovered = ImVec4(1.0F, 0.0F, 0.0F, 1.0F);
    fallback.dangerButtonActive = ImVec4(0.5F, 0.0F, 0.0F, 1.0F);

    fallback.windowBg = darkBg;
    fallback.childBg = ImVec4(0.0F, 0.0F, 0.0F, 0.0F);
    fallback.popupBg = ImVec4(0.08F, 0.08F, 0.08F, 0.94F);
    fallback.border = ImVec4(0.43F, 0.43F, 0.50F, 0.50F);
    fallback.frameBg = ImVec4(0.16F, 0.29F, 0.48F, 0.54F);
    fallback.frameBgHovered = ImVec4(0.26F, 0.59F, 0.98F, 0.40F);
    fallback.frameBgActive = ImVec4(0.26F, 0.59F, 0.98F, 0.67F);
    fallback.titleBg = ImVec4(0.04F, 0.04F, 0.04F, 1.0F);
    fallback.titleBgActive = ImVec4(0.16F, 0.29F, 0.48F, 1.0F);
    fallback.titleBgCollapsed = ImVec4(0.0F, 0.0F, 0.0F, 0.51F);
    fallback.menuBarBg = ImVec4(0.14F, 0.14F, 0.14F, 1.0F);
    fallback.statusBarBg = ImVec4(0.14F, 0.14F, 0.14F, 1.0F);
    fallback.scrollbarBg = ImVec4(0.02F, 0.02F, 0.02F, 0.53F);
    fallback.scrollbarGrab = ImVec4(0.31F, 0.31F, 0.31F, 1.0F);
    fallback.scrollbarGrabHovered = ImVec4(0.41F, 0.41F, 0.41F, 1.0F);
    fallback.scrollbarGrabActive = ImVec4(0.51F, 0.51F, 0.51F, 1.0F);
    fallback.checkMark = blue;
    fallback.sliderGrab = blue;
    fallback.sliderGrabActive = ImVec4(0.26F, 0.59F, 0.98F, 1.0F);
    fallback.button = ImVec4(0.26F, 0.59F, 0.98F, 0.40F);
    fallback.buttonHovered = ImVec4(0.26F, 0.59F, 0.98F, 1.0F);
    fallback.buttonActive = ImVec4(0.06F, 0.53F, 0.98F, 1.0F);
    fallback.header = ImVec4(0.26F, 0.59F, 0.98F, 0.31F);
    fallback.headerHovered = ImVec4(0.26F, 0.59F, 0.98F, 0.80F);
    fallback.headerActive = ImVec4(0.26F, 0.59F, 0.98F, 1.0F);
    fallback.separator = ImVec4(0.43F, 0.43F, 0.50F, 0.50F);
    fallback.separatorHovered = ImVec4(0.10F, 0.40F, 0.75F, 0.78F);
    fallback.separatorActive = ImVec4(0.10F, 0.40F, 0.75F, 1.0F);
    fallback.resizeGrip = ImVec4(0.26F, 0.59F, 0.98F, 0.20F);
    fallback.resizeGripHovered = ImVec4(0.26F, 0.59F, 0.98F, 0.67F);
    fallback.resizeGripActive = ImVec4(0.26F, 0.59F, 0.98F, 0.95F);
    fallback.tab = ImVec4(0.18F, 0.35F, 0.58F, 0.86F);
    fallback.tabHovered = ImVec4(0.26F, 0.59F, 0.98F, 0.80F);
    fallback.tabSelected = ImVec4(0.20F, 0.41F, 0.68F, 1.0F);
    fallback.tabSelectedOverline = ImVec4(0.0F, 0.0F, 0.0F, 0.0F); // Transparent to disable
    fallback.tabDimmed = ImVec4(0.07F, 0.10F, 0.15F, 0.97F);
    fallback.tabDimmedSelected = ImVec4(0.14F, 0.26F, 0.42F, 1.0F);
    fallback.tabDimmedSelectedOverline = ImVec4(0.0F, 0.0F, 0.0F, 0.0F); // Transparent
    fallback.dockingPreview = ImVec4(0.26F, 0.59F, 0.98F, 0.70F);
    fallback.dockingEmptyBg = ImVec4(0.20F, 0.20F, 0.20F, 1.0F);
    fallback.plotLines = ImVec4(0.61F, 0.61F, 0.61F, 1.0F);
    fallback.plotLinesHovered = ImVec4(1.0F, 0.43F, 0.35F, 1.0F);
    fallback.plotHistogram = ImVec4(0.90F, 0.70F, 0.0F, 1.0F);
    fallback.plotHistogramHovered = ImVec4(1.0F, 0.60F, 0.0F, 1.0F);
    fallback.tableHeaderBg = ImVec4(0.19F, 0.19F, 0.20F, 1.0F);
    fallback.tableBorderStrong = ImVec4(0.31F, 0.31F, 0.35F, 1.0F);
    fallback.tableBorderLight = ImVec4(0.23F, 0.23F, 0.25F, 1.0F);
    fallback.tableRowBg = ImVec4(0.0F, 0.0F, 0.0F, 0.0F);
    fallback.tableRowBgAlt = ImVec4(1.0F, 1.0F, 1.0F, 0.06F);
    fallback.textSelectedBg = ImVec4(0.26F, 0.59F, 0.98F, 0.35F);
    fallback.dragDropTarget = ImVec4(1.0F, 1.0F, 0.0F, 0.90F);
    fallback.navHighlight = ImVec4(0.26F, 0.59F, 0.98F, 1.0F);
    fallback.navWindowingHighlight = ImVec4(1.0F, 1.0F, 1.0F, 0.70F);
    fallback.navWindowingDimBg = ImVec4(0.80F, 0.80F, 0.80F, 0.20F);
    fallback.modalWindowDimBg = ImVec4(0.80F, 0.80F, 0.80F, 0.35F);

    // Add as the initial theme
    DiscoveredTheme fallbackInfo;
    fallbackInfo.id = "fallback";
    fallbackInfo.name = "Fallback";
    fallbackInfo.description = "Built-in fallback theme";

    m_DiscoveredThemes.push_back(std::move(fallbackInfo));
    m_LoadedSchemes.push_back(std::move(fallback));
}

void Theme::loadThemes(const std::filesystem::path& themesDir)
{
    spdlog::info("Loading themes from: {}", themesDir.string());

    auto discovered = ThemeLoader::discoverThemes(themesDir);

    if (discovered.empty())
    {
        spdlog::warn("No themes found in {}, using fallback", themesDir.string());
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
        spdlog::error("Failed to load any themes, reverting to fallback");
        loadDefaultFallbackTheme();
        return;
    }

    m_DiscoveredThemes = std::move(discoveredThemes);
    m_LoadedSchemes = std::move(loadedSchemes);
    m_CurrentThemeIndex = 0;

    // Set default theme (prefer arctic-fire if available)
    for (std::size_t i = 0; i < m_DiscoveredThemes.size(); ++i)
    {
        if (m_DiscoveredThemes[i].id == "arctic-fire")
        {
            m_CurrentThemeIndex = i;
            break;
        }
    }

    spdlog::info("Loaded {} themes, current: {}", m_LoadedSchemes.size(), m_DiscoveredThemes[m_CurrentThemeIndex].name);
}

void Theme::initializeFontSizes()
{
    // Font size presets (regular/large point sizes)
    m_FontSizes[static_cast<std::size_t>(FontSize::Small)] = {.name = "Small", .regularPt = 6.0F, .largePt = 8.0F};
    m_FontSizes[static_cast<std::size_t>(FontSize::Medium)] = {.name = "Medium", .regularPt = 8.0F, .largePt = 10.0F};
    m_FontSizes[static_cast<std::size_t>(FontSize::Large)] = {.name = "Large", .regularPt = 10.0F, .largePt = 12.0F};
    m_FontSizes[static_cast<std::size_t>(FontSize::ExtraLarge)] = {.name = "Extra Large", .regularPt = 12.0F, .largePt = 14.0F};
    m_FontSizes[static_cast<std::size_t>(FontSize::Huge)] = {.name = "Huge", .regularPt = 14.0F, .largePt = 16.0F};
    m_FontSizes[static_cast<std::size_t>(FontSize::EvenHuger)] = {.name = "Even Huger", .regularPt = 16.0F, .largePt = 18.0F};
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
    m_CurrentThemeIndex = index;
    spdlog::info("Theme changed to: {}", m_DiscoveredThemes[m_CurrentThemeIndex].name);
    applyImGuiStyle();
}

void Theme::setThemeById(const std::string& id)
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
    ImGuiStyle& style = ImGui::GetStyle();
    const auto& s = scheme();

    // Apply colors
    style.Colors[ImGuiCol_WindowBg] = s.windowBg;
    style.Colors[ImGuiCol_ChildBg] = s.childBg;
    style.Colors[ImGuiCol_PopupBg] = s.popupBg;
    style.Colors[ImGuiCol_Border] = s.border;
    auto borderShadow = s.border;
    borderShadow.w = 0.0F;
    style.Colors[ImGuiCol_BorderShadow] = borderShadow;
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

    // Style settings (consistent across themes)
    style.WindowRounding = 4.0F;
    style.ChildRounding = 4.0F;
    style.FrameRounding = 2.0F;
    style.PopupRounding = 4.0F;
    style.ScrollbarRounding = 4.0F;
    style.GrabRounding = 2.0F;
    style.TabRounding = 4.0F;

    style.WindowBorderSize = 1.0F;
    style.ChildBorderSize = 1.0F;
    style.PopupBorderSize = 1.0F;
    style.FrameBorderSize = 0.0F;
    style.TabBorderSize = 0.0F;

    style.WindowPadding = ImVec2(8.0F, 8.0F);
    style.FramePadding = ImVec2(4.0F, 3.0F);
    style.ItemSpacing = ImVec2(8.0F, 4.0F);
    style.ItemInnerSpacing = ImVec2(4.0F, 4.0F);
    style.IndentSpacing = 20.0F;
    style.ScrollbarSize = 14.0F;
    style.GrabMinSize = 10.0F;
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

auto Theme::heatmapColor(double percent) const -> ImVec4
{
    const auto& colors = scheme().heatmap;
    const double t = std::clamp(percent, 0.0, 100.0) / 100.0;

    // 5 stops: 0, 0.25, 0.5, 0.75, 1.0
    constexpr double STEP = 0.25;
    auto idx = static_cast<std::size_t>(t / STEP);
    if (idx >= 4)
    {
        idx = 3;
    }

    const double localT = (t - (static_cast<double>(idx) * STEP)) / STEP;

    const ImVec4& c1 = colors[idx];
    const ImVec4& c2 = colors[idx + 1];

    const auto localTf = static_cast<float>(localT);
    return ImVec4(c1.x + ((c2.x - c1.x) * localTf),
                  c1.y + ((c2.y - c1.y) * localTf),
                  c1.z + ((c2.z - c1.z) * localTf),
                  c1.w + ((c2.w - c1.w) * localTf));
}

auto Theme::progressColor(double percent) const -> ImVec4
{
    constexpr double LOW_THRESHOLD = 50.0;
    constexpr double HIGH_THRESHOLD = 80.0;

    if (percent < LOW_THRESHOLD)
    {
        return scheme().progressLow;
    }
    if (percent < HIGH_THRESHOLD)
    {
        return scheme().progressMedium;
    }
    return scheme().progressHigh;
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
}

auto Theme::fontConfig() const -> const FontSizeConfig&
{
    return m_FontSizes[static_cast<std::size_t>(m_CurrentFontSize)];
}

auto Theme::fontConfig(FontSize size) const -> const FontSizeConfig&
{
    return m_FontSizes[static_cast<std::size_t>(size)];
}

auto Theme::increaseFontSize() -> bool
{
    auto current = static_cast<int>(m_CurrentFontSize);
    if (current < static_cast<int>(FontSize::Count) - 1)
    {
        setFontSize(static_cast<FontSize>(current + 1));
        return true;
    }
    return false;
}

auto Theme::decreaseFontSize() -> bool
{
    auto current = static_cast<int>(m_CurrentFontSize);
    if (current > 0)
    {
        setFontSize(static_cast<FontSize>(current - 1));
        return true;
    }
    return false;
}

auto Theme::regularFont() const -> ImFont*
{
    return m_Fonts[static_cast<std::size_t>(m_CurrentFontSize)].regular;
}

auto Theme::largeFont() const -> ImFont*
{
    return m_Fonts[static_cast<std::size_t>(m_CurrentFontSize)].large;
}

void Theme::registerFonts(FontSize size, ImFont* regular, ImFont* large)
{
    m_Fonts[static_cast<std::size_t>(size)] = {.regular = regular, .large = large};
}

} // namespace UI
