#include "UserConfig.h"

#include "App/UserConfigHelpers.h"
#include "Domain/Numeric.h"
#include "Domain/SamplingConfig.h"
#include "ProcessColumnConfig.h"
#include "UI/Theme.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

#include <toml++/toml.hpp>

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
    const bool hasTraversal = std::ranges::any_of(normalized, [](const auto& part) { return part == ".."; });
    if (!normalized.is_absolute() || hasTraversal)
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

    if (!std::filesystem::exists(m_ConfigPath))
    {
        spdlog::info("No config file found at {}, using defaults", m_ConfigPath.string());
        return;
    }

    try
    {
        auto config = toml::parse_file(m_ConfigPath.string());

        // Sampling / refresh interval
        UserConfigHelpers::loadAndNarrowInt64(config,
                                              "sampling",
                                              "interval_ms",
                                              m_Settings.refreshIntervalMs,
                                              Domain::Sampling::REFRESH_INTERVAL_DEFAULT_MS,
                                              [](auto v) { return Domain::Sampling::clampRefreshInterval(v); });

        UserConfigHelpers::loadAndNarrowInt64(config,
                                              "sampling",
                                              "history_max_seconds",
                                              m_Settings.maxHistorySeconds,
                                              Domain::Sampling::HISTORY_SECONDS_DEFAULT,
                                              [](auto v) { return Domain::Sampling::clampHistorySeconds(v); });
        // When the key is missing we intentionally keep the default (300s) set in UserSettings.

        // PDH instance refresh interval (Windows-only, controls how often PDH refreshes GPU process instances)
        UserConfigHelpers::loadAndNarrowInt64(config,
                                              "sampling",
                                              "pdh_instance_refresh_seconds",
                                              m_Settings.pdhInstanceRefreshSeconds,
                                              Domain::Sampling::PDH_INSTANCE_REFRESH_SECONDS_DEFAULT,
                                              [](auto v) { return Domain::Sampling::clampPdhInstanceRefreshSeconds(v); });

        // Socket stats cache TTL (Linux-only, controls how long per-process network stats are cached)
        UserConfigHelpers::loadAndNarrowInt64(config,
                                              "sampling",
                                              "socket_stats_cache_ttl_ms",
                                              m_Settings.socketStatsCacheTtlMs,
                                              Domain::Sampling::SOCKET_STATS_CACHE_TTL_MS_DEFAULT,
                                              [](auto v) { return Domain::Sampling::clampSocketStatsCacheTtlMs(v); });

        // Metrics calculation parameters
        UserConfigHelpers::loadAndClamp(config,
                                        "metrics",
                                        "min_time_for_rate_seconds",
                                        m_Settings.minTimeForRateSeconds,
                                        [](auto v) { return Domain::Sampling::clampMinTimeForRateSeconds(v); });

        UserConfigHelpers::loadAndClamp(config,
                                        "metrics",
                                        "max_sane_rate_bps",
                                        m_Settings.maxSaneRateBps,
                                        [](auto v) { return Domain::Sampling::clampMaxSaneRateBps(v); });

        if (auto val = config["metrics"]["integrated_gpu_vram_threshold_mb"].value<std::int64_t>())
        {
            // Check for overflow before MB-to-bytes conversion
            constexpr int64_t MAX_MB_BEFORE_OVERFLOW = std::numeric_limits<int64_t>::max() / (1024LL * 1024LL);
            const int64_t mb = std::clamp(*val, static_cast<int64_t>(0), MAX_MB_BEFORE_OVERFLOW);
            const int64_t bytes = mb * 1024LL * 1024LL;
            m_Settings.integratedGpuVramThresholdBytes = Domain::Sampling::clampIntegratedGpuVramThresholdBytes(bytes);
        }

        // UI behavior parameters
        UserConfigHelpers::loadAndClamp(config,
                                        "ui",
                                        "chart_smooth_factor",
                                        m_Settings.chartSmoothFactor,
                                        [](auto v) { return Domain::Sampling::clampChartSmoothFactor(v); });

        UserConfigHelpers::loadAndNarrowInt64(config,
                                              "ui",
                                              "chart_tau_ms_min",
                                              m_Settings.chartTauMsMin,
                                              Domain::Sampling::CHART_TAU_MS_MIN_DEFAULT,
                                              [](auto v) { return Domain::Sampling::clampChartTauMsMin(v); });

        UserConfigHelpers::loadAndNarrowInt64(config,
                                              "ui",
                                              "chart_tau_ms_max",
                                              m_Settings.chartTauMsMax,
                                              Domain::Sampling::CHART_TAU_MS_MAX_DEFAULT,
                                              [](auto v) { return Domain::Sampling::clampChartTauMsMax(v); });

        UserConfigHelpers::loadAndClamp(config,
                                        "ui",
                                        "progress_color_low_threshold",
                                        m_Settings.progressColorLowThreshold,
                                        [](auto v) { return Domain::Sampling::clampProgressColorLowThreshold(v); });

        UserConfigHelpers::loadAndClamp(config,
                                        "ui",
                                        "progress_color_high_threshold",
                                        m_Settings.progressColorHighThreshold,
                                        [](auto v) { return Domain::Sampling::clampProgressColorHighThreshold(v); });

        // Validate that low <= high threshold
        if (m_Settings.progressColorLowThreshold > m_Settings.progressColorHighThreshold)
        {
            spdlog::warn("User config: progress_color_low_threshold ({}) > progress_color_high_threshold ({}); "
                         "swapping to maintain low <= high.",
                         m_Settings.progressColorLowThreshold,
                         m_Settings.progressColorHighThreshold);
            std::swap(m_Settings.progressColorLowThreshold, m_Settings.progressColorHighThreshold);
        }

        // Theme
        if (auto theme = config["theme"]["id"].value<std::string>())
        {
            m_Settings.themeId = *theme;
        }

        // Font size
        if (auto fontSizeStr = config["font"]["size"].value<std::string>())
        {
            if (*fontSizeStr == "small")
            {
                m_Settings.fontSize = UI::FontSize::Small;
            }
            else if (*fontSizeStr == "medium")
            {
                m_Settings.fontSize = UI::FontSize::Medium;
            }
            else if (*fontSizeStr == "large")
            {
                m_Settings.fontSize = UI::FontSize::Large;
            }
            else if (*fontSizeStr == "extra-large")
            {
                m_Settings.fontSize = UI::FontSize::ExtraLarge;
            }
            else if (*fontSizeStr == "huge")
            {
                m_Settings.fontSize = UI::FontSize::Huge;
            }
            else if (*fontSizeStr == "even-huger")
            {
                m_Settings.fontSize = UI::FontSize::EvenHuger;
            }
        }

        // Note: panels visibility is no longer used (removed in favor of tabbed UI)

        // Window state
        UserConfigHelpers::loadAndNarrowIntWithClamp(config, "window", "width", m_Settings.windowWidth, 800, 200, 16'384);
        UserConfigHelpers::loadAndNarrowIntWithClamp(config, "window", "height", m_Settings.windowHeight, 600, 200, 16'384);
        if (auto val = config["window"]["x"].value<std::int64_t>())
        {
            // Use default x position of 100 if narrowOr fails
            const int x = Domain::Numeric::narrowOr<int>(*val, 100);
            if (isSaneWindowPositionComponent(x))
            {
                m_Settings.windowPosX = x;
            }
            else
            {
                m_Settings.windowPosX.reset();
            }
        }
        if (auto val = config["window"]["y"].value<std::int64_t>())
        {
            // Use default y position of 100 if narrowOr fails
            const int y = Domain::Numeric::narrowOr<int>(*val, 100);
            if (isSaneWindowPositionComponent(y))
            {
                m_Settings.windowPosY = y;
            }
            else
            {
                m_Settings.windowPosY.reset();
            }
        }
        if (auto val = config["window"]["maximized"].value<bool>())
        {
            m_Settings.windowMaximized = *val;
        }

        // Process panel column visibility
        if (auto* cols = config["process_columns"].as_table())
        {
            for (std::size_t i = 0; i < std::to_underlying(ProcessColumn::Count); ++i)
            {
                const auto col = processColumnFromIndex(i);
                const auto info = getColumnInfo(col);
                if (auto* node = cols->get(info.configKey); node != nullptr)
                {
                    if (auto val = node->value<bool>())
                    {
                        m_Settings.processColumns.setVisible(col, *val);
                    }
                }
            }
        }

        // Note: imgui_layout is no longer used (removed in favor of tabbed UI)

        spdlog::info("Loaded config from {}", m_ConfigPath.string());
    }
    catch (const toml::parse_error& err)
    {
        spdlog::error("Failed to parse config file: {}", err.what());
    }
}

void UserConfig::save() const
{
    // Ensure config directory exists
    const std::filesystem::path configDir = m_ConfigPath.parent_path();
    if (!std::filesystem::exists(configDir))
    {
        std::error_code ec;
        std::filesystem::create_directories(configDir, ec);
        if (ec)
        {
            spdlog::error("Failed to create config directory {}: {}", configDir.string(), ec.message());
            return;
        }
    }

    // Convert font size to string
    std::string fontSizeStr;
    switch (m_Settings.fontSize)
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
        processColumnsTable.insert(std::string(info.configKey), m_Settings.processColumns.isVisible(col));
    }

    // Build TOML document
    auto windowTable = toml::table{
        {"width", m_Settings.windowWidth},
        {"height", m_Settings.windowHeight},
        {"maximized", m_Settings.windowMaximized},
    };

    if (m_Settings.windowPosX.has_value())
    {
        windowTable.insert("x", *m_Settings.windowPosX);
    }
    if (m_Settings.windowPosY.has_value())
    {
        windowTable.insert("y", *m_Settings.windowPosY);
    }

    auto config = toml::table{
        {"sampling",
         toml::table{
             {"interval_ms", Domain::Sampling::clampRefreshInterval(m_Settings.refreshIntervalMs)},
             {"history_max_seconds", Domain::Sampling::clampHistorySeconds(m_Settings.maxHistorySeconds)},
             {"pdh_instance_refresh_seconds", Domain::Sampling::clampPdhInstanceRefreshSeconds(m_Settings.pdhInstanceRefreshSeconds)},
             {"socket_stats_cache_ttl_ms", Domain::Sampling::clampSocketStatsCacheTtlMs(m_Settings.socketStatsCacheTtlMs)},
         }},
        {"metrics",
         toml::table{
             {"min_time_for_rate_seconds", Domain::Sampling::clampMinTimeForRateSeconds(m_Settings.minTimeForRateSeconds)},
             {"max_sane_rate_bps", Domain::Sampling::clampMaxSaneRateBps(m_Settings.maxSaneRateBps)},
             {"integrated_gpu_vram_threshold_mb", m_Settings.integratedGpuVramThresholdBytes / (1024LL * 1024LL)},
         }},
        {"ui",
         toml::table{
             {"chart_smooth_factor", Domain::Sampling::clampChartSmoothFactor(m_Settings.chartSmoothFactor)},
             {"chart_tau_ms_min", Domain::Sampling::clampChartTauMsMin(m_Settings.chartTauMsMin)},
             {"chart_tau_ms_max", Domain::Sampling::clampChartTauMsMax(m_Settings.chartTauMsMax)},
             {"progress_color_low_threshold", Domain::Sampling::clampProgressColorLowThreshold(m_Settings.progressColorLowThreshold)},
             {"progress_color_high_threshold", Domain::Sampling::clampProgressColorHighThreshold(m_Settings.progressColorHighThreshold)},
         }},
        {"theme", toml::table{{"id", m_Settings.themeId}}},
        {"font", toml::table{{"size", fontSizeStr}}},
        {"window", windowTable},
        {"process_columns", processColumnsTable},
    };

    // Write to file
    std::ofstream file(m_ConfigPath);
    if (!file)
    {
        spdlog::error("Failed to open config file for writing: {}", m_ConfigPath.string());
        return;
    }

    file << "# TaskSmack user configuration\n";
    file << "# This file is auto-generated. Manual edits are preserved.\n";
    file << "# Notes:\n";
    file << "#   [sampling] interval_ms: refresh cadence (100-5000ms); affects all samplers\n";
    file << "#   [sampling] history_max_seconds: timeline history window (10-1800s)\n";
    file << "#   [sampling] pdh_instance_refresh_seconds: Windows only; GPU process discovery interval (1-60s)\n";
    file << "#   [sampling] socket_stats_cache_ttl_ms: Linux only; per-process network stat cache TTL (0-5000ms)\n";
    file << "#   [metrics] min_time_for_rate_seconds: delay before computing network rates (0.0-5.0s); avoids early spikes\n";
    file << "#   [metrics] max_sane_rate_bps: sanity check for network/IO rates (bytes/sec); clamps outliers\n";
    file << "#   [metrics] integrated_gpu_vram_threshold_mb: GPU classification threshold (16-512MB)\n";
    file << "#   [ui] chart_smooth_factor: exponential smoothing for charts (0.0-0.95); 0=no smoothing, 0.95=max smoothing\n";
    file << "#   [ui] chart_tau_ms_min/max: adaptive smoothing time constant range (ms); affects chart responsiveness\n";
    file << "#   [ui] progress_color_low/high_threshold: color change percentages for progress bars\n";
    file << "#   [process_columns]: toggle columns on/off; true shows the column\n";
    file << "#   Themes: built-in themes in assets/themes. Add custom .toml themes beside this config under a 'themes' folder.\n\n";
    file << config;

    spdlog::info("Saved config to {}", m_ConfigPath.string());
}

void UserConfig::applyToApplication() const
{
    auto& theme = UI::Theme::get();

    // Apply theme and font size
    theme.setThemeById(m_Settings.themeId);
    theme.setFontSize(m_Settings.fontSize);
}

void UserConfig::captureFromApplication()
{
    auto& theme = UI::Theme::get();

    // Capture current theme and font size
    m_Settings.themeId = theme.currentThemeId();
    m_Settings.fontSize = theme.currentFontSize();
}

} // namespace App
