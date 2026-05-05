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

    // Process table column visibility
    ProcessColumnSettings processColumns;

    // Sampling / refresh interval (milliseconds)
    // Applied to all background samplers (process + system) for consistent cadence.
    int refreshIntervalMs = Domain::Sampling::REFRESH_INTERVAL_DEFAULT_MS;

    // Maximum duration of in-memory history buffers (seconds)
    // Controls how much timeline data is retained and shown in plots.
    int maxHistorySeconds = Domain::Sampling::HISTORY_SECONDS_DEFAULT;

    // PDH instance refresh interval (seconds) - Windows only
    // Controls how often PDH refreshes GPU process instance names.
    // Lower values detect new GPU-using processes faster but use more CPU.
    int pdhInstanceRefreshSeconds = Domain::Sampling::PDH_INSTANCE_REFRESH_SECONDS_DEFAULT;

    // Socket stats cache TTL (milliseconds) - Linux only
    // Controls how long per-process network stats are cached.
    // Lower values = fresher data but more kernel queries.
    int socketStatsCacheTtlMs = Domain::Sampling::SOCKET_STATS_CACHE_TTL_MS_DEFAULT;

    // Metrics Calculation Parameters
    // Minimum time elapsed before computing network rates (seconds)
    // Prevents large rate spikes early in process lifetime when few deltas exist.
    double minTimeForRateSeconds = Domain::Sampling::MIN_TIME_FOR_RATE_SECONDS_DEFAULT;

    // Maximum sanity check for network/IO rates (bytes per second)
    // Rates above this threshold are treated as errors and clamped to 0.
    double maxSaneRateBps = Domain::Sampling::MAX_SANE_RATE_BPS_DEFAULT;

    // GPU integrated VRAM threshold (bytes) - Windows only
    // Used to classify GPUs as integrated vs. discrete based on dedicated VRAM.
    int64_t integratedGpuVramThresholdBytes = Domain::Sampling::INTEGRATED_GPU_VRAM_THRESHOLD_BYTES_DEFAULT;

    // UI Behavior Parameters
    // Exponential smoothing factor for charts (0.0 = no smoothing, 1.0 = full averaging)
    double chartSmoothFactor = Domain::Sampling::CHART_SMOOTH_FACTOR_DEFAULT;

    // Adaptive time constant range for chart smoothing (milliseconds)
    int chartTauMsMin = Domain::Sampling::CHART_TAU_MS_MIN_DEFAULT;
    int chartTauMsMax = Domain::Sampling::CHART_TAU_MS_MAX_DEFAULT;

    // Progress bar color thresholds (percentage, 0-100)
    double progressColorLowThreshold = Domain::Sampling::PROGRESS_COLOR_LOW_THRESHOLD_DEFAULT;
    double progressColorHighThreshold = Domain::Sampling::PROGRESS_COLOR_HIGH_THRESHOLD_DEFAULT;

    // Window state
    int windowWidth = 1280;
    int windowHeight = 720;
    std::optional<int> windowPosX;
    std::optional<int> windowPosY;
    bool windowMaximized = false;

    // Whether to show the reduced-privileges notice dialog on startup.
    // Set to false permanently via "Don't show again" in the dialog.
    bool showPrivilegeNotice = true;
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

    /// Save settings to config file.
    /// Resets the loaded flag so a subsequent load() call will re-read from disk.
    void save();

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
    void applyToApplication() const;

    /// Capture current application state into settings
    void captureFromApplication();

    /// Get the config file path
    [[nodiscard]] auto configPath() const -> const std::filesystem::path&
    {
        return m_ConfigPath;
    }

    /// Override the config file path.
    /// Resets the loaded flag so the next load() call reads from the new path.
    /// Useful for isolated testing (each test can point to its own temp file).
    void setConfigPath(const std::filesystem::path& path)
    {
        m_ConfigPath = path;
        m_Settings = UserSettings{};
        m_IsLoaded = false;
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
