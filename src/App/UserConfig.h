#pragma once

#include "App/ProcessColumnConfig.h"
#include "UI/Theme.h"

#include <filesystem>
#include <string>

namespace App
{

/// User configuration settings that persist across sessions
struct UserSettings
{
    // Theme
    std::string themeId = "arctic-fire";

    // Font size
    UI::FontSize fontSize = UI::FontSize::Medium;

    // Panel visibility
    bool showProcesses = true;
    bool showMetrics = true;
    bool showDetails = true;

    // Process panel column visibility
    ProcessColumnSettings processColumns;

    // Window state (optional, for future use)
    int windowWidth = 1280;
    int windowHeight = 720;
    bool windowMaximized = false;
};

/**
 * @brief Manages user configuration persistence
 *
 * Saves/loads user preferences to a TOML file in the platform-appropriate
 * config directory:
 * - Linux: ~/.config/tasksmack/config.toml
 * - Windows: %APPDATA%/TaskSmack/config.toml
 */
class UserConfig
{
  public:
    /// Get the singleton instance
    static auto get() -> UserConfig&;

    /// Load settings from config file (call on startup)
    void load();

    /// Save settings to config file
    void save() const;

    /// Get current settings
    [[nodiscard]] auto settings() const -> const UserSettings&
    {
        return m_Settings;
    }

    /// Get mutable settings reference (for modification)
    [[nodiscard]] auto settings() -> UserSettings&
    {
        return m_Settings;
    }

    /// Apply loaded settings to the application (theme, font size, etc.)
    void applyToApplication();

    /// Capture current application state into settings
    void captureFromApplication();

    /// Get the config file path
    [[nodiscard]] auto configPath() const -> const std::filesystem::path&
    {
        return m_ConfigPath;
    }

  private:
    UserConfig();
    ~UserConfig() = default;

    UserConfig(const UserConfig&) = delete;
    auto operator=(const UserConfig&) -> UserConfig& = delete;
    UserConfig(UserConfig&&) = delete;
    auto operator=(UserConfig&&) -> UserConfig& = delete;

    std::filesystem::path m_ConfigPath;
    UserSettings m_Settings;

    static auto getConfigDirectory() -> std::filesystem::path;
};

} // namespace App
