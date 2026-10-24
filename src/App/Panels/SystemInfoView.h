#pragma once

// The System tab's page (#1399): the toolbar (filter, Show identifiers, Copy all, Refresh) and the
// titled label/value sections. Split from SystemInfoPanel, which owns the model and its reads, so it
// runs headless in TaskSmackTests; the content itself is built in SystemInfoSections.h.

#include "App/Panels/SystemInfoSections.h"
#include "Core/GraphicsHostInfo.h"
#include "Domain/SensorModel.h"
#include "Domain/SystemInfoModel.h"
#include "Platform/ISystemInfoProbe.h"
#include "UI/CellText.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace App
{

namespace SystemInfo
{

/// How the toolbar shows when the facts were read (#1922).
enum class ReadAtForm : std::uint8_t
{
    Full,     ///< "Read at 12:34:56" after Refresh.
    TimeOnly, ///< "12:34:56" after Refresh, "Read at 12:34:56" as its tooltip.
    NextLine, ///< Not even the time fits after Refresh: the full form on its own line, under the row.
};

/// The widths the toolbar row is laid out from, in pixels.
struct ToolbarWidths
{
    float rowPx;          ///< The row: the content region's width.
    float filterWantedPx; ///< ProcessTableLayout::computeFilterWidth() for this row: the box at its best.
    float filterHintPx;   ///< The box's hint plus its frame padding: it never shrinks below this.
    float controlsPx;     ///< Show identifiers, Copy all, Save report... and Refresh, each with the spacing before it,
                          ///< and the spacing before the read time.
    float readAtFullPx;   ///< "Read at 12:34:56" (or "Reading...").
    float readAtShortPx;  ///< "12:34:56" (or "Reading...", which has no shorter form).
};

/// Where the toolbar row's width goes.
struct ToolbarLayout
{
    float filterPx = 0.0F;
    ReadAtForm readAt = ReadAtForm::Full;
};

/// Lays out the toolbar row so the read time is never cut off at the window's edge (#1922). The
/// filter box gives way first, down to its hint; then the time drops its "Read at"; and only when not
/// even the time fits does it wrap to its own line. This row does not feed the window minimum (the
/// Processes toolbar and the Overview's charts set it): narrowing the box is enough for the full form
/// at the shipped fonts and the window minimum, and the shorter forms cover anything narrower.
[[nodiscard]] inline ToolbarLayout computeToolbarLayout(const ToolbarWidths& widths) noexcept
{
    const auto atLeastZero = [](float value)
    {
        return (std::isfinite(value) && value > 0.0F) ? value : 0.0F;
    };
    const float row = atLeastZero(widths.rowPx);
    const float hint = atLeastZero(widths.filterHintPx);
    const float wanted = std::max(atLeastZero(widths.filterWantedPx), hint);
    const float controls = atLeastZero(widths.controlsPx);
    // The widest box, between the hint and its wanted width, that leaves the time this much room.
    const auto filterFor = [&](float timePx)
    {
        return std::clamp(row - controls - timePx, hint, wanted);
    };
    const auto fits = [&](float filterPx, float timePx)
    {
        return !UI::Widgets::isCellTextClipped(timePx, row - filterPx - controls);
    };

    const float full = atLeastZero(widths.readAtFullPx);
    if (const float filter = filterFor(full); fits(filter, full))
    {
        return {.filterPx = filter, .readAt = ReadAtForm::Full};
    }
    const float time = atLeastZero(widths.readAtShortPx);
    if (const float filter = filterFor(time); fits(filter, time))
    {
        return {.filterPx = filter, .readAt = ReadAtForm::TimeOnly};
    }
    return {.filterPx = filterFor(0.0F), .readAt = ReadAtForm::NextLine};
}

} // namespace SystemInfo

/// What the view drew this frame.
enum class SystemInfoViewContent : std::uint8_t
{
    Unsupported, ///< The platform has no System Information facts.
    Loading,     ///< No read yet.
    Sections,
};

/// The view's state between frames. The sections are rebuilt only when a new read is published, and
/// the visible rows only when that, the filter or the identifier toggle changes (#580).
struct SystemInfoViewState
{
    std::string filter;
    bool showIdentifiers = false;

    /// Core's graphics facts for the Graphics & displays section (#1519), set by the panel with each new
    /// read; the sections are built from it and the snapshot together.
    Core::GraphicsHostInfo host;

    std::vector<SystemInfo::Section> sections;
    std::uint64_t sectionsVersion = 0;        ///< The page read the sections were built from
    std::uint64_t sectionsSensorsVersion = 0; ///< The sensor read they were built from (#2737)
    std::uint64_t sectionsGeneration = 0;     ///< Counts rebuilds, for the visible rows
    std::string readAtText;                   ///< "Read at HH:MM:SS", with the sections.
    /// How the toolbar showed the read time last frame (#1922): shortened or wrapped when the row is narrow.
    SystemInfo::ReadAtForm readAtForm = SystemInfo::ReadAtForm::Full;

    std::vector<SystemInfo::VisibleSection> visible;
    std::uint64_t visibleVersion = 0;
    std::string visibleFilter;
    bool visibleShowIdentifiers = false;

    std::string copiedText; ///< What the last Copy put on the clipboard.

    /// "Show Wi-Fi details" was clicked (#1782): its buttons stay disabled until a read without the
    /// details withheld replaces them.
    bool wifiDetailsClicked = false;
    /// "Show public IP" was clicked (#1518): its button stays disabled until a read with the addresses
    /// replaces it.
    bool publicIpClicked = false;
    /// "Authenticate to read" was clicked (#2310): its buttons stay disabled until the next read
    /// replaces them, which offers them again if the prompt was dismissed.
    bool authenticateClicked = false;

    /// A save dialog is open (#1866): Save report... is disabled until it closes. Set by the panel.
    bool saveDialogOpen = false;
    /// The last Save report's outcome, "Saved to ..." or why it failed, shown after the read time.
    std::string saveNote;
};

struct SystemInfoViewResult
{
    SystemInfoViewContent content = SystemInfoViewContent::Loading;
    bool refreshRequested = false; ///< Refresh was clicked.
    /// "Show Wi-Fi details" was clicked this frame (#1782): read again with the Wi-Fi details, and keep
    /// reading them for the rest of the session.
    bool wifiDetailsRequested = false;
    /// "Show public IP" was clicked this frame (#1518): read again with the public addresses, and keep
    /// looking them up for the rest of the session.
    bool publicIpRequested = false;
    /// "Authenticate to read" was clicked this frame (#2310): read again after asking for an
    /// administrator's password.
    bool authenticateRequested = false;
    /// "Save report..." was clicked this frame (#1866): ask where to save the page.
    bool saveReportRequested = false;
};

/// Draws the page into the current window.
/// @param snapshot Null is treated as no read yet.
/// @param reading  A read is in flight: Refresh is disabled.
/// @param sensors  The sensor sampler's newest read (#2737): the Sensors section's readings, ranges and
///                 charts. Null, or version 0, for none yet.
SystemInfoViewResult renderSystemInfoView(const Domain::SystemInfoSnapshot* snapshot,
                                          const Platform::SystemInfoCapabilities& capabilities,
                                          bool reading,
                                          SystemInfoViewState& state,
                                          const Domain::SensorPublication* sensors = nullptr);

} // namespace App
