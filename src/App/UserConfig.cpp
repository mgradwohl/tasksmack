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
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <limits>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

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

#include <fstream>
#include <ios>
// clang-format on
#else
#include <array>

#include <fcntl.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#if defined(__linux__) && __has_include(<sys/xattr.h>)
#include <sys/xattr.h>
#endif
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

#ifndef _WIN32
namespace
{

#if defined(__linux__) && __has_include(<sys/xattr.h>)
constexpr const char* POSIX_ACL_ACCESS_XATTR = "system.posix_acl_access";

/// The access ACL of @p path as its raw xattr, empty if it has none (or the filesystem has no ACLs),
/// or nullopt if it couldn't be read.
[[nodiscard]] std::optional<std::vector<char>> readAccessAcl(const std::filesystem::path& path)
{
    for (;;)
    {
        const auto size = ::getxattr(path.c_str(), POSIX_ACL_ACCESS_XATTR, nullptr, 0);
        if (size < 0)
        {
            if (errno == ENODATA || errno == ENOTSUP)
            {
                return std::vector<char>{};
            }
            return std::nullopt;
        }
        std::vector<char> acl(static_cast<std::size_t>(size));
        const auto read = ::getxattr(path.c_str(), POSIX_ACL_ACCESS_XATTR, acl.data(), acl.size());
        if (read >= 0)
        {
            acl.resize(static_cast<std::size_t>(read));
            return acl;
        }
        if (errno != ERANGE) // ERANGE: it grew in between; ask again
        {
            return std::nullopt;
        }
    }
}
#endif

/// Writes all of `contents` to `fd`, retrying short writes and EINTR, then sets @p owner (fchown),
/// @p accessAcl and @p mode (fchmod) where given, all on the same descriptor -- after @p accessAcl if set (a raw POSIX access
/// ACL to install, or empty to remove an inherited one), then closes it. False if any write, the fchmod
/// or the close failed; the descriptor is closed either way. fchmod on the descriptor, not chmod on
/// the path: the path could have been replaced by a symlink to another of the user's files, whose
/// mode chmod would then change (#1222 review).
/// The original config's owner, so the replacement keeps it: in a setgid directory the staging file
/// would otherwise take the directory's group, whose members could then read it (#1222 review).
struct FileOwner
{
    uid_t uid;
    gid_t gid;
};

[[nodiscard]] bool writeAllAndClose(int fd,
                                    std::string_view contents,
                                    std::optional<FileOwner> owner,
                                    std::optional<mode_t> mode,
                                    const std::optional<std::vector<char>>& accessAcl)
{
    bool ok = true;
    while (ok && !contents.empty())
    {
        const auto n = ::write(fd, contents.data(), contents.size());
        if (n < 0)
        {
            ok = (errno == EINTR);
            continue;
        }
        contents.remove_prefix(static_cast<std::size_t>(n));
    }
    // Owner first: changing it can clear mode bits, and the ACL's owning-group entry refers to it.
    if (ok && owner.has_value() && ::fchown(fd, owner->uid, owner->gid) != 0)
    {
        ok = false;
    }
#if defined(__linux__) && __has_include(<sys/xattr.h>)
    // The original's access ACL, or none if it had none -- a new file in a directory with a default
    // ACL inherits one, and with the original's mode restored its named grants could expose the
    // file to users who couldn't read the original (#1222 review). Before the mode, which then
    // leaves the ACL's mask matching the original's group bits.
    if (ok && accessAcl.has_value())
    {
        if (!accessAcl->empty())
        {
            ok = ::fsetxattr(fd, POSIX_ACL_ACCESS_XATTR, accessAcl->data(), accessAcl->size(), 0) == 0;
        }
        else if (::fremovexattr(fd, POSIX_ACL_ACCESS_XATTR) != 0)
        {
            ok = (errno == ENODATA || errno == ENOTSUP);
        }
    }
#else
    (void) accessAcl;
#endif
    if (ok && mode.has_value() && ::fchmod(fd, *mode) != 0)
    {
        ok = false;
    }
    if (::close(fd) != 0)
    {
        ok = false;
    }
    return ok;
}

} // namespace
#endif

void UserConfig::load()
{
    if (m_IsLoaded)
    {
        return;
    }
    m_IsLoaded = true;
    // The baseline save() merges against, until a file is read: the settings TaskSmack starts with.
    m_Synced = m_Settings;

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
        return;
    }

    try
    {
        const auto config = toml::parse_file(m_ConfigPath.string());
        readSettings(config, m_Settings);
        m_Synced = m_Settings;
        spdlog::info("Loaded config from {}", m_ConfigPath.string());
    }
    catch (const toml::parse_error& err)
    {
        spdlog::error("Failed to parse config file: {}", err.what());
    }
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

    // A symlinked config is written through the link: the temporary file goes beside the link's
    // target and is renamed over the target, so the link survives (renaming over the link itself
    // would replace it with a regular file and leave the target stale, #1222 review).
    // The chain is followed by hand rather than with canonical(), which needs the final target to
    // exist: a link to a file not created yet is still written through.
    std::filesystem::path destination = m_ConfigPath;
    constexpr int MAX_LINK_HOPS = 40; // the kernel's own limit before ELOOP
    int linkHops = 0;
    for (;;)
    {
        const std::filesystem::file_status linkStatus = std::filesystem::symlink_status(destination, ec);
        if (linkStatus.type() == std::filesystem::file_type::not_found)
        {
            ec.clear(); // nothing there yet: the file will be created
            break;
        }
        if (ec || !std::filesystem::is_symlink(linkStatus))
        {
            break;
        }
        if (++linkHops > MAX_LINK_HOPS)
        {
            spdlog::error("Not saving settings: {} is a loop of links", m_ConfigPath.string());
            return;
        }
        const std::filesystem::path target = std::filesystem::read_symlink(destination, ec);
        if (ec)
        {
            break;
        }
        destination = target.is_absolute() ? target : destination.parent_path() / target;
    }
    if (ec)
    {
        spdlog::error("Not saving settings: can't resolve the link {}: {}", m_ConfigPath.string(), ec.message());
        return;
    }

    // Start from the file as it is. One that exists but can't be read or parsed is left alone:
    // replacing it would lose every setting and unknown key in it, and a malformed file is the
    // user's to repair (#1122).
    toml::table document;
    std::filesystem::perms originalPermissions = std::filesystem::perms::unknown;
    const bool fileExists = std::filesystem::exists(destination, ec);
    if (ec)
    {
        spdlog::error("Not saving settings: can't check {}: {}", m_ConfigPath.string(), ec.message());
        return;
    }
    if (fileExists)
    {
        try
        {
            document = toml::parse_file(destination.string());
        }
        catch (const toml::parse_error& err)
        {
            spdlog::error("Not saving settings: {} can't be read or parsed ({}); fix or remove it", m_ConfigPath.string(), err.what());
            return;
        }
        // Fail closed: a file whose permissions can't be read must not be replaced by one with the
        // default mode, which could make a 0600 config world-readable.
        const std::filesystem::file_status status = std::filesystem::status(destination, ec);
        if (ec)
        {
            spdlog::error("Not saving settings: can't read the permissions of {}: {}", m_ConfigPath.string(), ec.message());
            return;
        }
        originalPermissions = status.permissions();
    }

    // Write only what TaskSmack changed since it last read or wrote the file; everything else in
    // the file, including edits made while TaskSmack runs, stays as it is (#1122).
    const toml::table mine = buildTable(m_Settings);
    // A new file gets every setting; an existing one -- even one created or repaired since startup --
    // gets only the keys whose value TaskSmack changed from its baseline.
    UserConfigHelpers::mergeOwnedKeys(document, fileExists ? buildTable(m_Synced) : toml::table{}, mine);

    // Write a temporary file beside it and rename it over the original, so a crash mid-write can't
    // leave the config truncated or empty (#1122). This does not make the new contents durable
    // across a power loss: nothing is synced to disk. Each save gets its own, exclusively created
    // temporary file, so two TaskSmack instances can't write into the same one.
    std::ostringstream text;
    text << "# TaskSmack user configuration\n";
    text << "# Written by TaskSmack. Keys it doesn't use are kept, but comments in this file are not.\n";
    text << "# Edits made while TaskSmack is running are kept unless TaskSmack changes the same setting.\n";
    text << "# Notes:\n";
    text << "#   [sampling] interval_ms: refresh cadence (100-5000ms); affects all samplers\n";
    text << "#   [sampling] history_max_seconds: timeline history window (10-1800s)\n";
    text << "#   [sampling] socket_stats_cache_ttl_ms: Linux only; per-process network stat cache TTL (0-5000ms)\n";
    text << "#   [metrics] min_time_for_rate_seconds: delay before computing network rates (0.0-5.0s); avoids early spikes\n";
    text << "#   [metrics] max_sane_rate_bps: sanity check for network/IO rates (bytes/sec); clamps outliers\n";
    text << "#   [metrics] integrated_gpu_vram_threshold_mb: GPU classification threshold (16-512MB)\n";
    text << "#   [ui] chart_smooth_factor: exponential smoothing for charts (0.0-0.95); 0=no smoothing, 0.95=max smoothing\n";
    text << "#   [ui] chart_tau_ms_min/max: adaptive smoothing time constant range (ms); affects chart responsiveness\n";
    text << "#   [ui] progress_color_low/high_threshold: color change percentages for progress bars\n";
    text << "#   [ui] show_privilege_notice: show startup dialog when running without elevated privileges (true/false)\n";
    text << "#   [ui] chart_anti_aliasing: smooth chart line/fill edges (true/false); disable for lower CPU/GPU cost "
            "on integrated GPUs\n";
    text << "#   [process_columns]: toggle columns on/off; true shows the column\n";
    text << "#   [process_table] layout: saved column widths, order and sort (written by TaskSmack; delete it to reset)\n";
    text << "#   Themes: built-in themes in assets/themes. Add custom .toml themes beside this config under a 'themes' folder.\n\n";
    text << document;
    // Stream exceptions are off, so a failed insertion (say, allocation) only sets badbit; its
    // output would be incomplete. Stop before staging anything over the intact config.
    if (!text)
    {
        spdlog::error("Not saving settings: couldn't serialise them");
        return;
    }
    const std::string contents = std::move(text).str();

    std::filesystem::path tempPath;
    // Removes the staging file on any way out of save() -- an early return or an exception -- but
    // only once this save has created it (a candidate name that already existed is someone else's),
    // and not after the rename has published it.
    class StagingFileGuard
    {
      public:
        StagingFileGuard() = default;
        StagingFileGuard(const StagingFileGuard&) = delete;
        StagingFileGuard& operator=(const StagingFileGuard&) = delete;
        StagingFileGuard(StagingFileGuard&&) = delete;
        StagingFileGuard& operator=(StagingFileGuard&&) = delete;
        ~StagingFileGuard()
        {
            if (!m_Path.empty())
            {
                std::error_code removeError;
                std::filesystem::remove(m_Path, removeError);
            }
        }
        void created(const std::filesystem::path& path)
        {
            m_Path = path;
        }
        void published() noexcept
        {
            m_Path.clear();
        }

      private:
        std::filesystem::path m_Path;
    };
    StagingFileGuard stagingGuard;
    std::random_device random;
    const auto nextTempPath = [&]
    {
        // A short name of its own in the same directory, not "<name>.<hex>.tmp": a target whose own
        // name is near the filesystem's 255-byte limit would leave no room for a suffix (#1222 review).
        return destination.parent_path() / std::format(".tasksmack-config.{:08x}.tmp", m_TempNameSource ? m_TempNameSource() : random());
    };
#ifndef _WIN32
    // Created exclusively (O_EXCL) and owner-only from the start, and written through that same
    // descriptor. Reopening the path would lose both: a substituted symlink could be followed and its
    // target truncated, and a umask without owner-write would make the reopen fail. Created with the
    // umask and narrowed afterwards, another user could open it in between and keep reading (#1222
    // review). The original's mode is restored only after writing, just before the rename.
    // The original's mode (a 0600 config stays 0600, a 0644 one stays 0644), applied only once the
    // file is complete, on the open descriptor.
    std::optional<FileOwner> owner;
    if (fileExists)
    {
        struct stat original = {};
        if (::stat(destination.c_str(), &original) != 0)
        {
            spdlog::error(
                "Not saving settings: can't read the owner of {}: {}", m_ConfigPath.string(), std::system_category().message(errno));
            return;
        }
        owner = FileOwner{.uid = original.st_uid, .gid = original.st_gid};
    }
    const std::optional<mode_t> mode = (originalPermissions != std::filesystem::perms::unknown)
                                         ? std::optional<mode_t>(static_cast<mode_t>(originalPermissions & std::filesystem::perms::mask))
                                         : std::nullopt;
    // nullopt: leave the new file's ACL as created (a brand-new config inherits the directory's
    // default ACL, as any new file would).
    std::optional<std::vector<char>> accessAcl;
#if defined(__linux__) && __has_include(<sys/xattr.h>)
    // Mode bits alone don't carry an extended ACL: a named-user grant with group::--- and
    // mask::r-- reports 0640, and that mode on a fresh file would let the whole group read it.
    // Copy the access ACL too, or don't replace the file (#1222 review). Read before the staging
    // file exists, so nothing between creating and writing it can fail or throw.
    if (fileExists)
    {
        auto acl = readAccessAcl(destination);
        if (!acl.has_value())
        {
            spdlog::error("Not saving settings: can't read the access control list of {}: {}",
                          m_ConfigPath.string(),
                          std::system_category().message(errno));
            return;
        }
        accessAcl = std::move(acl);
    }
#endif
    int fd = -1;
    for (int attempt = 0; attempt < 8 && fd < 0; ++attempt)
    {
        tempPath = nextTempPath();
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) - POSIX open() is variadic
        fd = ::open(tempPath.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, S_IRUSR | S_IWUSR);
        if (fd < 0 && errno != EEXIST)
        {
            break;
        }
    }
    if (fd < 0)
    {
        spdlog::error("Failed to create a temporary file beside {}: {}", m_ConfigPath.string(), std::system_category().message(errno));
        return;
    }
    stagingGuard.created(tempPath);
    const bool written = writeAllAndClose(fd, contents, owner, mode, accessAcl);
#else
    std::ofstream file;
    for (int attempt = 0; attempt < 8 && !file.is_open(); ++attempt)
    {
        tempPath = nextTempPath();
        if (std::filesystem::exists(tempPath, ec))
        {
            continue;
        }
#if defined(__cpp_lib_ios_noreplace)
        file.open(tempPath, std::ios::out | std::ios::noreplace);
#else
        file.open(tempPath, std::ios::out | std::ios::trunc);
#endif
    }
    if (!file.is_open())
    {
        spdlog::error("Failed to create a temporary file beside {}", m_ConfigPath.string());
        return;
    }
    stagingGuard.created(tempPath);
    file << contents;
    file.close();
    bool written = static_cast<bool>(file);
    if (written && originalPermissions != std::filesystem::perms::unknown)
    {
        // Windows' permission bits are only the read-only attribute; ACLs aren't copied.
        std::filesystem::permissions(tempPath, originalPermissions, ec);
        written = !ec;
    }
#endif
    if (!written)
    {
        spdlog::error("Not saving settings: couldn't write {} or give it the permissions of {}", tempPath.string(), m_ConfigPath.string());
        return;
    }

    std::filesystem::rename(tempPath, destination, ec);
    if (ec)
    {
        spdlog::error("Failed to replace {} with the new config: {}", m_ConfigPath.string(), ec.message());
        return;
    }

    stagingGuard.published();
    spdlog::info("Saved config to {}", m_ConfigPath.string());
    m_Synced = m_Settings;

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
