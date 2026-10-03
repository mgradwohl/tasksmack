#include "UserConfig.h"

#include "App/Panels/ProcessTableSettings.h"
#include "App/UserConfigHelpers.h"
#include "Core/WindowConstants.h"
#include "Domain/Numeric.h"
#include "Domain/SamplingConfig.h"
#include "ProcessColumnConfig.h"
#include "UI/ChartWidgets.h"
#include "UI/Theme.h"

#include <spdlog/spdlog.h>
#include <toml++/toml.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <ios>
#include <limits>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

#ifdef _WIN32
// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>
// clang-format on
#else
#include <array>

#include <pwd.h>
#include <unistd.h>
#endif

namespace App
{

namespace
{

// NOLINTNEXTLINE(bugprone-exception-escape) - spdlog logging may theoretically throw; acceptable in practice
[[nodiscard]] ProcessColumn processColumnFromIndex(const std::size_t index) noexcept
{
    const auto count = std::to_underlying(ProcessColumn::Count);
    if (index >= static_cast<std::size_t>(count))
    {
        spdlog::warn("processColumnFromIndex: index {} out of range [0, {})", index, count);
        return static_cast<ProcessColumn>(0);
    }
    return static_cast<ProcessColumn>(index);
}

constexpr int WINDOW_POS_ABS_MAX = 100'000;

[[nodiscard]] bool isSaneWindowPositionComponent(int value)
{
    return std::abs(value) <= WINDOW_POS_ABS_MAX;
}

// Helper functions moved to a testable header: App/UserConfigHelpers.h

#ifndef _WIN32
/// Read an environment variable as a string.
/// @param name The environment variable name (must be null-terminated for std::getenv)
/// @return The value if set and non-empty, or std::nullopt otherwise
/// Note: std::getenv requires const char*; using std::string_view would add no value.
[[nodiscard]] auto readEnvVarString(const char* name) -> std::optional<std::string>
{
    // NOLINT(concurrency-mt-unsafe): std::getenv is not thread-safe, but this function
    // is only called during single-threaded initialization (UserConfig constructor).
    const char* value = std::getenv(name); // NOLINT(concurrency-mt-unsafe)
    if (value == nullptr || value[0] == '\0')
    {
        return std::nullopt;
    }
    return std::string(value);
}

[[nodiscard]] auto sanitizeConfigDir(const std::filesystem::path& candidate, const std::filesystem::path& fallback) -> std::filesystem::path
{
    auto normalized = candidate.lexically_normal();
    if (!UserConfigHelpers::isValidConfigDir(normalized))
    {
        spdlog::warn("Ignoring unsafe config directory {}; using {}", normalized.string(), fallback.string());
        return fallback;
    }

    return normalized;
}

[[nodiscard]] auto resolveHomeConfigDir() -> std::filesystem::path
{
    if (auto homeEnv = readEnvVarString("HOME"))
    {
        return std::filesystem::path(*homeEnv) / ".config" / "tasksmack";
    }

    // Last resort: use passwd entry (thread-safe version)
    struct passwd pwBuf = {};
    struct passwd* pwResult = nullptr;
    std::array<char, 1024> buffer{};
    if (getpwuid_r(getuid(), &pwBuf, buffer.data(), buffer.size(), &pwResult) == 0 && pwResult != nullptr)
    {
        return std::filesystem::path(pwResult->pw_dir) / ".config" / "tasksmack";
    }

    return std::filesystem::current_path();
}
#endif

/// Reads every setting present in `config` into `settings`; keys that are absent leave the
/// existing value alone.
void readSettings(const toml::table& config, UserSettings& settings)
{

    // Sampling / refresh interval
    UserConfigHelpers::loadAndNarrowInt64(config,
                                          "sampling",
                                          "interval_ms",
                                          settings.refreshIntervalMs,
                                          Domain::Sampling::REFRESH_INTERVAL_DEFAULT_MS,
                                          [](auto v) { return Domain::Sampling::clampRefreshInterval(v); });

    UserConfigHelpers::loadAndNarrowInt64(config,
                                          "sampling",
                                          "history_max_seconds",
                                          settings.maxHistorySeconds,
                                          Domain::Sampling::HISTORY_SECONDS_DEFAULT,
                                          [](auto v) { return Domain::Sampling::clampHistorySeconds(v); });
    // When the key is missing we intentionally keep the default (300s) set in UserSettings.

    // Socket stats cache TTL (Linux-only, controls how long per-process network stats are cached)
    UserConfigHelpers::loadAndNarrowInt64(config,
                                          "sampling",
                                          "socket_stats_cache_ttl_ms",
                                          settings.socketStatsCacheTtlMs,
                                          Domain::Sampling::SOCKET_STATS_CACHE_TTL_MS_DEFAULT,
                                          [](auto v) { return Domain::Sampling::clampSocketStatsCacheTtlMs(v); });

    // Metrics calculation parameters
    UserConfigHelpers::loadAndClamp(config,
                                    "metrics",
                                    "min_time_for_rate_seconds",
                                    settings.minTimeForRateSeconds,
                                    [](auto v) { return Domain::Sampling::clampMinTimeForRateSeconds(v); });

    UserConfigHelpers::loadAndClamp(
        config, "metrics", "max_sane_rate_bps", settings.maxSaneRateBps, [](auto v) { return Domain::Sampling::clampMaxSaneRateBps(v); });

    if (auto val = config["metrics"]["integrated_gpu_vram_threshold_mb"].value<std::int64_t>())
    {
        // Check for overflow before MB-to-bytes conversion
        constexpr int64_t MAX_MB_BEFORE_OVERFLOW = std::numeric_limits<int64_t>::max() / (1024LL * 1024LL);
        const int64_t mb = std::clamp(*val, static_cast<int64_t>(0), MAX_MB_BEFORE_OVERFLOW);
        const int64_t bytes = mb * 1024LL * 1024LL;
        settings.integratedGpuVramThresholdBytes = Domain::Sampling::clampIntegratedGpuVramThresholdBytes(bytes);
    }

    // UI behavior parameters
    UserConfigHelpers::loadAndClamp(config,
                                    "ui",
                                    "chart_smooth_factor",
                                    settings.chartSmoothFactor,
                                    [](auto v) { return Domain::Sampling::clampChartSmoothFactor(v); });

    UserConfigHelpers::loadAndNarrowInt64(config,
                                          "ui",
                                          "chart_tau_ms_min",
                                          settings.chartTauMsMin,
                                          Domain::Sampling::CHART_TAU_MS_MIN_DEFAULT,
                                          [](auto v) { return Domain::Sampling::clampChartTauMsMin(v); });

    UserConfigHelpers::loadAndNarrowInt64(config,
                                          "ui",
                                          "chart_tau_ms_max",
                                          settings.chartTauMsMax,
                                          Domain::Sampling::CHART_TAU_MS_MAX_DEFAULT,
                                          [](auto v) { return Domain::Sampling::clampChartTauMsMax(v); });

    if (auto val = config["ui"]["chart_anti_aliasing"].value<bool>())
    {
        settings.chartAntiAliasing = *val;
    }

    UserConfigHelpers::loadAndClamp(config,
                                    "ui",
                                    "progress_color_low_threshold",
                                    settings.progressColorLowThreshold,
                                    [](auto v) { return Domain::Sampling::clampProgressColorLowThreshold(v); });

    UserConfigHelpers::loadAndClamp(config,
                                    "ui",
                                    "progress_color_high_threshold",
                                    settings.progressColorHighThreshold,
                                    [](auto v) { return Domain::Sampling::clampProgressColorHighThreshold(v); });

    // Validate that low <= high threshold
    if (settings.progressColorLowThreshold > settings.progressColorHighThreshold)
    {
        spdlog::warn("User config: progress_color_low_threshold ({}) > progress_color_high_threshold ({}); "
                     "swapping to maintain low <= high.",
                     settings.progressColorLowThreshold,
                     settings.progressColorHighThreshold);
        std::swap(settings.progressColorLowThreshold, settings.progressColorHighThreshold);
    }

    // Theme
    if (auto theme = config["theme"]["id"].value<std::string>())
    {
        settings.themeId = *theme;
    }

    // Font size
    if (auto fontSizeStr = config["font"]["size"].value<std::string>())
    {
        if (*fontSizeStr == "small")
        {
            settings.fontSize = UI::FontSize::Small;
        }
        else if (*fontSizeStr == "medium")
        {
            settings.fontSize = UI::FontSize::Medium;
        }
        else if (*fontSizeStr == "large")
        {
            settings.fontSize = UI::FontSize::Large;
        }
        else if (*fontSizeStr == "extra-large")
        {
            settings.fontSize = UI::FontSize::ExtraLarge;
        }
        else if (*fontSizeStr == "huge")
        {
            settings.fontSize = UI::FontSize::Huge;
        }
        else if (*fontSizeStr == "even-huger")
        {
            settings.fontSize = UI::FontSize::EvenHuger;
        }
    }

    // Note: panels visibility is no longer used (removed in favor of tabbed UI)

    // Window state
    UserConfigHelpers::loadAndNarrowIntWithClamp(
        config, "window", "width", settings.windowWidth, 800, Core::WINDOW_MIN_DIMENSION, Core::WINDOW_MAX_DIMENSION);
    UserConfigHelpers::loadAndNarrowIntWithClamp(
        config, "window", "height", settings.windowHeight, 600, Core::WINDOW_MIN_DIMENSION, Core::WINDOW_MAX_DIMENSION);
    if (auto val = config["window"]["x"].value<std::int64_t>())
    {
        // Use default x position of 100 if narrowOr fails
        const int x = Domain::Numeric::narrowOr<int>(*val, 100);
        if (isSaneWindowPositionComponent(x))
        {
            settings.windowPosX = x;
        }
        else
        {
            settings.windowPosX.reset();
        }
    }
    if (auto val = config["window"]["y"].value<std::int64_t>())
    {
        // Use default y position of 100 if narrowOr fails
        const int y = Domain::Numeric::narrowOr<int>(*val, 100);
        if (isSaneWindowPositionComponent(y))
        {
            settings.windowPosY = y;
        }
        else
        {
            settings.windowPosY.reset();
        }
    }
    if (auto val = config["window"]["maximized"].value<bool>())
    {
        settings.windowMaximized = *val;
    }
    if (auto val = config["window"]["force_native_decorations_on_wayland"].value<bool>())
    {
        settings.forceNativeWindowDecorationsOnWayland = *val;
    }

    // Privilege notice: suppress startup dialog if user dismissed it permanently
    if (auto val = config["ui"]["show_privilege_notice"].value<bool>())
    {
        settings.showPrivilegeNotice = *val;
    }

    // Process table column layout (widths, order, sort). Length-capped here; its content is
    // filtered where it is used, before ImGui parses it.
    if (auto layout = config["process_table"]["layout"].value<std::string>())
    {
        if (layout->size() <= ProcessTableSettings::MAX_STORED_BYTES)
        {
            settings.processTableLayout = std::move(*layout);
        }
    }

    // Process panel column visibility
    if (const auto* cols = config["process_columns"].as_table())
    {
        for (std::size_t i = 0; i < std::to_underlying(ProcessColumn::Count); ++i)
        {
            const auto col = processColumnFromIndex(i);
            const auto info = getColumnInfo(col);
            if (const auto* node = cols->get(info.configKey); node != nullptr)
            {
                if (auto val = node->value<bool>())
                {
                    settings.processColumns.setVisible(col, *val);
                }
            }
        }
    }

    // Note: imgui_layout is no longer used (removed in favor of tabbed UI)
}

/// The TOML document for `settings`: every key TaskSmack owns.
[[nodiscard]] toml::table buildTable(const UserSettings& settings)
{
    // Convert font size to string
    std::string fontSizeStr;
    switch (settings.fontSize)
    {
    case UI::FontSize::Small:
        fontSizeStr = "small";
        break;
    case UI::FontSize::Medium:
        fontSizeStr = "medium";
        break;
    case UI::FontSize::Large:
        fontSizeStr = "large";
        break;
    case UI::FontSize::ExtraLarge:
        fontSizeStr = "extra-large";
        break;
    case UI::FontSize::Huge:
        fontSizeStr = "huge";
        break;
    case UI::FontSize::EvenHuger:
        fontSizeStr = "even-huger";
        break;
    default:
        fontSizeStr = "medium";
        break;
    }

    // Build process columns table
    auto processColumnsTable = toml::table{};
    for (std::size_t i = 0; i < std::to_underlying(ProcessColumn::Count); ++i)
    {
        const auto col = processColumnFromIndex(i);
        const auto info = getColumnInfo(col);
        processColumnsTable.insert(std::string(info.configKey), settings.processColumns.isVisible(col));
    }

    // Build TOML document
    auto windowTable = toml::table{
        {"width", settings.windowWidth},
        {"height", settings.windowHeight},
        {"maximized", settings.windowMaximized},
        {"force_native_decorations_on_wayland", settings.forceNativeWindowDecorationsOnWayland},
    };

    if (settings.windowPosX.has_value())
    {
        windowTable.insert("x", *settings.windowPosX);
    }
    if (settings.windowPosY.has_value())
    {
        windowTable.insert("y", *settings.windowPosY);
    }

    return toml::table{
        {"sampling",
         toml::table{
             {"interval_ms", Domain::Sampling::clampRefreshInterval(settings.refreshIntervalMs)},
             {"history_max_seconds", Domain::Sampling::clampHistorySeconds(settings.maxHistorySeconds)},
             {"socket_stats_cache_ttl_ms", Domain::Sampling::clampSocketStatsCacheTtlMs(settings.socketStatsCacheTtlMs)},
         }},
        {"metrics",
         toml::table{
             {"min_time_for_rate_seconds", Domain::Sampling::clampMinTimeForRateSeconds(settings.minTimeForRateSeconds)},
             {"max_sane_rate_bps", Domain::Sampling::clampMaxSaneRateBps(settings.maxSaneRateBps)},
             {"integrated_gpu_vram_threshold_mb", settings.integratedGpuVramThresholdBytes / (1024LL * 1024LL)},
         }},
        {"ui",
         toml::table{
             {"chart_smooth_factor", Domain::Sampling::clampChartSmoothFactor(settings.chartSmoothFactor)},
             {"chart_tau_ms_min", Domain::Sampling::clampChartTauMsMin(settings.chartTauMsMin)},
             {"chart_tau_ms_max", Domain::Sampling::clampChartTauMsMax(settings.chartTauMsMax)},
             {"progress_color_low_threshold", Domain::Sampling::clampProgressColorLowThreshold(settings.progressColorLowThreshold)},
             {"progress_color_high_threshold", Domain::Sampling::clampProgressColorHighThreshold(settings.progressColorHighThreshold)},
             {"show_privilege_notice", settings.showPrivilegeNotice},
             {"chart_anti_aliasing", settings.chartAntiAliasing},
         }},
        {"theme", toml::table{{"id", settings.themeId}}},
        {"font", toml::table{{"size", fontSizeStr}}},
        {"window", windowTable},
        {"process_columns", processColumnsTable},
        {"process_table", toml::table{{"layout", settings.processTableLayout}}},
    };
}

/// Lays `owned` (TaskSmack's keys) over `document` (the file as it is), so tables and keys
/// TaskSmack doesn't own survive a save (#1122).
void overlay(toml::table& document, const toml::table& owned)
{
    for (const auto& [key, value] : owned)
    {
        auto* existing = document.get_as<toml::table>(key);
        if (const auto* ownedTable = value.as_table(); ownedTable != nullptr && existing != nullptr)
        {
            for (const auto& [subKey, subValue] : *ownedTable)
            {
                existing->insert_or_assign(subKey, subValue);
            }
        }
        else
        {
            document.insert_or_assign(key, value);
        }
    }
}

[[nodiscard]] std::optional<std::filesystem::file_time_type> lastWriteTime(const std::filesystem::path& path)
{
    std::error_code ec;
    const auto time = std::filesystem::last_write_time(path, ec);
    if (ec)
    {
        return std::nullopt;
    }
    return time;
}

} // namespace

auto UserConfig::get() -> UserConfig&
{
    static UserConfig instance;
    return instance;
}

UserConfig::UserConfig()
{
    m_ConfigPath = getConfigDirectory() / "config.toml";
    spdlog::debug("Config path: {}", m_ConfigPath.string());
}

auto UserConfig::getConfigDirectory() -> std::filesystem::path
{
#ifdef _WIN32
    // Windows: %APPDATA%/TaskSmack
    wchar_t* appDataPath = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appDataPath)))
    {
        std::filesystem::path configDir = std::filesystem::path(appDataPath) / "TaskSmack";
        CoTaskMemFree(appDataPath);
        return configDir;
    }
    // Fallback to current directory
    return std::filesystem::current_path();
#else
    // Linux: XDG_CONFIG_HOME or ~/.config
    // Ensure fallback is always safe; it comes from resolveHomeConfigDir() which returns absolute paths
    auto fallback = resolveHomeConfigDir();
    if (!fallback.is_absolute())
    {
        spdlog::error("Fallback config directory is not absolute: {}", fallback.string());
        return std::filesystem::current_path();
    }

    if (auto xdgConfig = readEnvVarString("XDG_CONFIG_HOME"))
    {
        return sanitizeConfigDir(std::filesystem::path(*xdgConfig) / "tasksmack", fallback);
    }

    // Fallback is guaranteed absolute, so just return it directly
    return fallback;
#endif
}

void UserConfig::load()
{
    if (m_IsLoaded)
    {
        return;
    }
    m_IsLoaded = true;

    // error_code overloads: an unreadable config directory falls back to defaults instead of
    // throwing out of startup (#1124).
    std::error_code ec;
    if (!std::filesystem::exists(m_ConfigPath, ec))
    {
        if (ec)
        {
            spdlog::warn("Can't check config file {}: {}; using defaults", m_ConfigPath.string(), ec.message());
        }
        else
        {
            spdlog::info("No config file found at {}, using defaults", m_ConfigPath.string());
        }
        markSynced();
        return;
    }

    try
    {
        const auto config = toml::parse_file(m_ConfigPath.string());
        readSettings(config, m_Settings);
        spdlog::info("Loaded config from {}", m_ConfigPath.string());
    }
    catch (const toml::parse_error& err)
    {
        spdlog::error("Failed to parse config file: {}", err.what());
    }
    markSynced();
}

void UserConfig::markSynced()
{
    m_Baseline = m_Settings;
    m_SyncedWriteTime = lastWriteTime(m_ConfigPath);
}

void UserConfig::save()
{
    // Ensure config directory exists
    const std::filesystem::path configDir = m_ConfigPath.parent_path();
    std::error_code ec;
    if (!std::filesystem::exists(configDir, ec))
    {
        std::filesystem::create_directories(configDir, ec);
        if (ec)
        {
            spdlog::error("Failed to create config directory {}: {}", configDir.string(), ec.message());
            return;
        }
    }

    // Start from the file as it is, so keys and tables TaskSmack doesn't own are kept (#1122).
    toml::table document;
    const auto writeTime = lastWriteTime(m_ConfigPath);
    bool parsed = false;
    if (writeTime.has_value())
    {
        try
        {
            document = toml::parse_file(m_ConfigPath.string());
            parsed = true;
        }
        catch (const toml::parse_error& err)
        {
            spdlog::warn("Rewriting unparseable config file {}: {}", m_ConfigPath.string(), err.what());
        }
    }

    // Edited outside TaskSmack since it was last read or written (e.g. via Settings' "Edit Config
    // File")? Keep those edits for every setting TaskSmack hasn't itself changed since then.
    if (parsed && writeTime != m_SyncedWriteTime)
    {
        UserSettings external = m_Baseline;
        readSettings(document, external);
        m_Settings = mergeSettings(external, m_Baseline, m_Settings);
        spdlog::info("Config file {} was edited outside TaskSmack; keeping those edits", m_ConfigPath.string());
    }

    overlay(document, buildTable(m_Settings));

    // Write a temporary file beside it and rename it over the original, so a crash, power loss or
    // full disk mid-write can't leave an empty or truncated config (#1122).
    std::filesystem::path tempPath = m_ConfigPath;
    tempPath += ".tmp";
    {
        std::ofstream file(tempPath, std::ios::trunc);
        if (!file)
        {
            spdlog::error("Failed to open {} for writing", tempPath.string());
            return;
        }

        file << "# TaskSmack user configuration\n";
        file << "# Written by TaskSmack. Keys it doesn't use are kept, but comments in this file are not.\n";
        file << "# Edits made while TaskSmack is running are kept unless TaskSmack changes the same setting.\n";
        file << "# Notes:\n";
        file << "#   [sampling] interval_ms: refresh cadence (100-5000ms); affects all samplers\n";
        file << "#   [sampling] history_max_seconds: timeline history window (10-1800s)\n";
        file << "#   [sampling] socket_stats_cache_ttl_ms: Linux only; per-process network stat cache TTL (0-5000ms)\n";
        file << "#   [metrics] min_time_for_rate_seconds: delay before computing network rates (0.0-5.0s); avoids early spikes\n";
        file << "#   [metrics] max_sane_rate_bps: sanity check for network/IO rates (bytes/sec); clamps outliers\n";
        file << "#   [metrics] integrated_gpu_vram_threshold_mb: GPU classification threshold (16-512MB)\n";
        file << "#   [ui] chart_smooth_factor: exponential smoothing for charts (0.0-0.95); 0=no smoothing, 0.95=max smoothing\n";
        file << "#   [ui] chart_tau_ms_min/max: adaptive smoothing time constant range (ms); affects chart responsiveness\n";
        file << "#   [ui] progress_color_low/high_threshold: color change percentages for progress bars\n";
        file << "#   [ui] show_privilege_notice: show startup dialog when running without elevated privileges (true/false)\n";
        file << "#   [ui] chart_anti_aliasing: smooth chart line/fill edges (true/false); disable for lower CPU/GPU cost "
                "on integrated GPUs\n";
        file << "#   [process_columns]: toggle columns on/off; true shows the column\n";
        file << "#   [process_table] layout: saved column widths, order and sort (written by TaskSmack; delete it to reset)\n";
        file << "#   Themes: built-in themes in assets/themes. Add custom .toml themes beside this config under a 'themes' folder.\n\n";
        file << document;
        file.close();
        if (!file)
        {
            spdlog::error("Failed to write {}: stream error after write", tempPath.string());
            std::filesystem::remove(tempPath, ec);
            return;
        }
    }

    std::filesystem::rename(tempPath, m_ConfigPath, ec);
    if (ec)
    {
        spdlog::error("Failed to replace {} with the new config: {}", m_ConfigPath.string(), ec.message());
        std::filesystem::remove(tempPath, ec);
        return;
    }

    spdlog::info("Saved config to {}", m_ConfigPath.string());
    markSynced();

    // Reset so that the next load() call re-reads from disk (e.g., for test round-trips
    // or any future live-reload use case).
    m_IsLoaded = false;
}

void UserConfig::applyToApplication() const
{
    auto& theme = UI::Theme::get();

    // Apply theme and font size
    theme.setThemeById(m_Settings.themeId);
    theme.setFontSize(m_Settings.fontSize);

    // Push the anti-aliasing preference into UI (which must not depend on App/UserConfig
    // directly -- see tasksmack.md's Dependency Rules). Config-file-only for now, like the
    // adjacent chart_smooth_factor/chart_tau_ms_min/max tuning knobs: not exposed as a Settings
    // UI toggle, so re-applying only at startup (no live-change path to wire up) is sufficient.
    UI::Widgets::setChartAntiAliasingEnabled(m_Settings.chartAntiAliasing);
}

void UserConfig::captureFromApplication()
{
    auto& theme = UI::Theme::get();

    // Capture current theme and font size
    m_Settings.themeId = theme.currentThemeId();
    m_Settings.fontSize = theme.currentFontSize();
}

} // namespace App
