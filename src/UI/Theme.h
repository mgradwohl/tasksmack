#pragma once

#include "UI/FontPointSize.h"

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

/// Color scheme definition with accent colors
struct ColorScheme
{
    std::string name;

    // Accent colors for line charts, legends, etc. (8 colors)
    std::array<ImVec4, 8> accents{};

    // Semantic UI colors
    ImVec4 textPrimary;  // Primary text color
    ImVec4 textDisabled; // Disabled text color
    ImVec4 textMuted;    // Dimmed/secondary text (labels, hints)
    ImVec4 textError;    // Error messages
    ImVec4 textWarning;  // Warning messages
    ImVec4 textSuccess;  // Success messages
    ImVec4 textInfo;     // Informational text

    // CPU Cores markers on a hybrid CPU (#1536): the P-core bolt, the E-core leaf and the dimmer
    // low-power E-core leaf (fallbacks: text_warning, text_success, text_muted).
    ImVec4 corePerformance;
    ImVec4 coreEfficiency;
    ImVec4 coreLowPower;

    // The Processes table's tint for a process that just started (#3060), drawn at a low, fading
    // opacity behind the row (fallback: text_success).
    ImVec4 processNew;
    // The tint for a process that just exited, kept as a dimmed row for a moment (#3060), drawn at a low,
    // steady opacity (fallback: text_error).
    ImVec4 processExiting;

    // Processes table inline meters (#1528): the heat gradient a cell's bar runs along as its value
    // rises, alpha included so the text stays readable on it ([process_meter] low / high; fallbacks:
    // text_warning at up to 0.15 alpha and text_error at up to 0.30, lowered until text_primary keeps
    // 4.5:1 on them over a plain, striped and selected row).
    ImVec4 processMeterLow;
    ImVec4 processMeterHigh;

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

    // Metric roles (#1196): every metric has its own colour field, so no chart borrows another
    // metric's colour. Each falls back to the field it used to borrow when a theme omits it, so older
    // themes look as they did (except process Power, which now matches system Power). A fill falls
    // back to its line at ~0.35 alpha, or to the borrowed fill; CPU Total, Page Faults and GDI are
    // drawn as lines only and have none.
    ImVec4 chartCpuTotal;          // CPU Total line over the User/System bands (fallback: charts.cpu)
    ImVec4 chartMemoryCached;      // System Memory: Cached (fallback: charts.cpu)
    ImVec4 chartMemoryCachedFill;  // (fallback: charts.cpu_fill)
    ImVec4 chartMemoryShared;      // Process Memory: Shared (fallback: charts.memory_cached)
    ImVec4 chartMemorySharedFill;  // (fallback: charts.memory_cached_fill)
    ImVec4 chartMemoryVirtual;     // Process Memory: Virtual (fallback: charts.swap)
    ImVec4 chartMemoryVirtualFill; // (fallback: charts.swap_fill)
    ImVec4 chartMemoryPrivate;     // Process Memory: Private (fallback: charts.memory_shared, #1885)
    ImVec4 chartMemoryPrivateFill; // (fallback: charts.memory_shared_fill)
    ImVec4 chartMemorySwap;        // Process Memory: Swap (fallback: charts.swap, #1885)
    ImVec4 chartMemorySwapFill;    // (fallback: charts.swap_fill)
    ImVec4 chartSwap;              // System Memory: Swap (fallback: charts.io)
    ImVec4 chartSwapFill;          // (fallback: charts.io_fill)
    ImVec4 chartPower;             // System and process power draw (fallback: charts.cpu)
    ImVec4 chartPowerFill;         // (fallback: charts.cpu_fill)
    ImVec4 chartBattery;           // Battery charge (fallback: charts.memory)
    ImVec4 chartBatteryFill;       // (fallback: charts.memory_fill)
    ImVec4 chartThreads;           // Thread count (fallback: charts.cpu)
    ImVec4 chartThreadsFill;       // (fallback: charts.cpu_fill)
    ImVec4 chartHandles;           // Handles / FDs (fallback: charts.memory)
    ImVec4 chartHandlesFill;       // (fallback: charts.memory_fill)
    ImVec4 chartPageFaults;        // Page faults per second (fallback: accents[3])
    ImVec4 chartGdi;               // GDI objects, Windows (fallback: accents[4])

    // Chart overlay colors
    ImVec4 chartPeakLine; // Peak value reference line (semi-transparent)

    // Success button colors (e.g., Apply, Resume)
    ImVec4 successButton;
    ImVec4 successButtonHovered;
    ImVec4 successButtonActive;

    // Danger button colors (Terminate, Kill): actions that end a process
    ImVec4 dangerButton;
    ImVec4 dangerButtonHovered;
    ImVec4 dangerButtonActive;

    // Close button colors (title bar ×)
    ImVec4 closeButtonHovered;
    ImVec4 closeButtonActive;

    // Priority slider gradient endpoint colors
    ImVec4 priorityHighColor;   // nice < 0 end (high priority; default: red/orange)
    ImVec4 priorityNormalColor; // nice == 0 mid (normal priority; default: green)
    ImVec4 priorityLowColor;    // nice > 0 end (low priority; default: blue)
    ImVec4
        priorityBadgeTextColor; // preferred badge text: drawn where it reaches 4.5:1 on the fill, else windowBg, else black/white (#1130)

    // ImGui style colors (base colors for UI chrome)
    ImVec4 windowBg;
    ImVec4 childBg;
    ImVec4 popupBg;
    ImVec4 border;
    ImVec4 borderShadow; // Border shadow (typically transparent)
    ImVec4 frameBg;
    ImVec4 frameBgHovered;
    ImVec4 frameBgActive;
    /// Outline of checkboxes, text fields, combo boxes and radio buttons (#1923): ui.frame.border, else
    /// ui.window.border, moved toward text_primary until it reaches ColorContrast::FRAME_BORDER_CONTRAST_MIN
    /// on the window, a card and a popup. Opaque. Drawn only inside UI::Widgets::FrameOutlineScope.
    ImVec4 frameBorder;
    /// Hatch of a NowBar with no reading (N/A, #1918, #1935): text_disabled, moved toward text_primary
    /// until it reaches ColorContrast::UNAVAILABLE_HATCH_CONTRAST_MIN on the bar's track (the frame
    /// fill over the window and over a card) and on the window and a card themselves. Opaque.
    ImVec4 unavailableHatch;
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
    /// Fills of a plain button (ImGuiCol_Button*) (#1976): ui.button.normal / hovered / active as the
    /// theme gives them when they already reach ColorContrast::BUTTON_FILL_CONTRAST_MIN on the window, a
    /// card and a popup (and hovered and active stay BUTTON_STATE_CONTRAST_MIN from normal), else moved
    /// toward text_primary until they do -- never so far that text_primary drops below
    /// ColorContrast::TEXT_CONTRAST_MIN on them. A moved fill is opaque.
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
    ImVec4 plotGrid; ///< Chart grid lines: ui.plot.grid, 1.3-1.8:1 on the plot (#1191); ui.window.border if absent
    ImVec4 plotLines;
    ImVec4 plotLinesHovered;
    ImVec4 plotHistogram;
    ImVec4 plotHistogramHovered;
    ImVec4 tableHeaderBg;
    ImVec4 tableBorderStrong;
    ImVec4 tableBorderLight;
    ImVec4 tableRowBg;
    ImVec4 tableRowBgAlt;
    /// Marker of a multi-selection's primary row, the one Process Details shows (#1926): a bar at the
    /// row's left edge and an outline round it. ui.table.selection_primary, else ui.misc.nav_highlight
    /// (ImGui's focus colour), moved toward text_primary until it reaches
    /// ColorContrast::SELECTION_PRIMARY_CONTRAST_MIN on the selection fill of a plain and a striped row.
    /// Opaque.
    ImVec4 selectionPrimary;
    /// The Processes tree's depth guide lines (#838), also ImGui's ImGuiCol_TreeLines: ui.table.border_light
    /// made opaque over the window, then moved toward text_primary until it reaches
    /// ColorContrast::TREE_LINES_CONTRAST_MIN on a plain and a striped row. Opaque.
    ImVec4 treeLines;
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

/// Global theme manager - provides access to color schemes and font settings
class Theme
{
  public:
    /// The style's FramePadding at the reference configuration, scaled by styleScale(). Named so a
    /// scope that overrides it for its own frames -- the panels' tall tabs -- can restore it for the
    /// content inside (TabContentScope).
    static constexpr float FRAME_PADDING_X = 4.0F;
    static constexpr float FRAME_PADDING_Y = 3.0F;

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
    /// has deferred for this reason since it was written; setFontPointSize() and setDisplayScale() now
    /// defer the same way.
    ///
    /// @return true if a theme change was applied (a style-only rebuild does not count, as callers
    ///         use this to decide whether to re-read theme colors).
    auto applyPendingStyleChanges() -> bool;

    /// Get current color scheme
    [[nodiscard]] auto scheme() const -> const ColorScheme&;

    /// Get accent color by index (wraps around)
    [[nodiscard]] auto accentColor(std::size_t index) const -> ImVec4;

    /// Number of accent colors
    [[nodiscard]] static constexpr auto accentCount() -> std::size_t
    {
        return 8;
    }

    // ============ Font Size Management ============

    /// Record the UI scale in window units (SDL display scale over pixel density, UI::windowUnitScale),
    /// 1.0 at 96 DPI.
    ///
    /// Feeds the ImGuiStyle scale factor so chrome tracks the UI scale as well as font size (#936).
    /// Font *sizes* are computed in the same window units, so the two must change together: UILayer
    /// re-measures the scale when SDL reports a display-scale change, rebuilds the fonts at the new
    /// scale and then calls this, so text and chrome rescale at the same frame boundary (#943).
    /// Re-scaling the style alone would grow the chrome while the text stayed put.
    ///
    /// Raster density is not part of this scale: ImGui rasterises at the window's pixel density
    /// itself (DisplayFramebufferScale). On native Wayland the display scale and the density change
    /// together, so this ratio -- and with it the fonts and style -- stays put, and no rebuild is
    /// needed; ImGui alone picks up the new density (#1096).
    ///
    /// Queues the rebuild rather than performing it, like setTheme() -- see
    /// applyPendingStyleChanges(), which flushes it at the next frame boundary.
    void setDisplayScale(float scale);

    [[nodiscard]] auto displayScale() const -> float
    {
        return m_DisplayScale;
    }

    /// The Interface scale setting (#3376): a percentage in [INTERFACE_SCALE_MIN_PERCENT,
    /// INTERFACE_SCALE_MAX_PERCENT] (DpiScale.h) that replaces the display's measured scale, or 0 for Auto. UILayer
    /// notices a change at the next frame boundary and rebuilds the fonts and style at effectiveDisplayScale().
    void setInterfaceScalePercent(int percent) noexcept
    {
        m_InterfaceScalePercent = percent;
    }

    [[nodiscard]] auto interfaceScalePercent() const noexcept -> int
    {
        return m_InterfaceScalePercent;
    }

    /// What the display last reported (UILayer's measurement), whatever the Interface scale setting says: Settings
    /// shows it beside "Auto". 1.0 before the first measurement.
    void setMeasuredDisplayScale(float scale) noexcept
    {
        m_MeasuredDisplayScale = scale;
    }

    [[nodiscard]] auto measuredDisplayScale() const noexcept -> float
    {
        return m_MeasuredDisplayScale;
    }

    /// Factor applyImGuiStyle() multiplies its size literals by: the font point size relative to
    /// the default 8 pt, times the display scale (see computeStyleScale()).
    ///
    /// For the few call sites that push their own padding over the style's -- the tab bars, the
    /// shell's content gutter -- so that what they push scales the same way as what they replace.
    /// A literal pushed over a scaled style value is the fixed-pixel bug again (#971).
    [[nodiscard]] auto styleScale() const -> float;

    /// The Font size setting (#3375): the body text size in whole points, in
    /// [FONT_POINT_SIZE_MIN, FONT_POINT_SIZE_MAX] (FontPointSize.h).
    [[nodiscard]] auto fontPointSize() const noexcept -> int
    {
        return m_FontPointSize;
    }

    /// Set the Font size setting, clamped to its range (#3375).
    ///
    /// Rebuilds no font itself: the atlas holds the fonts for one size, and UILayer notices the
    /// new size at the next frame boundary and rebuilds them there (fontsNeedRebuild()), as it does
    /// for a display-scale change. It queues a style rebuild too, so padding and spacing follow the
    /// new size at the same boundary -- see applyPendingStyleChanges().
    void setFontPointSize(int points);

    /// The body, heading and chart sizes, in points, for the current Font size setting.
    [[nodiscard]] auto fontConfig() const -> FontPointSizes
    {
        return fontPointSizesFor(m_FontPointSize);
    }

    /// One point larger, up to FONT_POINT_SIZE_MAX (Ctrl+=). @return Whether the size changed.
    auto increaseFontSize() -> bool;

    /// One point smaller, down to FONT_POINT_SIZE_MIN (Ctrl+-). @return Whether the size changed.
    auto decreaseFontSize() -> bool;

    /// The point size the registered fonts were built at (registerFonts()), or 0 before any are.
    [[nodiscard]] auto fontsBuiltPointSize() const noexcept -> int
    {
        return m_FontsBuiltPointSize;
    }

    /// Whether fonts are registered, but at another size than the Font size setting: UILayer then
    /// rebuilds the atlas at the next frame boundary (#3375).
    [[nodiscard]] auto fontsNeedRebuild() const noexcept -> bool
    {
        return m_FontsBuiltPointSize != 0 && m_FontsBuiltPointSize != m_FontPointSize;
    }

    // ============ Font Access ============

    /// Get the current regular font (based on font size setting)
    [[nodiscard]] auto regularFont() const -> ImFont*;

    /// Push regularFont() as the frame's body font, at the size it was built for.
    ///
    /// The one way to select the body font for a frame, shared by UILayer and the headless UI
    /// training driver so both draw at the same size (#2338). ImGui's PushFont(font, 0.0F) would keep
    /// the size already current -- at the start of a frame, that of the atlas's first font -- so this
    /// passes the font's LegacySize, the size it was baked for.
    ///
    /// @return The font pushed, which the caller pops with ImGui::PopFont() at the end of the frame;
    ///         nullptr, with nothing pushed, when no font is registered.
    [[nodiscard]] auto pushRegularFont() const -> ImFont*;

    /// Get the current large/heading font (based on font size setting)
    [[nodiscard]] auto largeFont() const -> ImFont*;

    /// Get the current bold font, at the body size (based on font size setting), for section headers
    /// (#1200). Falls back to the regular font when Inter-Bold could not be loaded.
    [[nodiscard]] auto boldFont() const -> ImFont*;

    /// Get the font for chart axis labels, legends and hints (see chartFontPointSize()). Falls back
    /// to the regular font when none was registered.
    [[nodiscard]] auto chartFont() const -> ImFont*;

    /// Get the title font (Sixtyfour pixel font for custom title bar)
    [[nodiscard]] auto titleFont() const -> ImFont*;

    /// Register the body and heading fonts, built at @p pointSize (loadAllFonts()). Clears the bold
    /// and chart fonts, which are registered after these.
    void registerFonts(int pointSize, ImFont* regular, ImFont* large);

    /// Register the bold body-size font. Optional: without one, boldFont() is the regular font.
    void registerBoldFont(ImFont* bold);

    /// Register the chart font. Optional: without one (its size is the body size, or it failed to
    /// load), chartFont() is the regular font.
    void registerChartFont(ImFont* chart);

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

    int m_FontPointSize = FONT_POINT_SIZE_DEFAULT; // The Font size setting (#3375)
    int m_FontsBuiltPointSize = 0;                 // What the registered fonts were built at; 0 = none

    // Display density, 1.0 at 96 DPI. Defaults to 1.0 so the style is sane before the window exists.
    float m_DisplayScale = 1.0F;
    int m_InterfaceScalePercent = 0;     // The Interface scale setting; 0 = Auto (#3376)
    float m_MeasuredDisplayScale = 1.0F; // The display's own report, for Settings' "Auto" label

    // The fonts for the current Font size setting, built by loadAllFonts() (#3375).
    struct FontSet
    {
        ImFont* regular = nullptr;
        ImFont* large = nullptr;
        ImFont* bold = nullptr;  // Body size, for section headers (#1200); optional
        ImFont* chart = nullptr; // Chart text size (chartFontPointSize()); optional
    };
    FontSet m_Fonts{};
    ImFont* m_TitleFont = nullptr;      // Sixtyfour pixel font for title bar
    ImFont* m_ChromeIconFont = nullptr; // Fixed-size Font Awesome for title-bar controls
    // Pixel size m_ChromeIconFont was rasterized at; overwritten by registerChromeIconFont().
    float m_ChromeIconFontSizePx = 18.0F;
    std::uint64_t m_FontGeneration = 0; // see fontGeneration()
    // Title bar height in pixels. Defaults to 24pt at a 1.0 display scale so geometry is usable
    // before fonts load; overwritten by setTitleBarHeightPx().
    float m_TitleBarHeightPx = 32.0F;

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

/// One em of chart text (axis labels, legends, hints) at the current font size and display scale. Call
/// it outside a PlotFontGuard: it scales the body em, ImGui::GetFontSize(), by chartFontScale().
[[nodiscard]] inline auto chartEmPx() -> float
{
    return ImGui::GetFontSize() * chartFontScale(Theme::get().fontPointSize());
}

/// Return a copy of a color with a different alpha value.
[[nodiscard]] inline ImVec4 withAlpha(const ImVec4& color, float alpha) noexcept
{
    return {color.x, color.y, color.z, alpha};
}

} // namespace UI
