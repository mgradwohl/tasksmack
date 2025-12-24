#pragma once

#include "App/ProcessColumnConfig.h"
#include "Domain/SamplingConfig.h"
#include "UI/Theme.h"

#include <filesystem>
#include <optional>
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

    // Process table column visibility
    ProcessColumnSettings processColumns;

    // Sampling / refresh interval (milliseconds)
    // Applied to all background samplers (process + system) for consistent cadence.
    int refreshIntervalMs = Domain::Sampling::REFRESH_INTERVAL_DEFAULT_MS;

    // Maximum duration of in-memory history buffers (seconds)
    // Controls how much timeline data is retained and shown in plots.
    int maxHistorySeconds = Domain::Sampling::HISTORY_SECONDS_DEFAULT;

    // Window state (optional, for future use)
    int windowWidth = 1280;
    int windowHeight = 720;
    std::optional<int> windowPosX;
    std::optional<int> windowPosY;
    bool windowMaximized = false;

    // ImGui layout state (window positions, docking layout, etc.)
    // Stored as INI-format string from ImGui::SaveIniSettingsToMemory()
    std::string imguiLayout;
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

    UserConfig(const UserConfig&) = delete;
    auto operator=(const UserConfig&) -> UserConfig& = delete;
    UserConfig(UserConfig&&) = delete;
    auto operator=(UserConfig&&) -> UserConfig& = delete;

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

    /// Apply ImGui layout state from settings
    void applyImGuiLayout() const;

    /// Capture current ImGui layout state into settings
    void captureImGuiLayout();

    /// Get the config file path
    [[nodiscard]] auto configPath() const -> const std::filesystem::path&
    {
        return m_ConfigPath;
    }

  private:
    UserConfig();
    ~UserConfig() = default;

    std::filesystem::path m_ConfigPath;
    UserSettings m_Settings;
    bool m_IsLoaded = false;

    static auto getConfigDirectory() -> std::filesystem::path;
};

} // namespace App
