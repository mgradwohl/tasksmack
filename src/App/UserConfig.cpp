#include "UserConfig.h"

#include "Domain/Numeric.h"
#include "UI/Theme.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
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
#include <cstdlib>

#include <pwd.h>
#include <unistd.h>
#endif

namespace App
{

namespace
{

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

#ifndef _WIN32
[[nodiscard]] auto readEnvVarString(const char* name) -> std::optional<std::string>
{
    const char* value = std::getenv(name); // NOLINT(concurrency-mt-unsafe)
    if (value == nullptr || value[0] == '\0')
    {
        return std::nullopt;
    }
    return std::string(value);
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
    if (auto xdgConfig = readEnvVarString("XDG_CONFIG_HOME"))
    {
        return std::filesystem::path(*xdgConfig) / "tasksmack";
    }

    // Fall back to ~/.config
    if (auto homeEnv = readEnvVarString("HOME"))
    {
        return std::filesystem::path(*homeEnv) / ".config" / "tasksmack";
    }

    // Last resort: use passwd entry
    if (const auto* pw = getpwuid(getuid()))
    {
        return std::filesystem::path(pw->pw_dir) / ".config" / "tasksmack";
    }

    return std::filesystem::current_path();
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
        if (auto val = config["sampling"]["interval_ms"].value<std::int64_t>())
        {
            // Clamp function will handle out-of-range values; use default if narrowOr fails
            m_Settings.refreshIntervalMs =
                Domain::Sampling::clampRefreshInterval(Domain::Numeric::narrowOr<int>(*val, Domain::Sampling::REFRESH_INTERVAL_DEFAULT_MS));
        }

        if (auto val = config["sampling"]["history_max_seconds"].value<std::int64_t>())
        {
            // Clamp function will handle out-of-range values; use default if narrowOr fails
            m_Settings.maxHistorySeconds =
                Domain::Sampling::clampHistorySeconds(Domain::Numeric::narrowOr<int>(*val, Domain::Sampling::HISTORY_SECONDS_DEFAULT));
        }
        // When the key is missing we intentionally keep the default (300s) set in UserSettings.

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

        // Panel visibility
        if (auto val = config["panels"]["processes"].value<bool>())
        {
            m_Settings.showProcesses = *val;
        }
        if (auto val = config["panels"]["metrics"].value<bool>())
        {
            m_Settings.showMetrics = *val;
        }
        if (auto val = config["panels"]["details"].value<bool>())
        {
            m_Settings.showDetails = *val;
        }

        // Window state
        if (auto val = config["window"]["width"].value<std::int64_t>())
        {
            // Use default width of 800 if narrowOr fails; clamp will further constrain to valid range
            const int width = Domain::Numeric::narrowOr<int>(*val, 800);
            m_Settings.windowWidth = std::clamp(width, 200, 16'384);
        }
        if (auto val = config["window"]["height"].value<std::int64_t>())
        {
            // Use default height of 600 if narrowOr fails; clamp will further constrain to valid range
            const int height = Domain::Numeric::narrowOr<int>(*val, 600);
            m_Settings.windowHeight = std::clamp(height, 200, 16'384);
        }
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
    std::filesystem::path configDir = m_ConfigPath.parent_path();
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
         }},
        {"theme", toml::table{{"id", m_Settings.themeId}}},
        {"font", toml::table{{"size", fontSizeStr}}},
        {"panels",
         toml::table{
             {"processes", m_Settings.showProcesses},
             {"metrics", m_Settings.showMetrics},
             {"details", m_Settings.showDetails},
         }},
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
    file << "# This file is auto-generated. Manual edits are preserved.\n\n";
    file << config;

    spdlog::info("Saved config to {}", m_ConfigPath.string());
}

void UserConfig::applyToApplication()
{
    auto& theme = UI::Theme::get();

    // Apply theme
    theme.setThemeById(m_Settings.themeId);

    // Apply font size
    theme.setFontSize(m_Settings.fontSize);

    spdlog::debug("Applied user config: theme={}, fontSize={}",
                  m_Settings.themeId,
                  // Use 0 as fallback if enum value is out of int range
                  Domain::Numeric::narrowOr<int>(std::to_underlying(m_Settings.fontSize), 0));
}

void UserConfig::captureFromApplication()
{
    auto& theme = UI::Theme::get();

    // Capture current theme
    m_Settings.themeId = theme.currentThemeId();

    // Capture font size
    m_Settings.fontSize = theme.currentFontSize();

    spdlog::debug("Captured app state: theme={}, fontSize={}",
                  m_Settings.themeId,
                  // Use 0 as fallback if enum value is out of int range
                  Domain::Numeric::narrowOr<int>(std::to_underlying(m_Settings.fontSize), 0));
}

} // namespace App
