#pragma once

#include <imgui.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace UI
{

/// Font size presets
enum class FontSize : std::uint8_t
{
    Small = 0,  // 7pt / 9pt
    Medium,     // 8pt / 10pt (default)
    Large,      // 10pt / 12pt
    ExtraLarge, // 12pt / 14pt
    Huge,       // 14pt / 16pt
    EvenHuger,  // 16pt / 18pt
    Count
};

inline constexpr auto ALL_FONT_SIZES = std::to_array<FontSize>({
    FontSize::Small,
    FontSize::Medium,
    FontSize::Large,
    FontSize::ExtraLarge,
    FontSize::Huge,
    FontSize::EvenHuger,
});

inline constexpr std::size_t FONT_SIZE_COUNT = ALL_FONT_SIZES.size();

/// Color scheme definition with accent colors
struct ColorScheme
{
    std::string name;

    // Accent colors for line charts, legends, etc. (8 colors)
    std::array<ImVec4, 8> accents{};

    // Progress bar colors (low, medium, high)
    ImVec4 progressLow;    // 0-50%
    ImVec4 progressMedium; // 50-80%
    ImVec4 progressHigh;   // 80-100%

    // Semantic UI colors
    ImVec4 textPrimary;  // Primary text color
    ImVec4 textDisabled; // Disabled text color
    ImVec4 textMuted;    // Dimmed/secondary text (labels, hints)
    ImVec4 textError;    // Error messages
    ImVec4 textWarning;  // Warning messages
    ImVec4 textSuccess;  // Success messages
    ImVec4 textInfo;     // Informational text

    // Status colors for process states
    ImVec4 statusRunning;   // R - Running/Active (green)
    ImVec4 statusSleeping;  // S - Sleeping/Interruptible (gray/muted)
    ImVec4 statusDiskSleep; // D - Disk sleep/Uninterruptible (yellow/orange)
    ImVec4 statusZombie;    // Z - Zombie/Defunct (red)
    ImVec4 statusStopped;   // T - Stopped/Traced (purple/magenta)
    ImVec4 statusIdle;      // I - Idle kernel thread (gray)

    // Chart line colors (for specific metrics)
    ImVec4 chartCpu;     // CPU usage line
    ImVec4 chartMemory;  // Memory usage line
    ImVec4 chartIo;      // I/O read usage line
    ImVec4 chartIoWrite; // I/O write usage line (distinct from read)

    // Network chart line colors (dedicated, separate from CPU to avoid cross-chart confusion)
    ImVec4 chartNetTx; // Network transmit (sent) line
    ImVec4 chartNetRx; // Network receive line

    // Chart fill colors (semi-transparent versions for shaded plots)
    ImVec4 chartCpuFill;     // CPU usage fill
    ImVec4 chartMemoryFill;  // Memory usage fill
    ImVec4 chartIoFill;      // I/O read usage fill
    ImVec4 chartIoWriteFill; // I/O write usage fill
    ImVec4 chartNetTxFill;   // Network transmit fill
    ImVec4 chartNetRxFill;   // Network receive fill

    // CPU breakdown colors
    ImVec4 cpuUser;   // User CPU time
    ImVec4 cpuSystem; // System/kernel CPU time
    ImVec4 cpuIowait; // I/O wait time
    ImVec4 cpuIdle;   // Idle time

    // CPU breakdown fill colors (semi-transparent versions for stacked area charts)
    ImVec4 cpuUserFill;   // User CPU time fill
    ImVec4 cpuSystemFill; // System/kernel CPU time fill
    ImVec4 cpuIowaitFill; // I/O wait time fill
    ImVec4 cpuIdleFill;   // Idle time fill

    // GPU chart colors
    ImVec4 gpuUtilization;     // GPU utilization line
    ImVec4 gpuUtilizationFill; // GPU utilization fill
    ImVec4 gpuMemory;          // GPU memory line
    ImVec4 gpuMemoryFill;      // GPU memory fill
    ImVec4 gpuTemperature;     // GPU temperature line
    ImVec4 gpuPower;           // GPU power line
    ImVec4 gpuEncoder;         // GPU encoder utilization
    ImVec4 gpuDecoder;         // GPU decoder utilization
    ImVec4 gpuClock;           // GPU clock speed line
    ImVec4 gpuClockFill;       // GPU clock speed fill (semi-transparent)
    ImVec4 gpuFan;             // GPU fan speed line

    // Chart overlay colors
    ImVec4 chartPeakLine; // Peak value reference line (semi-transparent)

    // Success button colors (e.g., Apply, Resume)
    ImVec4 successButton;
    ImVec4 successButtonHovered;
    ImVec4 successButtonActive;

    // Close button colors (title bar ×)
    ImVec4 closeButtonHovered;
    ImVec4 closeButtonActive;

    // Priority slider gradient endpoint colors
    ImVec4 priorityHighColor;      // nice < 0 end (high priority; default: red/orange)
    ImVec4 priorityNormalColor;    // nice == 0 mid (normal priority; default: green)
    ImVec4 priorityLowColor;       // nice > 0 end (low priority; default: blue)
    ImVec4 priorityBadgeTextColor; // text drawn on the priority badge (white on dark themes, near-black on light)

    // ImGui style colors (base colors for UI chrome)
    ImVec4 windowBg;
    ImVec4 childBg;
    ImVec4 popupBg;
    ImVec4 border;
    ImVec4 borderShadow; // Border shadow (typically transparent)
    ImVec4 frameBg;
    ImVec4 frameBgHovered;
    ImVec4 frameBgActive;
    ImVec4 titleBg;
    ImVec4 titleBgActive;
    ImVec4 titleBgCollapsed;
    ImVec4 menuBarBg;
    ImVec4 statusBarBg; // Status bar background (distinct from window/menu)
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
    ImVec4 tabSelected;
    ImVec4 tabSelectedOverline;
    ImVec4 tabDimmed;
    ImVec4 tabDimmedSelected;
    ImVec4 tabDimmedSelectedOverline;
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

/// Information about a discovered theme
struct DiscoveredTheme
{
    std::string id;             ///< Theme identifier (filename without extension)
    std::string name;           ///< Display name from TOML [meta] section
    std::string description;    ///< Description from TOML [meta] section
    std::filesystem::path path; ///< Full path to the TOML file
};

/// Font size configuration (in points)
struct FontSizeConfig
{
    std::string_view name;
    float regularPt = 0.0F; // Body text
    float largePt = 0.0F;   // Headings
};

/// Font size presets in points (body / headings), indexed by FontSize. Fonts are rasterised at
/// pt * 96 / 72 px at 100 % display scale, so Small's 7 pt body is about 9.3 px: no preset sets body
/// text under 9 px (#1194).
inline constexpr auto FONT_SIZE_PRESETS = std::to_array<FontSizeConfig>({
    {.name = "Small", .regularPt = 7.0F, .largePt = 9.0F},
    {.name = "Medium", .regularPt = 8.0F, .largePt = 10.0F},
    {.name = "Large", .regularPt = 10.0F, .largePt = 12.0F},
    {.name = "Extra Large", .regularPt = 12.0F, .largePt = 14.0F},
    {.name = "Huge", .regularPt = 14.0F, .largePt = 16.0F},
    {.name = "Even Huger", .regularPt = 16.0F, .largePt = 18.0F},
});
static_assert(FONT_SIZE_PRESETS.size() == FONT_SIZE_COUNT);

/// The preset chart axis, legend and hint text is drawn at for a given body preset: one step smaller
/// than the body text, but never below Medium, so chart text stays at least Medium's body size
/// (about 10.7 px at 100 %) except at Small, where it matches the body text (#1194).
[[nodiscard]] constexpr auto chartFontSize(FontSize bodySize) -> FontSize
{
    switch (bodySize)
    {
    case FontSize::Small:
        return FontSize::Small;
    case FontSize::Medium:
    case FontSize::Large:
        return FontSize::Medium;
    case FontSize::ExtraLarge:
        return FontSize::Large;
    case FontSize::Huge:
        return FontSize::ExtraLarge;
    case FontSize::EvenHuger:
        return FontSize::Huge;
    case FontSize::Count:
        break;
    }
    return FontSize::Small;
}

/// Chart text size as a fraction of body text size at a given body preset (1.0 at Small and Medium).
[[nodiscard]] constexpr auto chartFontScale(FontSize bodySize) -> float
{
    const auto body = static_cast<std::size_t>(bodySize);
    if (body >= FONT_SIZE_PRESETS.size())
    {
        return 1.0F;
    }
    return FONT_SIZE_PRESETS[static_cast<std::size_t>(chartFontSize(bodySize))].regularPt / FONT_SIZE_PRESETS[body].regularPt;
}

/// Global theme manager - provides access to color schemes and font settings
class Theme
{
  public:
    /// Get the singleton instance
    static auto get() -> Theme&;

    Theme(const Theme&) = delete;
    auto operator=(const Theme&) -> Theme& = delete;
    Theme(Theme&&) = delete;
    auto operator=(Theme&&) -> Theme& = delete;

    /// Initialize themes by loading from TOML files
    /// @param themesDir Path to themes directory (e.g., "assets/themes")
    void loadThemes(const std::filesystem::path& themesDir);

    /// Get list of discovered themes
    [[nodiscard]] auto discoveredThemes() const -> const std::vector<DiscoveredTheme>&
    {
        return m_DiscoveredThemes;
    }

    /// Get current theme index
    [[nodiscard]] auto currentThemeIndex() const -> std::size_t
    {
        return m_CurrentThemeIndex;
    }

    /// Get current theme ID (filename without extension)
    [[nodiscard]] auto currentThemeId() const -> const std::string&;

    /// Set current theme by index (deferred to next frame to avoid mid-frame style changes)
    void setTheme(std::size_t index);

    /// Set current theme by ID (deferred to next frame)
    void setThemeById(std::string_view id);

    /// Apply current theme colors to ImGui style
    void applyImGuiStyle() const;

    /// Flush any queued theme, font-size or display-scale change.
    ///
    /// Must be called at the start of a frame, before any widget is laid out. Every one of those
    /// changes rewrites the global ImGui and ImPlot styles, and the settings dialog triggers them
    /// from an Apply button *inside* a live frame, so applying them where they are requested would
    /// leave that frame half laid out against the old style and half against the new one. setTheme()
    /// has deferred for this reason since it was written; setFontSize() and setDisplayScale() now
    /// defer the same way.
    ///
    /// @return true if a theme change was applied (a style-only rebuild does not count, as callers
    ///         use this to decide whether to re-read theme colors).
    auto applyPendingStyleChanges() -> bool;

    /// Get current color scheme
    [[nodiscard]] auto scheme() const -> const ColorScheme&;

    /// Get theme name
    [[nodiscard]] auto themeName(std::size_t index) const -> std::string_view;

    /// Get progress bar color based on percent
    [[nodiscard]] auto progressColor(double percent) const -> ImVec4;

    /// Get accent color by index (wraps around)
    [[nodiscard]] auto accentColor(std::size_t index) const -> ImVec4;

    /// Number of accent colors
    [[nodiscard]] static constexpr auto accentCount() -> std::size_t
    {
        return 8;
    }

    // ============ Font Size Management ============

    /// Record the display scale from SDL_GetWindowDisplayScale(), 1.0 at 96 DPI.
    ///
    /// Feeds the ImGuiStyle scale factor so chrome tracks display density as well as font size
    /// (#936). The fonts are baked at the same density, so the two must change together: UILayer
    /// re-measures the scale when SDL reports a display-scale change, rebuilds the fonts at the new
    /// density and then calls this, so text and chrome rescale at the same frame boundary (#943).
    /// Re-scaling the style alone would grow the chrome while the text stayed put.
    ///
    /// Queues the rebuild rather than performing it, like setTheme() -- see
    /// applyPendingStyleChanges(), which flushes it at the next frame boundary.
    void setDisplayScale(float scale);

    [[nodiscard]] auto displayScale() const -> float
    {
        return m_DisplayScale;
    }

    /// Factor applyImGuiStyle() multiplies its size literals by: the font preset relative to
    /// Medium, times the display scale (see computeStyleScale()).
    ///
    /// For the few call sites that push their own padding over the style's -- the tab bars, the
    /// shell's content gutter -- so that what they push scales the same way as what they replace.
    /// A literal pushed over a scaled style value is the fixed-pixel bug again (#971).
    [[nodiscard]] auto styleScale() const -> float;

    /// Get current font size preset
    [[nodiscard]] auto currentFontSize() const -> FontSize
    {
        return m_CurrentFontSize;
    }

    /// Select a font size preset.
    ///
    /// Does not rebuild any font: every preset is pre-baked into the atlas at startup, so this
    /// only changes which one regularFont()/largeFont() hand out. It does queue a style rebuild,
    /// so padding and spacing follow the new size -- see applyPendingStyleChanges().
    void setFontSize(FontSize size);

    /// Get font size config
    [[nodiscard]] auto fontConfig() const -> const FontSizeConfig&;
    [[nodiscard]] auto fontConfig(FontSize size) const -> const FontSizeConfig&;

    /// Increase font size (returns true if changed)
    auto increaseFontSize() -> bool;

    /// Decrease font size (returns true if changed)
    auto decreaseFontSize() -> bool;

    // ============ Pre-baked Font Access ============

    /// Get the current regular font (based on font size setting)
    [[nodiscard]] auto regularFont() const -> ImFont*;

    /// Get the current large/heading font (based on font size setting)
    [[nodiscard]] auto largeFont() const -> ImFont*;

    /// Get the current monospace font (based on font size setting); falls back to regular if unset
    [[nodiscard]] auto monospaceFont() const -> ImFont*;

    /// Get the font for chart axis labels, legends and hints (see chartFontSize())
    [[nodiscard]] auto chartFont() const -> ImFont*;

    /// Get the title font (Sixtyfour pixel font for custom title bar)
    [[nodiscard]] auto titleFont() const -> ImFont*;

    /// Register pre-baked fonts (called by UILayer during initialization)
    void registerFonts(FontSize size, ImFont* regular, ImFont* large, ImFont* monospace);

    /// Register the title-bar display font (called by UILayer during initialization).
    void registerTitleFont(ImFont* font);

    /// Forget every registered font, before the font atlas is cleared to be rebuilt (#943).
    ///
    /// The title and chrome-icon fonts are optional -- re-registered only if their files load -- so
    /// without this a failed reload would leave them pointing into the freed atlas. Also advances
    /// fontGeneration().
    void clearFontRegistrations();

    /// Counts font atlas rebuilds. A cache keyed on an ImFont* must also compare this: a rebuilt
    /// atlas can hand out a font at the address the old one had, so the pointer alone cannot tell
    /// the fonts apart (#943).
    [[nodiscard]] auto fontGeneration() const -> std::uint64_t
    {
        return m_FontGeneration;
    }

    /// Register the fixed-size icon font used for the title bar's window and app controls.
    ///
    /// Separate from the body fonts because the icon ranges are merged into each of those at that
    /// font's size, which made the chrome controls track the Font Size setting.
    void registerChromeIconFont(ImFont* font, float sizePx);

    /// Icon font for title-bar controls; null if the icon font could not be loaded.
    [[nodiscard]] auto chromeIconFont() const -> ImFont*
    {
        return m_ChromeIconFont;
    }

    /// Pixel size the chrome icon font was rasterized at. Callers that need a glyph drawn at a
    /// different size push it relative to this rather than re-deriving it from the bar height.
    [[nodiscard]] auto chromeIconFontSizePx() const -> float
    {
        return m_ChromeIconFontSizePx;
    }

    /// Record the title bar's height in pixels, converted from points against the display scale.
    void setTitleBarHeightPx(float heightPx);

    /// Title bar height in pixels. Valid outside an ImGui frame, which SDL's hit-test callback
    /// requires.
    [[nodiscard]] auto titleBarHeightPx() const -> float
    {
        return m_TitleBarHeightPx;
    }

  private:
    Theme();
    ~Theme() = default;

    std::vector<DiscoveredTheme> m_DiscoveredThemes;
    std::vector<ColorScheme> m_LoadedSchemes;
    std::size_t m_CurrentThemeIndex = 0;
    std::optional<std::size_t> m_PendingThemeIndex; // Deferred theme change (applied next frame)
    // Set when a font-size or display-scale change needs the style rebuilt; flushed at the next
    // frame boundary by applyPendingStyleChanges().
    bool m_StyleDirty = false;

    FontSize m_CurrentFontSize = FontSize::Medium;

    // Display density, 1.0 at 96 DPI. Defaults to 1.0 so the style is sane before the window exists.
    float m_DisplayScale = 1.0F;
    std::array<FontSizeConfig, FONT_SIZE_COUNT> m_FontSizes;

    // Pre-baked fonts for each size preset (regular and large variants)
    struct FontPair
    {
        ImFont* regular = nullptr;
        ImFont* large = nullptr;
        ImFont* monospace = nullptr;
    };
    std::array<FontPair, FONT_SIZE_COUNT> m_Fonts{};
    ImFont* m_TitleFont = nullptr;      // Sixtyfour pixel font for title bar
    ImFont* m_ChromeIconFont = nullptr; // Fixed-size Font Awesome for title-bar controls
    // Pixel size m_ChromeIconFont was rasterized at; overwritten by registerChromeIconFont().
    float m_ChromeIconFontSizePx = 18.0F;
    std::uint64_t m_FontGeneration = 0; // see fontGeneration()
    // Title bar height in pixels. Defaults to 24pt at a 1.0 display scale so geometry is usable
    // before fonts load; overwritten by setTitleBarHeightPx().
    float m_TitleBarHeightPx = 32.0F;

    void initializeFontSizes();
    void loadDefaultFallbackTheme();
};

// Helper to convert hex color to ImVec4 (compile-time friendly)
constexpr ImVec4 hexToImVec4(std::uint32_t hex)
{
    return {static_cast<float>((hex >> 16) & 0xFF) / 255.0F,
            static_cast<float>((hex >> 8) & 0xFF) / 255.0F,
            static_cast<float>(hex & 0xFF) / 255.0F,
            1.0F};
}

/// One em of chart text (axis labels, legends, hints) at the current preset and display scale. Call
/// it outside a PlotFontGuard: it scales the body em, ImGui::GetFontSize(), by chartFontScale().
[[nodiscard]] inline auto chartEmPx() -> float
{
    return ImGui::GetFontSize() * chartFontScale(Theme::get().currentFontSize());
}

/// Return a copy of a color with a different alpha value.
[[nodiscard]] inline ImVec4 withAlpha(const ImVec4& color, float alpha) noexcept
{
    return {color.x, color.y, color.z, alpha};
}

} // namespace UI
