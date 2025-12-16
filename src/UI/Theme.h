#pragma once

#include <imgui.h>

#include <array>
#include <cstddef>
#include <string_view>

namespace UI
{

/// Named color schemes for the application
enum class ThemeId
{
    ArcticFire = 0, // Blue → Emerald → Amber → Red
    Cyberpunk,      // Purple → Magenta → Orange → Yellow (neon)
    Monochrome,     // Terminal green aesthetic
    Count
};

/// Font size presets
enum class FontSize
{
    Small = 0,  // 6pt / 8pt
    Medium,     // 8pt / 10pt (default)
    Large,      // 10pt / 12pt
    ExtraLarge, // 12pt / 14pt
    Count
};

/// Color scheme definition with heatmap gradient and accent colors
struct ColorScheme
{
    std::string_view name;

    // Heatmap gradient (5 stops: 0%, 25%, 50%, 75%, 100%)
    std::array<ImVec4, 5> heatmap;

    // Accent colors for line charts, legends, etc. (8 colors)
    std::array<ImVec4, 8> accents;

    // Progress bar colors (low, medium, high)
    ImVec4 progressLow;    // 0-50%
    ImVec4 progressMedium; // 50-80%
    ImVec4 progressHigh;   // 80-100%

    // Semantic UI colors
    ImVec4 textMuted;   // Dimmed/secondary text (labels, hints)
    ImVec4 textError;   // Error messages
    ImVec4 textWarning; // Warning messages
    ImVec4 textSuccess; // Success messages
    ImVec4 textInfo;    // Informational text

    // Status colors for process states
    ImVec4 statusRunning;  // Running/Active
    ImVec4 statusStopped;  // Stopped/Terminated
    ImVec4 statusSleeping; // Sleeping/Waiting

    // Chart line colors (for specific metrics)
    ImVec4 chartCpu;    // CPU usage line
    ImVec4 chartMemory; // Memory usage line
    ImVec4 chartIo;     // I/O usage line

    // CPU breakdown colors
    ImVec4 cpuUser;   // User CPU time
    ImVec4 cpuSystem; // System/kernel CPU time
    ImVec4 cpuIowait; // I/O wait time
    ImVec4 cpuIdle;   // Idle time
    ImVec4 cpuSteal;  // VM steal time

    // Danger button colors
    ImVec4 buttonDanger;
    ImVec4 buttonDangerHovered;
    ImVec4 buttonDangerActive;

    // ImGui style colors (base colors for UI chrome)
    ImVec4 windowBg;
    ImVec4 childBg;
    ImVec4 popupBg;
    ImVec4 border;
    ImVec4 frameBg;
    ImVec4 frameBgHovered;
    ImVec4 frameBgActive;
    ImVec4 titleBg;
    ImVec4 titleBgActive;
    ImVec4 titleBgCollapsed;
    ImVec4 menuBarBg;
    ImVec4 scrollbarBg;
    ImVec4 scrollbarGrab;
    ImVec4 scrollbarGrabHovered;
    ImVec4 scrollbarGrabActive;
    ImVec4 checkMark;
    ImVec4 sliderGrab;
    ImVec4 sliderGrabActive;
    ImVec4 button;
    ImVec4 buttonHovered;
    ImVec4 buttonActive;
    ImVec4 header;
    ImVec4 headerHovered;
    ImVec4 headerActive;
    ImVec4 separator;
    ImVec4 separatorHovered;
    ImVec4 separatorActive;
    ImVec4 resizeGrip;
    ImVec4 resizeGripHovered;
    ImVec4 resizeGripActive;
    ImVec4 tab;
    ImVec4 tabHovered;
    ImVec4 tabActive;
    ImVec4 tabUnfocused;
    ImVec4 tabUnfocusedActive;
    ImVec4 dockingPreview;
    ImVec4 dockingEmptyBg;
    ImVec4 plotLines;
    ImVec4 plotLinesHovered;
    ImVec4 plotHistogram;
    ImVec4 plotHistogramHovered;
    ImVec4 tableHeaderBg;
    ImVec4 tableBorderStrong;
    ImVec4 tableBorderLight;
    ImVec4 tableRowBg;
    ImVec4 tableRowBgAlt;
    ImVec4 textSelectedBg;
    ImVec4 dragDropTarget;
    ImVec4 navHighlight;
    ImVec4 navWindowingHighlight;
    ImVec4 navWindowingDimBg;
    ImVec4 modalWindowDimBg;
};

/// Font size configuration (in points)
struct FontSizeConfig
{
    std::string_view name;
    float regularPt; // Body text
    float largePt;   // Headings
};

/// Global theme manager - provides access to color schemes and font settings
class Theme
{
  public:
    /// Get the singleton instance
    static Theme& get();

    /// Get current theme ID
    [[nodiscard]] ThemeId currentTheme() const
    {
        return m_CurrentTheme;
    }

    /// Set current theme
    void setTheme(ThemeId theme);

    /// Apply current theme colors to ImGui style
    void applyImGuiStyle() const;

    /// Get current color scheme
    [[nodiscard]] const ColorScheme& scheme() const;

    /// Get color scheme by ID
    [[nodiscard]] const ColorScheme& scheme(ThemeId id) const;

    /// Get theme name
    [[nodiscard]] std::string_view themeName(ThemeId id) const;

    /// Interpolate heatmap color for a value 0-100
    [[nodiscard]] ImVec4 heatmapColor(double percent) const;

    /// Get progress bar color based on percent
    [[nodiscard]] ImVec4 progressColor(double percent) const;

    /// Get accent color by index (wraps around)
    [[nodiscard]] ImVec4 accentColor(size_t index) const;

    /// Number of accent colors
    [[nodiscard]] static constexpr size_t accentCount()
    {
        return 8;
    }

    // ============ Font Size Management ============

    /// Get current font size preset
    [[nodiscard]] FontSize currentFontSize() const
    {
        return m_CurrentFontSize;
    }

    /// Set font size preset (triggers font rebuild on next frame)
    void setFontSize(FontSize size);

    /// Get font size config
    [[nodiscard]] const FontSizeConfig& fontConfig() const;
    [[nodiscard]] const FontSizeConfig& fontConfig(FontSize size) const;

    /// Increase font size (returns true if changed)
    bool increaseFontSize();

    /// Decrease font size (returns true if changed)
    bool decreaseFontSize();

    // ============ Pre-baked Font Access ============

    /// Get the current regular font (based on font size setting)
    [[nodiscard]] ImFont* regularFont() const;

    /// Get the current large/heading font (based on font size setting)
    [[nodiscard]] ImFont* largeFont() const;

    /// Register pre-baked fonts (called by UILayer during initialization)
    void registerFonts(FontSize size, ImFont* regular, ImFont* large);

  private:
    Theme();
    ~Theme() = default;

    Theme(const Theme&) = delete;
    Theme& operator=(const Theme&) = delete;

    ThemeId m_CurrentTheme = ThemeId::ArcticFire;
    std::array<ColorScheme, static_cast<size_t>(ThemeId::Count)> m_Schemes;

    FontSize m_CurrentFontSize = FontSize::Medium;
    std::array<FontSizeConfig, static_cast<size_t>(FontSize::Count)> m_FontSizes;

    // Pre-baked fonts for each size preset (regular and large variants)
    struct FontPair
    {
        ImFont* regular = nullptr;
        ImFont* large = nullptr;
    };
    std::array<FontPair, static_cast<size_t>(FontSize::Count)> m_Fonts{};

    void initializeSchemes();
    void initializeFontSizes();
};

// Helper to convert hex color to ImVec4 (compile-time friendly)
constexpr ImVec4 hexToImVec4(uint32_t hex)
{
    return ImVec4(static_cast<float>((hex >> 16) & 0xFF) / 255.0F,
                  static_cast<float>((hex >> 8) & 0xFF) / 255.0F,
                  static_cast<float>(hex & 0xFF) / 255.0F,
                  1.0F);
}

} // namespace UI
