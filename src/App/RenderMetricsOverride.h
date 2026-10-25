#pragma once

// The Render Metrics overlay without input (#2923): the per-chart vertex and CPU-time instrument is
// toggled with Ctrl+Shift+M and its CSV export goes to the clipboard, so an unattended run (a
// before/after measurement for #843, a CI artifact) could neither enable it nor read its numbers.
//
//   TASKSMACK_RENDER_METRICS=perf-data/idle.csv ./TaskSmack
//
// The value is read once, at startup (main() calls active()). It names the file the overlay's CSV --
// the same bytes its Copy CSV button puts on the clipboard, UI::RenderMetrics::toCsv() -- is written
// to at exit, where config.toml is saved (ShellLayer::onDetach), and while it is set the overlay is
// shown from the first frame, as the key chord shows it. A relative path is taken from the working
// directory; the directory must exist. Unset or blank turns it off. So a TASKSMACK_SCREENSHOT run
// with it set leaves a PNG and the metrics CSV behind.
//
// TASKSMACK_DEMO's metrics= action (DemoOverride.h) is the scripted form: metrics=on and metrics=off
// show and hide the overlay as the chord does, and metrics=export writes the CSV at that step, to this
// hook's file or, when it is unset, beside the screenshot's path (csvBesideScreenshot()).
//
// The parser here is pure and header-only (tests/App/test_RenderMetricsOverride.cpp); active() reads
// the environment and logs, and writeCsv() touches the disk (RenderMetricsOverride.cpp).

#include "Core/EnvUtils.h"
#include "Core/Utf8Path.h"
#include "Platform/AsciiCase.h"
#include "Platform/AsciiTrim.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace App::RenderMetricsOverride
{

inline constexpr std::string_view ENV_VAR = "TASKSMACK_RENDER_METRICS";
static_assert(Core::isKnownEnvHook(ENV_VAR)); // listed in KNOWN_ENV_HOOKS (#2170)

/// What a TASKSMACK_DEMO metrics= step asks for.
enum class MetricsRequest : std::uint8_t
{
    On,    ///< Show the overlay (and so start recording), as Ctrl+Shift+M does
    Off,   ///< Hide it, as the chord does again
    Export ///< Write the last completed frame's CSV to the hook's file
};

/// The request @p text names, "on", "off" or "export" in any case; nullopt for anything else.
[[nodiscard]] inline std::optional<MetricsRequest> parseMetricsRequest(const std::string_view text) noexcept
{
    using Platform::equalsIgnoreCase;
    if (equalsIgnoreCase(text, "on"))
    {
        return MetricsRequest::On;
    }
    if (equalsIgnoreCase(text, "off"))
    {
        return MetricsRequest::Off;
    }
    if (equalsIgnoreCase(text, "export"))
    {
        return MetricsRequest::Export;
    }
    return std::nullopt;
}

/// The name parseMetricsRequest() accepts for @p request, for a log line.
[[nodiscard]] constexpr std::string_view metricsRequestName(const MetricsRequest request) noexcept
{
    switch (request)
    {
    case MetricsRequest::On:
        return "on";
    case MetricsRequest::Off:
        return "off";
    case MetricsRequest::Export:
        return "export";
    }
    return {};
}

/// The CSV file a TASKSMACK_RENDER_METRICS value (UTF-8, nullptr when unset) names, surrounding
/// whitespace trimmed; nullopt when it is unset or blank.
[[nodiscard]] inline std::optional<std::filesystem::path> parse(const char* value)
{
    const std::string_view text = Platform::trimAscii(value != nullptr ? value : "");
    if (text.empty())
    {
        return std::nullopt;
    }
    return Core::utf8ToPath(text);
}

/// Where a metrics=export step writes when TASKSMACK_RENDER_METRICS is unset but TASKSMACK_SCREENSHOT
/// names @p screenshot: <stem>-metrics.csv beside it ("shots/idle.png" -> "shots/idle-metrics.csv").
[[nodiscard]] inline std::filesystem::path csvBesideScreenshot(const std::filesystem::path& screenshot)
{
    std::filesystem::path name = screenshot.stem();
    name += "-metrics.csv"; // ASCII, so exact whatever the narrow encoding
    return screenshot.parent_path() / name;
}

/// The rows in @p csv as toCsv() writes it: its lines less the header.
[[nodiscard]] inline std::size_t csvRowCount(const std::string_view csv) noexcept
{
    std::size_t lines = 0;
    for (const char character : csv)
    {
        lines += character == '\n' ? 1 : 0;
    }
    return lines == 0 ? 0 : lines - 1;
}

/// Writes @p csv (UI::RenderMetrics::toCsv()'s text, byte for byte) to @p path, creating or replacing
/// the file; the directory must exist. The error text on failure. Opens the path itself, so a name in
/// any encoding works with or without the manifest (#1648).
[[nodiscard]] std::expected<void, std::string> writeCsv(const std::filesystem::path& path, std::string_view csv);

/// The file TASKSMACK_RENDER_METRICS names, read and logged on the first call (main() makes it at
/// startup); nullopt when it is unset or blank. Thread-safe.
[[nodiscard]] const std::optional<std::filesystem::path>& active();

} // namespace App::RenderMetricsOverride
