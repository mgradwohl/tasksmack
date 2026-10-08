#include "ThemeLoader.h"

#include "Theme.h"
#include "UI/Format.h"

#include <imgui.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

// NOLINTBEGIN(misc-include-cleaner) - toml++ is an umbrella header; sub-header symbols (node,
// node_view, table, parse_file, parse_error) cannot be included individually. Suppress false
// positives at include and all usage sites throughout this file.
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
    return {1.0F, 0.0F, 1.0F, 1.0F}; // Bright magenta
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

    // Each field must parse AND consume both of its characters. A successful error code alone is
    // not enough: from_chars("FZ", ..., 16) stops at 'Z' and still reports success, so "#FZ0000"
    // used to be accepted as the colour 0x0F0000. Comparing ptr against the field's end is what
    // rejects that -- and is why the returned pointer, previously bound and ignored (which is what
    // CodeQL cpp/unused-local-variable was pointing at), is needed rather than discarded.
    const std::from_chars_result red = std::from_chars(hexData, hexData + 2, r, 16);
    const std::from_chars_result greenResult = std::from_chars(hexData + 2, hexData + 4, g, 16);
    const std::from_chars_result blueResult = std::from_chars(hexData + 4, hexData + 6, b, 16);

    const bool rgbParsed = red.ec == std::errc{} && red.ptr == hexData + 2 && greenResult.ec == std::errc{} &&
                           greenResult.ptr == hexData + 4 && blueResult.ec == std::errc{} && blueResult.ptr == hexData + 6;
    if (!rgbParsed)
    {
        spdlog::warn("Invalid hex color: {} (contains non-hex characters)", hex);
        return errorColor();
    }

    // Parse alpha if present (8-digit hex)
    if (hex.size() == 8)
    {
        // Same full-consumption requirement as the RGB fields above.
        const std::from_chars_result alpha = std::from_chars(hexData + 6, hexData + 8, a, 16);
        if (alpha.ec != std::errc{} || alpha.ptr != hexData + 8)
        {
            spdlog::warn("Invalid hex color alpha: {}", hex);
            return errorColor();
        }
    }

    constexpr float INV_MAX_COMPONENT = 1.0F / 255.0F;
    return {UI::Format::toFloatNarrow(r) * INV_MAX_COMPONENT,
            UI::Format::toFloatNarrow(g) * INV_MAX_COMPONENT,
            UI::Format::toFloatNarrow(b) * INV_MAX_COMPONENT,
            UI::Format::toFloatNarrow(a) * INV_MAX_COMPONENT};
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
            return {r, g, b, a};
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
                return {r, g, b, a};
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

/// A metric role's line and fill colours (#1196).
struct RoleColors
{
    ImVec4 line;
    ImVec4 fill;
};

/// A metric role's line and fill. A theme that sets the line but not the fill gets the line at ~0.35
/// alpha, like the other chart fills; a theme without the role keeps the colours the role used to
/// borrow (@p borrowedLine, @p borrowedFill), so older user themes draw as they did.
auto getRoleColors(const toml::table& tbl,
                   std::string_view key,
                   std::string_view fillKey,
                   const ImVec4& borrowedLine,
                   const ImVec4& borrowedFill) -> RoleColors
{
    if (tbl.at_path(key))
    {
        const ImVec4 line = parseColorView(tbl.at_path(key));
        return {.line = line, .fill = getColor(tbl, fillKey, withAlpha(line, line.w * 0.35F))};
    }
    return {.line = borrowedLine, .fill = getColor(tbl, fillKey, borrowedFill)};
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

/// Builds a ColorScheme from a parsed theme document. A missing or malformed colour becomes
/// errorColor() (or its documented fallback) rather than failing the whole theme.
auto schemeFromTable(const toml::table& tbl) -> ColorScheme
{
    ColorScheme scheme{};

    // Meta
    if (const auto* meta = tbl["meta"].as_table())
    {
        scheme.name = (*meta)["name"].value_or(std::string{"Unknown"});
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
    scheme.chartIoWrite = getColor(tbl, "charts.io_write", scheme.chartMemory);

    // Network chart colors; fall back to chartCpu/chartMemory for backward compat.
    // chartMemory is a required field guaranteed to be loaded by this point, so
    // it is a safer fallback than accents[2] which could be black in malformed themes.
    scheme.chartNetTx = getColor(tbl, "charts.net_tx", scheme.chartCpu);
    scheme.chartNetRx = getColor(tbl, "charts.net_rx", scheme.chartMemory);

    // Chart fill colors: fall back to the line color at ~0.35 alpha (matching plotLineWithFill's
    // implicit fill behavior) so themes that omit fill keys get a translucent fill, not an opaque one.
    scheme.chartCpuFill = getColor(tbl, "charts.cpu_fill", withAlpha(scheme.chartCpu, (scheme.chartCpu.w * 0.35F)));
    scheme.chartMemoryFill = getColor(tbl, "charts.memory_fill", withAlpha(scheme.chartMemory, (scheme.chartMemory.w * 0.35F)));
    scheme.chartIoFill = getColor(tbl, "charts.io_fill", withAlpha(scheme.chartIo, (scheme.chartIo.w * 0.35F)));
    scheme.chartIoWriteFill = getColor(tbl, "charts.io_write_fill", withAlpha(scheme.chartIoWrite, (scheme.chartIoWrite.w * 0.35F)));
    scheme.chartNetTxFill = getColor(tbl, "charts.net_tx_fill", withAlpha(scheme.chartNetTx, (scheme.chartNetTx.w * 0.35F)));
    scheme.chartNetRxFill = getColor(tbl, "charts.net_rx_fill", withAlpha(scheme.chartNetRx, (scheme.chartNetRx.w * 0.35F)));

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
    // Fill fallbacks translucent like the chart fills above, not the opaque line colour.
    scheme.gpuUtilizationFill =
        getColor(tbl, "charts.gpu.utilization_fill", withAlpha(scheme.gpuUtilization, (scheme.gpuUtilization.w * 0.35F)));
    scheme.gpuMemory = getColor(tbl, "charts.gpu.memory");
    scheme.gpuMemoryFill = getColor(tbl, "charts.gpu.memory_fill", withAlpha(scheme.gpuMemory, (scheme.gpuMemory.w * 0.35F)));
    scheme.gpuTemperature = getColor(tbl, "charts.gpu.temperature");
    scheme.gpuPower = getColor(tbl, "charts.gpu.power");
    scheme.gpuEncoder = getColor(tbl, "charts.gpu.encoder");
    scheme.gpuDecoder = getColor(tbl, "charts.gpu.decoder");
    scheme.gpuClock = getColor(tbl, "charts.gpu.clock");
    scheme.gpuClockFill = getColor(tbl, "charts.gpu.clock_fill", withAlpha(scheme.gpuClock, (scheme.gpuClock.w * 0.35F)));
    scheme.gpuFan = getColor(tbl, "charts.gpu.fan");

    // Metric roles (#1196). Each falls back to the field it borrowed before it had its own, so a
    // theme that predates them draws as it did -- except process Power, which was semantic.text_info
    // and now matches system Power (charts.cpu unless charts.power is set): Power is one colour on
    // every screen. Shared and Virtual fall back through Cached and Swap, which in turn fall back to
    // what they borrowed.
    const auto cpuTotal = getRoleColors(tbl, "charts.cpu_total", "charts.cpu_total_fill", scheme.chartCpu, scheme.chartCpuFill);
    scheme.chartCpuTotal = cpuTotal.line;
    scheme.chartCpuTotalFill = cpuTotal.fill;
    const auto cached = getRoleColors(tbl, "charts.memory_cached", "charts.memory_cached_fill", scheme.chartCpu, scheme.chartCpuFill);
    scheme.chartMemoryCached = cached.line;
    scheme.chartMemoryCachedFill = cached.fill;
    const auto shared = getRoleColors(tbl, "charts.memory_shared", "charts.memory_shared_fill", cached.line, cached.fill);
    scheme.chartMemoryShared = shared.line;
    scheme.chartMemorySharedFill = shared.fill;
    const auto swap = getRoleColors(tbl, "charts.swap", "charts.swap_fill", scheme.chartIo, scheme.chartIoFill);
    scheme.chartSwap = swap.line;
    scheme.chartSwapFill = swap.fill;
    const auto virt = getRoleColors(tbl, "charts.memory_virtual", "charts.memory_virtual_fill", swap.line, swap.fill);
    scheme.chartMemoryVirtual = virt.line;
    scheme.chartMemoryVirtualFill = virt.fill;
    const auto power = getRoleColors(tbl, "charts.power", "charts.power_fill", scheme.chartCpu, scheme.chartCpuFill);
    scheme.chartPower = power.line;
    scheme.chartPowerFill = power.fill;
    const auto battery = getRoleColors(tbl, "charts.battery", "charts.battery_fill", scheme.chartMemory, scheme.chartMemoryFill);
    scheme.chartBattery = battery.line;
    scheme.chartBatteryFill = battery.fill;
    const auto threads = getRoleColors(tbl, "charts.threads", "charts.threads_fill", scheme.chartCpu, scheme.chartCpuFill);
    scheme.chartThreads = threads.line;
    scheme.chartThreadsFill = threads.fill;
    const auto handles = getRoleColors(tbl, "charts.handles", "charts.handles_fill", scheme.chartMemory, scheme.chartMemoryFill);
    scheme.chartHandles = handles.line;
    scheme.chartHandlesFill = handles.fill;
    scheme.chartPageFaults = getColor(tbl, "charts.page_faults", scheme.accents[3]);
    scheme.chartGdi = getColor(tbl, "charts.gdi", scheme.accents[4]);

    // Chart overlays
    scheme.chartPeakLine = getColor(tbl, "charts.peak_line", scheme.textWarning);

    // Success buttons (e.g., Apply, Resume)
    scheme.successButton = getColor(tbl, "buttons.success.normal");
    scheme.successButtonHovered = getColor(tbl, "buttons.success.hovered");
    scheme.successButtonActive = getColor(tbl, "buttons.success.active");

    // Danger buttons (Terminate, Kill); default to the same reds as the close button, a step
    // darker at rest, so a theme without the section still sets them apart (#1273)
    scheme.dangerButton = getColor(tbl, "buttons.danger.normal", ImVec4(0.64F, 0.08F, 0.08F, 1.0F));
    scheme.dangerButtonHovered = getColor(tbl, "buttons.danger.hovered", ImVec4(0.8F, 0.1F, 0.1F, 1.0F));
    scheme.dangerButtonActive = getColor(tbl, "buttons.danger.active", ImVec4(0.9F, 0.2F, 0.2F, 1.0F));

    // Close button (title bar ×); defaults to conventional dark-red hover/active
    scheme.closeButtonHovered = getColor(tbl, "buttons.close.hovered", ImVec4(0.8F, 0.1F, 0.1F, 1.0F));
    scheme.closeButtonActive = getColor(tbl, "buttons.close.active", ImVec4(0.9F, 0.2F, 0.2F, 1.0F));

    // Priority slider gradient endpoints; defaults match legacy hardcoded values
    scheme.priorityHighColor = getColor(tbl, "priority.high", ImVec4(1.0F, 0.3F, 0.2F, 1.0F));
    scheme.priorityNormalColor = getColor(tbl, "priority.normal", ImVec4(0.5F, 0.8F, 0.2F, 1.0F));
    scheme.priorityLowColor = getColor(tbl, "priority.low", ImVec4(0.4F, 0.4F, 0.8F, 1.0F));
    // Badge text: white on dark themes, near-black on light; defaults to white for safety
    scheme.priorityBadgeTextColor = getColor(tbl, "priority.badge_text_color", ImVec4(1.0F, 1.0F, 1.0F, 1.0F));

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
    // Grid lines were the window border, which nearly vanished on the plot (1.01:1 on Tokyo Night).
    // Themes without their own grid colour keep that behaviour (#1191).
    scheme.plotGrid = getColor(tbl, "ui.plot.grid", scheme.border);
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

    return scheme;
}

} // namespace

auto ThemeLoader::discoverThemes(const std::filesystem::path& themesDir) -> std::vector<ThemeInfo>
{
    std::vector<ThemeInfo> themes;

    // error_code overloads throughout: a theme directory that can't be read (EACCES, a symlink
    // loop) is skipped with a warning instead of throwing out of UILayer::onAttach (#1127).
    std::error_code ec;
    if (!std::filesystem::is_directory(themesDir, ec))
    {
        spdlog::warn("Themes directory does not exist or can't be read: {}", themesDir.string());
        return themes;
    }

    std::filesystem::directory_iterator it(themesDir, ec);
    if (ec)
    {
        spdlog::warn("Can't list themes directory {}: {}", themesDir.string(), ec.message());
        return themes;
    }
    for (const std::filesystem::directory_iterator end; !ec && it != end; it.increment(ec))
    {
        std::error_code fileEc;
        if (it->is_regular_file(fileEc) && it->path().extension() == ".toml")
        {
            if (auto info = loadThemeInfo(it->path()))
            {
                themes.push_back(std::move(*info));
            }
        }
    }
    if (ec)
    {
        spdlog::warn("Stopped listing themes directory {}: {}", themesDir.string(), ec.message());
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

        // Read meta section. Index through node_view, which is null-safe: table::get() returns
        // nullptr for a missing key, and calling value_or() on that crashed at startup (#1095).
        if (const auto* meta = tbl["meta"].as_table())
        {
            info.name = (*meta)["name"].value_or(info.id);
            info.description = (*meta)["description"].value_or(std::string{});
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
        ColorScheme scheme = schemeFromTable(toml::parse_file(path.string()));
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

auto ThemeLoader::loadThemeFromString(std::string_view tomlText, std::string_view sourceName) -> std::optional<ColorScheme>
{
    try
    {
        return schemeFromTable(toml::parse(tomlText, sourceName));
    }
    catch (const toml::parse_error& err)
    {
        spdlog::error("Failed to parse theme {}: {}", sourceName, err.description());
        return std::nullopt;
    }
    catch (const std::exception& ex)
    {
        spdlog::error("Failed to load theme {}: {}", sourceName, ex.what());
        return std::nullopt;
    }
}

// NOLINTEND(misc-include-cleaner)
} // namespace UI
