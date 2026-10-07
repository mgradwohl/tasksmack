#pragma once

// A measurement-only window geometry (#1453): open TaskSmack at a fixed size, maximized or not,
// instead of the geometry saved in the config file, and leave that saved geometry alone on exit.
//
//   TASKSMACK_WINDOW=1600x900 ./TaskSmack            1600x900, not maximized
//   TASKSMACK_WINDOW=1600x900,maximized ./TaskSmack  maximized, restoring to 1600x900
//
// tools/measure-idle.sh --window sets it, so an idle-CPU figure no longer depends on the size the
// measuring machine last saved. Width and height are window units, clamped to
// [Core::WINDOW_MIN_DIMENSION, Core::WINDOW_MAX_DIMENSION] and then to the display, like a saved
// size. The variable is read once, at startup (main() calls active()). Unset, empty, or one of the
// "off" flag words ("0", "off", "false", "no") turn it off; anything else that doesn't parse is
// ignored with a warning. While it is on, the window's [window] keys are not written on exit
// (captureWindowGeometry()); every other setting still saves as usual.
//
// The parser and the capture decision are pure and header-only so they are unit-tested without a
// window (tests/App/test_WindowOverride.cpp); only active() reads the environment and logs.

#include "App/UserConfig.h"
#include "Core/EnvUtils.h"
#include "Core/WindowConstants.h"
#include "Core/WindowGeometry.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace App::WindowOverride
{

inline constexpr std::string_view ENV_VAR = "TASKSMACK_WINDOW";

/// The geometry TASKSMACK_WINDOW asks for.
struct Geometry
{
    int width = 0;
    int height = 0;
    bool maximized = false;

    [[nodiscard]] constexpr bool operator==(const Geometry&) const = default;
};

struct ParseResult
{
    std::optional<Geometry> geometry; ///< nullopt: no override (unset, off, or invalid)
    std::string warning;              ///< why the value was ignored or clamped; empty if it wasn't
};

namespace Detail
{

[[nodiscard]] inline std::string_view trim(std::string_view text) noexcept
{
    const auto isSpace = [](const char c)
    {
        return std::isspace(static_cast<unsigned char>(c)) != 0;
    };
    while (!text.empty() && isSpace(text.front()))
    {
        text.remove_prefix(1);
    }
    while (!text.empty() && isSpace(text.back()))
    {
        text.remove_suffix(1);
    }
    return text;
}

/// A whole number of digits only (no sign, no spaces), or nullopt. Values past int saturate at
/// WINDOW_MAX_DIMENSION + 1 so the caller clamps them like any other size that is too large.
[[nodiscard]] inline std::optional<int> parseDimension(const std::string_view text) noexcept
{
    if (text.empty() || !std::ranges::all_of(text, [](const char c) { return c >= '0' && c <= '9'; }))
    {
        return std::nullopt;
    }
    int value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error == std::errc::result_out_of_range)
    {
        return Core::WINDOW_MAX_DIMENSION + 1;
    }
    if (error != std::errc{} || end != text.data() + text.size())
    {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] inline bool equalsIgnoreCase(const std::string_view a, const std::string_view lowercase) noexcept
{
    return a.size() == lowercase.size() &&
           std::ranges::equal(a, lowercase, [](const char x, const char y) { return std::tolower(static_cast<unsigned char>(x)) == y; });
}

} // namespace Detail

/// Parses a TASKSMACK_WINDOW value: "<width>x<height>" or "<width>x<height>,maximized" ('x' or
/// 'X'; spaces around the parts are allowed). nullptr, empty and the "off" flag words give no
/// override and no warning; any other value that doesn't parse gives no override and a warning.
/// A width or height outside [WINDOW_MIN_DIMENSION, WINDOW_MAX_DIMENSION] is clamped, with a warning.
[[nodiscard]] inline ParseResult parse(const char* value)
{
    ParseResult result;
    if (!Core::isEnvFlagEnabled(value))
    {
        return result;
    }
    const std::string_view text = Detail::trim(value);
    if (text.empty() || !Core::isEnvFlagEnabled(std::string(text).c_str()))
    {
        return result;
    }

    const auto invalid = [&result, text]
    {
        result.warning = std::format("'{}' is not <width>x<height>[,maximized] (e.g. 1600x900); ignored", text);
        return result;
    };

    // The size, then at most one flag after a comma.
    const std::size_t comma = text.find(',');
    const std::string_view size = Detail::trim(text.substr(0, comma));
    bool maximized = false;
    if (comma != std::string_view::npos)
    {
        const std::string_view flag = Detail::trim(text.substr(comma + 1));
        if (!Detail::equalsIgnoreCase(flag, "maximized") && !Detail::equalsIgnoreCase(flag, "maximised"))
        {
            return invalid();
        }
        maximized = true;
    }

    const std::size_t separator = size.find_first_of("xX");
    if (separator == std::string_view::npos)
    {
        return invalid();
    }
    const auto width = Detail::parseDimension(Detail::trim(size.substr(0, separator)));
    const auto height = Detail::parseDimension(Detail::trim(size.substr(separator + 1)));
    if (!width || !height)
    {
        return invalid();
    }

    const Geometry geometry{
        .width = std::clamp(*width, Core::WINDOW_MIN_DIMENSION, Core::WINDOW_MAX_DIMENSION),
        .height = std::clamp(*height, Core::WINDOW_MIN_DIMENSION, Core::WINDOW_MAX_DIMENSION),
        .maximized = maximized,
    };
    if (geometry.width != *width || geometry.height != *height)
    {
        result.warning = std::format("'{}': size clamped to {}x{} (each side {}-{})",
                                     text,
                                     geometry.width,
                                     geometry.height,
                                     Core::WINDOW_MIN_DIMENSION,
                                     Core::WINDOW_MAX_DIMENSION);
    }
    result.geometry = geometry;
    return result;
}

/// "1600x900, not maximized" / "1600x900, maximized"
[[nodiscard]] inline std::string describe(const Geometry& geometry)
{
    return std::format("{}x{}, {}", geometry.width, geometry.height, geometry.maximized ? "maximized" : "not maximized");
}

/// Whether the window's geometry is saved on exit: never while an override is active, so a
/// measurement run leaves the user's saved geometry exactly as it was.
[[nodiscard]] constexpr bool shouldSaveWindowGeometry(const std::optional<Geometry>& windowOverride) noexcept
{
    return !windowOverride.has_value();
}

/// The live window state captured on exit (Core::Window's normal geometry, its scale, and whether
/// the window is maximized), as plain values so the capture decision needs no window.
struct CapturedWindow
{
    /// The normal (restored) rectangle; nullopt when it is unknown (maximized by the OS/compositor).
    std::optional<Core::WindowGeometry::Rect> normal;
    /// The scale @ref normal was measured at; 0 when unknown.
    float normalScale = 0.0F;
    /// Whether the platform can position windows (Core::Window::supportsPositioning()).
    bool canPosition = false;
    bool maximized = false;
};

/// Writes the captured window geometry into @p settings for UserConfig::save(), or, while
/// @p windowOverride is set, leaves every window geometry setting as it was loaded. UserConfig::save()
/// writes only the settings that changed (#1122), so the file's [window] keys then stay unchanged.
inline void captureWindowGeometry(UserSettings& settings, const CapturedWindow& captured, const std::optional<Geometry>& windowOverride)
{
    if (!shouldSaveWindowGeometry(windowOverride))
    {
        return;
    }
    // While maximized the live size and position are the maximized ones; the normal rectangle is
    // what Restore returns to, so that is what is saved (#1121). Unknown, the saved one is kept.
    if (captured.normal.has_value())
    {
        settings.windowWidth = captured.normal->width;
        settings.windowHeight = captured.normal->height;
        // With the scale it was measured at, so the next launch can convert it to the display it
        // opens on (#1168). An unknown scale saves none: the size is then restored as saved.
        settings.windowScale = captured.normalScale > 0.0F ? std::optional<float>{captured.normalScale} : std::nullopt;
        if (captured.canPosition)
        {
            settings.windowPosX = captured.normal->x;
            settings.windowPosY = captured.normal->y;
        }
    }
    settings.windowMaximized = captured.maximized;
}

/// The override TASKSMACK_WINDOW selects, read and logged on the first call (main() makes it at
/// startup, before the window is created); nullopt when it is off or invalid. Thread-safe.
[[nodiscard]] const std::optional<Geometry>& active();

} // namespace App::WindowOverride
