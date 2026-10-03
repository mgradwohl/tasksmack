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

    // Process table column widths, order and sort, as ImGui's own "[Table]" settings text (#952).
    // Opaque to TaskSmack: it is filtered by ProcessTableSettings::sanitize() and handed to ImGui.
    std::string processTableLayout;

    // Sampling / refresh interval (milliseconds)
    // Applied to all background samplers (process + system) for consistent cadence.
    int refreshIntervalMs = Domain::Sampling::REFRESH_INTERVAL_DEFAULT_MS;

    // Maximum duration of in-memory history buffers (seconds)
    // Controls how much timeline data is retained and shown in plots.
    int maxHistorySeconds = Domain::Sampling::HISTORY_SECONDS_DEFAULT;

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

    // Anti-aliased line/fill rendering for history chart plots. On by default (preserves the
    // existing look); disabling it trades chart-edge smoothness for lower CPU/GPU cost, which
    // profiling showed as a real, non-trivial share of both idle and interactive frame time
    // (Dear ImGui's AddPolyline/PathArcToFastEx -- perf-plan #843 phase 1). An escape hatch for
    // lower-power/integrated GPUs rather than a default-off change, since the app's charts
    // should stay just as beautiful as today unless the user opts into the tradeoff.
    bool chartAntiAliasing = true;

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

    // Opt-in escape hatch (default off -- the custom title bar is the default
    // everywhere): use native OS/compositor window decorations instead of the custom
    // borderless title bar. Only has an effect on native Wayland; read once at startup
    // (Core::Application's constructor), so it takes effect on next launch. See #745.
    bool forceNativeWindowDecorationsOnWayland = false;
};

/// Merges settings when config.toml was edited outside TaskSmack (for example through Settings'
/// "Edit Config File") since TaskSmack last read or wrote it (#1122). `baseline` is what TaskSmack
/// last read or wrote, `mine` is what it holds now, and `external` is the edited file read over
/// `baseline`. A field TaskSmack changed since the baseline keeps its value; every other field
/// takes the edited file's, so neither side's changes are lost. Keep in step with UserSettings.
[[nodiscard]] inline UserSettings mergeSettings(const UserSettings& external, const UserSettings& baseline, const UserSettings& mine)
{
    UserSettings merged = external;
    const auto keepMine = [&](auto UserSettings::* field)
    {
        if (mine.*field != baseline.*field)
        {
            merged.*field = mine.*field;
        }
    };
    keepMine(&UserSettings::themeId);
    keepMine(&UserSettings::fontSize);
    keepMine(&UserSettings::processColumns);
    keepMine(&UserSettings::processTableLayout);
    keepMine(&UserSettings::refreshIntervalMs);
    keepMine(&UserSettings::maxHistorySeconds);
    keepMine(&UserSettings::socketStatsCacheTtlMs);
    keepMine(&UserSettings::minTimeForRateSeconds);
    keepMine(&UserSettings::maxSaneRateBps);
    keepMine(&UserSettings::integratedGpuVramThresholdBytes);
    keepMine(&UserSettings::chartSmoothFactor);
    keepMine(&UserSettings::chartTauMsMin);
    keepMine(&UserSettings::chartTauMsMax);
    keepMine(&UserSettings::chartAntiAliasing);
    keepMine(&UserSettings::progressColorLowThreshold);
    keepMine(&UserSettings::progressColorHighThreshold);
    keepMine(&UserSettings::windowWidth);
    keepMine(&UserSettings::windowHeight);
    keepMine(&UserSettings::windowPosX);
    keepMine(&UserSettings::windowPosY);
    keepMine(&UserSettings::windowMaximized);
    keepMine(&UserSettings::showPrivilegeNotice);
    keepMine(&UserSettings::forceNativeWindowDecorationsOnWayland);
    return merged;
}

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

    /// Save settings to config file, atomically (a temporary file renamed over it), keeping keys
    /// TaskSmack doesn't own. If the file was edited outside TaskSmack since it was last read or
    /// written, those edits are merged in first (see mergeSettings).
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

    /// Override the config file path and reset all in-memory settings to defaults.
    /// This is a destructive operation intended only for test fixtures: it clears
    /// m_Settings so that each test starts from a clean state without inheriting
    /// values from a previous test or the real platform config.
    /// Application code should never call this method.
    void resetConfigPathForTesting(const std::filesystem::path& path)
    {
        m_ConfigPath = path;
        m_Settings = UserSettings{};
        m_Baseline = UserSettings{};
        m_SyncedWriteTime.reset();
        m_IsLoaded = false;
    }

  private:
    UserConfig();
    ~UserConfig() = default;

    std::filesystem::path m_ConfigPath;
    UserSettings m_Settings;
    bool m_IsLoaded = false;

    // What TaskSmack last read from or wrote to the file, and the file's modification time then:
    // a different time at save means it was edited outside TaskSmack (#1122).
    UserSettings m_Baseline;
    std::optional<std::filesystem::file_time_type> m_SyncedWriteTime;

    void markSynced();

    static auto getConfigDirectory() -> std::filesystem::path;
};

} // namespace App
