#pragma once

// A startup screenshot (#2827): write a PNG of the rendered window, or a numbered sequence of them,
// and quit -- with no input, so documentation screenshots and showcases come from a script.
//
//   TASKSMACK_SCREENSHOT=shot.png ./TaskSmack                       shot.png, 3 s after startup
//   TASKSMACK_SCREENSHOT='shot.png;after=5' ./TaskSmack             5 s after startup
//   TASKSMACK_SCREENSHOT='run.png;frames=30;fps=10' ./TaskSmack     run-0001.png ... run-0030.png, one every 0.1 s
//
// The value is read once, at startup (main() calls active()). The path is required and relative to
// the working directory; `after` (default DEFAULT_AFTER_SECONDS, at most MAX_AFTER_SECONDS) is how
// many seconds to wait before the first capture, `frames` (default 1, at most MAX_FRAMES) how many
// captures to make and `fps` (default DEFAULT_FPS, 1 to MAX_FPS) how many per second. One frame is
// written at the given path; a sequence as <stem>-NNNN.png beside it (frameFileName()). Unset or
// blank turns it off; a value that doesn't parse is ignored with a warning, and a number outside its
// range is clamped with one. ScreenshotLayer does the waiting, the capture and the quit. The parser
// here is pure and header-only (tests/App/test_ScreenshotOverride.cpp); only active() reads the
// environment and logs.

#include "Core/EnvUtils.h"
#include "Core/Utf8Path.h"
#include "Platform/AsciiTrim.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <filesystem>
#include <format>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace App::ScreenshotOverride
{

inline constexpr std::string_view ENV_VAR = "TASKSMACK_SCREENSHOT";
static_assert(Core::isKnownEnvHook(ENV_VAR)); // listed in KNOWN_ENV_HOOKS (#2170)

/// Seconds after startup the first capture waits, unless `after` says otherwise.
inline constexpr int DEFAULT_AFTER_SECONDS = 3;
inline constexpr int MAX_AFTER_SECONDS = 60;
/// The most captures one run makes (`frames`).
inline constexpr int MAX_FRAMES = 600;
/// Captures per second in a sequence, unless `fps` says otherwise.
inline constexpr int DEFAULT_FPS = 10;
inline constexpr int MAX_FPS = 60;

/// What TASKSMACK_SCREENSHOT asks for.
struct Request
{
    std::filesystem::path path; ///< As given: relative to the working directory, UTF-8 decoded
    int afterSeconds = DEFAULT_AFTER_SECONDS;
    int frames = 1;
    int fps = DEFAULT_FPS;

    [[nodiscard]] bool operator==(const Request&) const = default;
};

struct ParseResult
{
    std::optional<Request> request; ///< nullopt: no screenshot (unset, blank, or invalid)
    std::string warning;            ///< why the value was ignored or a number clamped; empty if neither
};

namespace Detail
{

/// A whole number of digits only (no sign, no spaces), or nullopt. A value past int saturates at
/// int's maximum, so the caller clamps it like any other number that is too large.
[[nodiscard]] inline std::optional<int> parseWholeNumber(const std::string_view text)
{
    if (text.empty() || !std::ranges::all_of(text, [](const char c) { return c >= '0' && c <= '9'; }))
    {
        return std::nullopt;
    }
    int value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error == std::errc::result_out_of_range)
    {
        return std::numeric_limits<int>::max();
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

/// Parses a TASKSMACK_SCREENSHOT value: "<file.png>[;after=<seconds>][;frames=<N>][;fps=<F>]", the
/// options in any order and case, spaces around each part allowed. nullptr, empty and blank give no
/// request and no warning. A missing path, an option without '=' or with a non-number, an unknown
/// or repeated option gives no request and a warning. A number outside its range (after 0 to
/// MAX_AFTER_SECONDS, frames 1 to MAX_FRAMES, fps 1 to MAX_FPS) is clamped, with a warning.
[[nodiscard]] inline ParseResult parse(const char* value)
{
    ParseResult result;
    const std::string_view text = Platform::trimAscii(value != nullptr ? value : "");
    if (text.empty())
    {
        return result;
    }

    const auto invalid = [&result, text](const std::string& why)
    {
        result.warning = std::format("'{}' {}; ignored (expected <file.png>[;after=<seconds>][;frames=<N>][;fps=<F>])", text, why);
        return result;
    };

    // The path, then options as key=value, separated by ';'.
    std::size_t end = text.find(';');
    const std::string_view pathText = Platform::trimAscii(text.substr(0, end));
    if (pathText.empty())
    {
        return invalid("names no file");
    }
    std::optional<int> after;
    std::optional<int> frames;
    std::optional<int> fps;
    while (end != std::string_view::npos)
    {
        const std::size_t start = end + 1;
        end = text.find(';', start);
        const std::string_view option = Platform::trimAscii(text.substr(start, end - start));
        const std::size_t equals = option.find('=');
        if (equals == std::string_view::npos)
        {
            return invalid(std::format("has an option '{}' that isn't key=value", option));
        }
        const std::string_view key = Platform::trimAscii(option.substr(0, equals));
        const std::optional<int> number = Detail::parseWholeNumber(Platform::trimAscii(option.substr(equals + 1)));
        if (!number.has_value())
        {
            return invalid(std::format("gives '{}' a value that isn't a whole number", key));
        }
        std::optional<int>* slot = nullptr;
        if (Detail::equalsIgnoreCase(key, "after"))
        {
            slot = &after;
        }
        else if (Detail::equalsIgnoreCase(key, "frames"))
        {
            slot = &frames;
        }
        else if (Detail::equalsIgnoreCase(key, "fps"))
        {
            slot = &fps;
        }
        if (slot == nullptr)
        {
            return invalid(std::format("has an unknown option '{}'", key));
        }
        if (slot->has_value())
        {
            return invalid(std::format("sets '{}' twice", key));
        }
        *slot = number;
    }

    // Each number into its range, saying so; a missing one is its default.
    std::string clamped;
    const auto clamp =
        [&clamped](const std::string_view key, const std::optional<int> given, const int low, const int high, const int fallback)
    {
        if (!given.has_value())
        {
            return fallback;
        }
        const int value = std::clamp(*given, low, high);
        if (value != *given)
        {
            clamped += std::format("{}{} clamped to {} ({}-{})", clamped.empty() ? "" : ", ", key, value, low, high);
        }
        return value;
    };
    Request request{.path = Core::utf8ToPath(pathText)};
    request.afterSeconds = clamp("after", after, 0, MAX_AFTER_SECONDS, DEFAULT_AFTER_SECONDS);
    request.frames = clamp("frames", frames, 1, MAX_FRAMES, 1);
    request.fps = clamp("fps", fps, 1, MAX_FPS, DEFAULT_FPS);
    if (!clamped.empty())
    {
        result.warning = std::format("'{}': {}", text, clamped);
    }
    result.request = std::move(request);
    return result;
}

/// The file frame @p index (from 1) of a sequence is written to: <stem>-NNNN.png beside @p path,
/// the index zero-padded to four digits ("shots/run.png", 7 -> "shots/run-0007.png").
[[nodiscard]] inline std::filesystem::path frameFileName(const std::filesystem::path& path, const int index)
{
    std::filesystem::path name = path.stem();
    name += std::format("-{:04}.png", index); // ASCII, so exact whatever the narrow encoding
    return path.parent_path() / name;
}

/// How a sequence's files beside @p path are named, for a log line: "shots/run-NNNN.png".
[[nodiscard]] inline std::string sequencePattern(const std::filesystem::path& path)
{
    std::filesystem::path name = path.stem();
    name += "-NNNN.png";
    return Core::pathToUtf8(path.parent_path() / name);
}

/// "shot.png after 3 s" / "30 frames at 10 fps after 3 s -> run-NNNN.png"
[[nodiscard]] inline std::string describe(const Request& request)
{
    if (request.frames == 1)
    {
        return std::format("{} after {} s", Core::pathToUtf8(request.path), request.afterSeconds);
    }
    return std::format(
        "{} frames at {} fps after {} s -> {}", request.frames, request.fps, request.afterSeconds, sequencePattern(request.path));
}

/// The request TASKSMACK_SCREENSHOT makes, read and logged on the first call (main() makes it at
/// startup); nullopt when it is unset or invalid. Thread-safe.
[[nodiscard]] const std::optional<Request>& active();

} // namespace App::ScreenshotOverride
