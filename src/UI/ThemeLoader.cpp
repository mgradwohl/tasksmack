#include "ThemeLoader.h"

#include "UI/Numeric.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <charconv>
#include <optional>

#include <toml++/toml.hpp>

namespace UI
{

namespace
{

/// Return a bright magenta color to make missing/invalid colors obvious
/// This is intentionally NOT reading from the current theme to avoid
/// circular dependencies when loading a new theme
[[nodiscard]] constexpr auto errorColor() -> ImVec4
{
    return ImVec4(1.0F, 0.0F, 1.0F, 1.0F); // Bright magenta
}

} // namespace

auto ThemeLoader::hexToImVec4(std::string_view hex) -> ImVec4
{
    // Strip leading # if present
    if (!hex.empty() && hex[0] == '#')
    {
        hex = hex.substr(1);
    }

    // Support both 6-digit (RRGGBB) and 8-digit (RRGGBBAA) hex
    if (hex.size() != 6 && hex.size() != 8)
    {
        spdlog::warn("Invalid hex color: {} (expected 6 or 8 digits)", hex);
        return errorColor();
    }

    unsigned int r = 0;
    unsigned int g = 0;
    unsigned int b = 0;
    unsigned int a = 255; // Default to fully opaque

    // Use string_view's data() directly - no need to create a string copy
    const char* hexData = hex.data();

    // Parse RGB components
    auto [ptr1, ec1] = std::from_chars(hexData, hexData + 2, r, 16);
    auto [ptr2, ec2] = std::from_chars(hexData + 2, hexData + 4, g, 16);
    auto [ptr3, ec3] = std::from_chars(hexData + 4, hexData + 6, b, 16);

    if (ec1 != std::errc{} || ec2 != std::errc{} || ec3 != std::errc{})
    {
        spdlog::warn("Invalid hex color: {} (contains non-hex characters)", hex);
        return errorColor();
    }

    // Parse alpha if present (8-digit hex)
    if (hex.size() == 8)
    {
        auto [ptrA, ecA] = std::from_chars(hexData + 6, hexData + 8, a, 16);
        if (ecA != std::errc{})
        {
            spdlog::warn("Invalid hex color alpha: {}", hex);
            return errorColor();
        }
    }

    constexpr float INV_MAX_COMPONENT = 1.0F / 255.0F;
    return ImVec4(UI::Numeric::toFloatNarrow(r) * INV_MAX_COMPONENT,
                  UI::Numeric::toFloatNarrow(g) * INV_MAX_COMPONENT,
                  UI::Numeric::toFloatNarrow(b) * INV_MAX_COMPONENT,
                  UI::Numeric::toFloatNarrow(a) * INV_MAX_COMPONENT);
}

namespace
{

/// Parse a color from a TOML node (hex string or [r,g,b,a] array)
auto parseColorNode(const toml::node& node) -> ImVec4
{
    if (node.is_string())
    {
        const auto str = node.value<std::string>();
        if (str.has_value())
        {
            return ThemeLoader::hexToImVec4(*str);
        }
        spdlog::warn("Invalid color string node");
        return errorColor();
    }

    if (node.is_array())
    {
        const auto* arr = node.as_array();
        if (arr && arr->size() >= 3)
        {
            const float r = arr->get(0)->value_or(0.0F);
            const float g = arr->get(1)->value_or(0.0F);
            const float b = arr->get(2)->value_or(0.0F);
            const float a = (arr->size() >= 4) ? arr->get(3)->value_or(1.0F) : 1.0F;
            return ImVec4(r, g, b, a);
        }
    }

    spdlog::warn("Invalid color node type");
    return errorColor();
}

/// Parse color from node_view (returned by at_path)
auto parseColorView(toml::node_view<const toml::node> view) -> ImVec4
{
    if (view.is_string())
    {
        const auto str = view.value<std::string>();
        if (str.has_value())
        {
            return ThemeLoader::hexToImVec4(*str);
        }
        spdlog::warn("Invalid color string node");
        return errorColor();
    }

    if (view.is_array())
    {
        if (const auto* arr = view.as_array())
        {
            if (arr->size() >= 3)
            {
                const float r = arr->get(0)->value_or(0.0F);
                const float g = arr->get(1)->value_or(0.0F);
                const float b = arr->get(2)->value_or(0.0F);
                const float a = (arr->size() >= 4) ? arr->get(3)->value_or(1.0F) : 1.0F;
                return ImVec4(r, g, b, a);
            }
        }
    }

    spdlog::warn("Invalid color node type");
    return errorColor();
}

/// Get a color from a table, with default fallback
/// If the key is missing and no default is provided, logs a warning and returns errorColor()
auto getColor(const toml::table& tbl, std::string_view key, std::optional<ImVec4> defaultColor = std::nullopt) -> ImVec4
{
    if (auto node = tbl.at_path(key))
    {
        return parseColorView(node);
    }
    if (defaultColor.has_value())
    {
        return *defaultColor;
    }
    // Key is missing and no default provided - this is a theme authoring error
    spdlog::warn("Theme missing required color key: '{}'", key);
    return errorColor();
}

/// Load a color array (e.g., accent colors)
template<std::size_t N> void loadColorArray(const toml::table& tbl, std::string_view key, std::array<ImVec4, N>& colors)
{
    if (const auto* arr = tbl.at_path(key).as_array())
    {
        for (std::size_t i = 0; i < std::min(N, arr->size()); ++i)
        {
            colors[i] = parseColorNode(*arr->get(i));
        }
    }
}

} // namespace

auto ThemeLoader::discoverThemes(const std::filesystem::path& themesDir) -> std::vector<ThemeInfo>
{
    std::vector<ThemeInfo> themes;

    if (!std::filesystem::exists(themesDir))
    {
        spdlog::warn("Themes directory does not exist: {}", themesDir.string());
        return themes;
    }

    for (const auto& entry : std::filesystem::directory_iterator(themesDir))
    {
        if (entry.is_regular_file() && entry.path().extension() == ".toml")
        {
            if (auto info = loadThemeInfo(entry.path()))
            {
                themes.push_back(std::move(*info));
            }
        }
    }

    // Sort by name for consistent UI ordering
    std::ranges::sort(themes, [](const ThemeInfo& a, const ThemeInfo& b) { return a.name < b.name; });

    return themes;
}

auto ThemeLoader::loadThemeInfo(const std::filesystem::path& path) -> std::optional<ThemeInfo>
{
    try
    {
        auto tbl = toml::parse_file(path.string());

        ThemeInfo info;
        info.path = path;
        info.id = path.stem().string(); // filename without extension

        // Read meta section
        if (auto* meta = tbl["meta"].as_table())
        {
            info.name = meta->get("name")->value_or(info.id);
            info.description = meta->get("description")->value_or("");
        }
        else
        {
            info.name = info.id;
        }

        return info;
    }
    catch (const toml::parse_error& err)
    {
        spdlog::error("Failed to parse theme {}: {}", path.string(), err.description());
        return std::nullopt;
    }
}

auto ThemeLoader::loadTheme(const std::filesystem::path& path) -> std::optional<ColorScheme>
{
    try
    {
        auto tbl = toml::parse_file(path.string());
        ColorScheme scheme;

        // Meta
        if (auto* meta = tbl["meta"].as_table())
        {
            scheme.name = meta->get("name")->value_or("Unknown");
        }

        // Accents
        loadColorArray(tbl, "accents.colors", scheme.accents);

        // Progress colors
        scheme.progressLow = getColor(tbl, "progress.low");
        scheme.progressMedium = getColor(tbl, "progress.medium");
        scheme.progressHigh = getColor(tbl, "progress.high");

        // Semantic colors
        scheme.textMuted = getColor(tbl, "semantic.text_muted");
        scheme.textError = getColor(tbl, "semantic.text_error");
        scheme.textWarning = getColor(tbl, "semantic.text_warning");
        scheme.textSuccess = getColor(tbl, "semantic.text_success");
        scheme.textInfo = getColor(tbl, "semantic.text_info");
        scheme.textPrimary = getColor(tbl, "semantic.text_primary", scheme.textInfo);
        scheme.textDisabled = getColor(tbl, "semantic.text_disabled", scheme.textMuted);

        // Status colors
        scheme.statusRunning = getColor(tbl, "status.running");
        scheme.statusSleeping = getColor(tbl, "status.sleeping");
        scheme.statusDiskSleep = getColor(tbl, "status.disk_sleep");
        scheme.statusZombie = getColor(tbl, "status.zombie");
        scheme.statusStopped = getColor(tbl, "status.stopped");
        scheme.statusIdle = getColor(tbl, "status.idle");

        // Chart colors
        scheme.chartCpu = getColor(tbl, "charts.cpu");
        scheme.chartMemory = getColor(tbl, "charts.memory");
        scheme.chartIo = getColor(tbl, "charts.io");

        // Chart fill colors (with fallback to line colors for backward compatibility)
        scheme.chartCpuFill = getColor(tbl, "charts.cpu_fill", scheme.chartCpu);
        scheme.chartMemoryFill = getColor(tbl, "charts.memory_fill", scheme.chartMemory);
        scheme.chartIoFill = getColor(tbl, "charts.io_fill", scheme.chartIo);

        // CPU breakdown
        scheme.cpuUser = getColor(tbl, "cpu_breakdown.user");
        scheme.cpuSystem = getColor(tbl, "cpu_breakdown.system");
        scheme.cpuIowait = getColor(tbl, "cpu_breakdown.iowait");
        scheme.cpuIdle = getColor(tbl, "cpu_breakdown.idle");

        // CPU breakdown fill colors (with fallback to line colors for backward compatibility)
        scheme.cpuUserFill = getColor(tbl, "cpu_breakdown.user_fill", scheme.cpuUser);
        scheme.cpuSystemFill = getColor(tbl, "cpu_breakdown.system_fill", scheme.cpuSystem);
        scheme.cpuIowaitFill = getColor(tbl, "cpu_breakdown.iowait_fill", scheme.cpuIowait);
        scheme.cpuIdleFill = getColor(tbl, "cpu_breakdown.idle_fill", scheme.cpuIdle);

        // GPU chart colors
        scheme.gpuUtilization = getColor(tbl, "charts.gpu.utilization");
        scheme.gpuUtilizationFill = getColor(tbl, "charts.gpu.utilization_fill", scheme.gpuUtilization);
        scheme.gpuMemory = getColor(tbl, "charts.gpu.memory");
        scheme.gpuMemoryFill = getColor(tbl, "charts.gpu.memory_fill", scheme.gpuMemory);
        scheme.gpuTemperature = getColor(tbl, "charts.gpu.temperature");
        scheme.gpuPower = getColor(tbl, "charts.gpu.power");
        scheme.gpuEncoder = getColor(tbl, "charts.gpu.encoder");
        scheme.gpuDecoder = getColor(tbl, "charts.gpu.decoder");
        scheme.gpuClock = getColor(tbl, "charts.gpu.clock");
        scheme.gpuClockFill = getColor(tbl, "charts.gpu.clock_fill", scheme.gpuClock);
        scheme.gpuFan = getColor(tbl, "charts.gpu.fan");

        // Chart overlays
        scheme.chartPeakLine = getColor(tbl, "charts.peak_line", scheme.textWarning);

        // Success buttons (e.g., Apply, Resume)
        scheme.successButton = getColor(tbl, "buttons.success.normal");
        scheme.successButtonHovered = getColor(tbl, "buttons.success.hovered");
        scheme.successButtonActive = getColor(tbl, "buttons.success.active");

        // Window colors
        scheme.windowBg = getColor(tbl, "ui.window.background");
        scheme.childBg = getColor(tbl, "ui.window.child_background");
        scheme.popupBg = getColor(tbl, "ui.window.popup_background");
        scheme.border = getColor(tbl, "ui.window.border");
        scheme.borderShadow = getColor(tbl, "ui.window.border_shadow", scheme.border);

        // Frame colors
        scheme.frameBg = getColor(tbl, "ui.frame.background");
        scheme.frameBgHovered = getColor(tbl, "ui.frame.background_hovered");
        scheme.frameBgActive = getColor(tbl, "ui.frame.background_active");

        // Title bar colors
        scheme.titleBg = getColor(tbl, "ui.title.background");
        scheme.titleBgActive = getColor(tbl, "ui.title.background_active");
        scheme.titleBgCollapsed = getColor(tbl, "ui.title.background_collapsed");

        // Bar colors
        scheme.menuBarBg = getColor(tbl, "ui.bars.menu");
        scheme.statusBarBg = getColor(tbl, "ui.bars.status");

        // Scrollbar colors
        scheme.scrollbarBg = getColor(tbl, "ui.scrollbar.background");
        scheme.scrollbarGrab = getColor(tbl, "ui.scrollbar.grab");
        scheme.scrollbarGrabHovered = getColor(tbl, "ui.scrollbar.grab_hovered");
        scheme.scrollbarGrabActive = getColor(tbl, "ui.scrollbar.grab_active");

        // Control colors
        scheme.checkMark = getColor(tbl, "ui.controls.check_mark");
        scheme.sliderGrab = getColor(tbl, "ui.controls.slider_grab");
        scheme.sliderGrabActive = getColor(tbl, "ui.controls.slider_grab_active");

        // Button colors
        scheme.button = getColor(tbl, "ui.button.normal");
        scheme.buttonHovered = getColor(tbl, "ui.button.hovered");
        scheme.buttonActive = getColor(tbl, "ui.button.active");

        // Header colors
        scheme.header = getColor(tbl, "ui.header.normal");
        scheme.headerHovered = getColor(tbl, "ui.header.hovered");
        scheme.headerActive = getColor(tbl, "ui.header.active");

        // Separator colors
        scheme.separator = getColor(tbl, "ui.separator.normal");
        scheme.separatorHovered = getColor(tbl, "ui.separator.hovered");
        scheme.separatorActive = getColor(tbl, "ui.separator.active");

        // Resize grip colors
        scheme.resizeGrip = getColor(tbl, "ui.resize_grip.normal");
        scheme.resizeGripHovered = getColor(tbl, "ui.resize_grip.hovered");
        scheme.resizeGripActive = getColor(tbl, "ui.resize_grip.active");

        // Tab colors
        scheme.tab = getColor(tbl, "ui.tab.normal");
        scheme.tabHovered = getColor(tbl, "ui.tab.hovered");
        scheme.tabSelected = getColor(tbl, "ui.tab.active");
        scheme.tabSelectedOverline = getColor(tbl, "ui.tab.active_overline");
        scheme.tabDimmed = getColor(tbl, "ui.tab.unfocused");
        scheme.tabDimmedSelected = getColor(tbl, "ui.tab.unfocused_active");
        scheme.tabDimmedSelectedOverline = getColor(tbl, "ui.tab.unfocused_active_overline");

        // Docking colors
        scheme.dockingPreview = getColor(tbl, "ui.docking.preview");
        scheme.dockingEmptyBg = getColor(tbl, "ui.docking.empty_background");

        // Plot colors
        scheme.plotLines = getColor(tbl, "ui.plot.lines");
        scheme.plotLinesHovered = getColor(tbl, "ui.plot.lines_hovered");
        scheme.plotHistogram = getColor(tbl, "ui.plot.histogram");
        scheme.plotHistogramHovered = getColor(tbl, "ui.plot.histogram_hovered");

        // Table colors
        scheme.tableHeaderBg = getColor(tbl, "ui.table.header_background");
        scheme.tableBorderStrong = getColor(tbl, "ui.table.border_strong");
        scheme.tableBorderLight = getColor(tbl, "ui.table.border_light");
        scheme.tableRowBg = getColor(tbl, "ui.table.row_background");
        scheme.tableRowBgAlt = getColor(tbl, "ui.table.row_background_alt");

        // Misc UI colors
        scheme.textSelectedBg = getColor(tbl, "ui.misc.text_selected_background");
        scheme.dragDropTarget = getColor(tbl, "ui.misc.drag_drop_target");
        scheme.navHighlight = getColor(tbl, "ui.misc.nav_highlight");
        scheme.navWindowingHighlight = getColor(tbl, "ui.misc.nav_windowing_highlight");
        scheme.navWindowingDimBg = getColor(tbl, "ui.misc.nav_windowing_dim_background");
        scheme.modalWindowDimBg = getColor(tbl, "ui.misc.modal_window_dim_background");

        spdlog::info("Loaded theme: {} from {}", scheme.name, path.string());
        return scheme;
    }
    catch (const toml::parse_error& err)
    {
        spdlog::error("Failed to parse theme {}: {}", path.string(), err.description());
        return std::nullopt;
    }
    catch (const std::exception& ex)
    {
        spdlog::error("Failed to load theme {}: {}", path.string(), ex.what());
        return std::nullopt;
    }
}

} // namespace UI
