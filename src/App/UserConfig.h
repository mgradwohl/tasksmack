#pragma once

#include "App/ProcessColumnConfig.h"
#include "Domain/SamplingConfig.h"
#include "UI/Theme.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace App
{

/// The format of the config file TaskSmack writes, its top-level `config_version` key (#1376).
///  - 1 (no key): [process_columns] lists every column, whether or not the user chose it.
///  - 2: [process_columns] lists only the columns whose visibility the user chose; every other column
///    follows the defaults, including the ones that depend on what this system can fill (#1210).
/// A version 1 file is migrated when it is read (ProcessColumnSettings::isLegacyChoice()) and
/// rewritten in the new format on the next save.
inline constexpr std::int64_t CONFIG_FORMAT_VERSION = 2;

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
    // Network rate ceiling (bytes per second), [metrics] max_sane_rate_bps. A per-process or
    // per-interface rate above it is taken for a bad reading and shown as 0 (an interface's is also
    // a gap in its chart). Config-file only; applied to the ProcessModel when the Processes panel
    // attaches (#1123) and to the SystemModel when the System panel attaches (#1291).
    double maxSaneRateBps = Domain::Sampling::MAX_SANE_RATE_BPS_DEFAULT;

    // UI Behavior Parameters
    // How live values and the "now" bars beside charts ease toward each new sample
    // (UI::Widgets::computeAlpha): the time constant is chartSmoothFactor x the refresh interval,
    // kept within [chartTauMsMin, chartTauMsMax] ms. Config-file only; pushed into UI by
    // applyToApplication() at startup (#1123).
    double chartSmoothFactor = Domain::Sampling::CHART_SMOOTH_FACTOR_DEFAULT;
    int chartTauMsMin = Domain::Sampling::CHART_TAU_MS_MIN_DEFAULT;
    int chartTauMsMax = Domain::Sampling::CHART_TAU_MS_MAX_DEFAULT;

    // Anti-aliased line/fill rendering for history chart plots. On by default (preserves the
    // existing look); disabling it trades chart-edge smoothness for lower CPU/GPU cost, which
    // profiling showed as a real, non-trivial share of both idle and interactive frame time
    // (Dear ImGui's AddPolyline/PathArcToFastEx -- perf-plan #843 phase 1). An escape hatch for
    // lower-power/integrated GPUs rather than a default-off change, since the app's charts
    // should stay just as beautiful as today unless the user opts into the tradeoff.
    bool chartAntiAliasing = true;

    // Window state
    int windowWidth = 1280;
    int windowHeight = 720;
    // Window scale (Window::getUnitScale()) windowWidth/windowHeight are measured at: on Windows the
    // size is in physical pixels, so it is converted to the scale of the display the window opens
    // on (#1168). The default size is for 100 %; a config saved before the scale was recorded has
    // none, and its size is restored unconverted, as it always was.
    std::optional<float> windowScale = 1.0F;
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

    /// Parses `tomlText` as a config file and reads every setting it holds into `settings`, the same
    /// way load() reads the file (keys that are absent leave `settings` alone). Returns false, with
    /// `settings` untouched, if the text is not valid TOML. No file I/O; the seam the config fuzz
    /// target drives (tests/fuzz/fuzz_user_config.cpp).
    [[nodiscard]] static auto parseSettings(std::string_view tomlText, UserSettings& settings) -> bool;

    /// Save settings to the config file by replacing it with a new file, so a crash mid-write can't
    /// leave it truncated. Only the settings TaskSmack changed since it last read or wrote the file
    /// are written (UserConfigHelpers::mergeOwnedKeys): keys it doesn't own, and edits made to the
    /// file while TaskSmack runs, are kept. A file that exists but can't be read or parsed is left
    /// alone and nothing is saved. The read-merge-rename isn't locked against another TaskSmack:
    /// main() lets only one run per config directory (InstanceLock, #1230).
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

    /// Apply loaded settings to the application: theme, font size, chart anti-aliasing and smoothing
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
        m_Synced = UserSettings{};
        m_IsLoaded = false;
        m_TempNameSource = nullptr;
    }

    /// Replaces the random number that names save()'s staging file, so a test can make every
    /// candidate name collide. Reset by resetConfigPathForTesting().
    void setTempNameSourceForTesting(std::function<std::uint32_t()> source)
    {
        m_TempNameSource = std::move(source);
    }

  private:
    UserConfig();
    ~UserConfig() = default;

    std::filesystem::path m_ConfigPath;
    UserSettings m_Settings;
    bool m_IsLoaded = false;

    // The settings as TaskSmack last read them from, or wrote them to, the file (the merge base for
    // save(), #1122). With no readable file at startup it is the settings TaskSmack started with,
    // so a file created or repaired before the first save only gets what TaskSmack changed.
    UserSettings m_Synced;
    std::function<std::uint32_t()> m_TempNameSource; // Testing only; empty means std::random_device

    static auto getConfigDirectory() -> std::filesystem::path;
};

} // namespace App
