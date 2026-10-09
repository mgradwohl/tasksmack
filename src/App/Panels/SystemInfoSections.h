#pragma once

// The System Information page's content (#1399): titled sections of label/value rows, built from a
// Domain::SystemInfoSnapshot, plus the filter, identifier hiding and the Copy text. Pure (no ImGui
// calls), so all of it is unit-tested; SystemInfoView draws it.
//
// Adding a section: give it raw facts on the probe and the snapshot, a build*Section() here, and a
// line in buildSystemInfoSections() (CONTRIBUTING.md, "Adding a System Information section").

#include "Domain/SystemInfoModel.h"
#include "Platform/ISystemInfoProbe.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace App::SystemInfo
{

/// What an unavailable value shows: an em dash, muted, with the reason as its tooltip.
inline constexpr std::string_view UNAVAILABLE_TEXT = "\xE2\x80\x94";

/// One label/value row. An empty value is unavailable; unavailableReason says why.
struct Row
{
    std::string label;
    std::string value;
    std::string unavailableReason;
    bool isIdentifier = false; ///< Names the user or the machine: hidden (and left out of Copy) unless shown.

    [[nodiscard]] bool available() const noexcept
    {
        return !value.empty();
    }
};

/// One titled section of rows.
struct Section
{
    std::string title;
    const char* icon = ""; ///< A Font Awesome glyph for the section header.
    std::vector<Row> rows;
};

/// The rows of one section that pass the filter and the identifier toggle, as indices.
struct VisibleSection
{
    std::size_t section = 0;
    std::vector<std::size_t> rows;
};

/// "UTC+05:30", "UTC-08:00", "UTC" for minutes east of UTC.
[[nodiscard]] std::string formatUtcOffset(int minutes);

/// "Pacific Standard Time (UTC-07:00)"; the name alone without an offset, the offset alone without a
/// name, empty with neither.
[[nodiscard]] std::string formatTimeZone(std::string_view name, std::optional<int> offsetMinutes);

/// The Operating system section (#1512). The rows depend on os.family; uptime is as of the read.
[[nodiscard]] Section buildOsSection(const Platform::OsInfo& os, std::uint64_t readAtUnixSeconds);

/// The Firmware & board section (#1513). Serials and the UUID are identifiers; the embedded controller
/// row is left out when there is none.
[[nodiscard]] Section buildFirmwareSection(const Platform::FirmwareInfo& firmware);

/// The page's sections, in display order. Empty before the first read.
[[nodiscard]] std::vector<Section> buildSystemInfoSections(const Domain::SystemInfoSnapshot& snapshot);

/// The sections and rows to draw: hidden identifiers left out, then, with a filter, a section whose
/// title matches keeps all its rows and otherwise only rows whose label or value matches (ASCII case-
/// insensitive). Sections left with no rows are dropped.
[[nodiscard]] std::vector<VisibleSection> visibleSections(std::span<const Section> sections, std::string_view filter, bool showIdentifiers);

/// A section as plain text: its title, then one "Label: Value" line per row (an unavailable value as
/// "unavailable (reason)"), hidden identifiers left out.
[[nodiscard]] std::string sectionText(const Section& section, bool showIdentifiers);

/// Every section's sectionText(), separated by blank lines.
[[nodiscard]] std::string allSectionsText(std::span<const Section> sections, bool showIdentifiers);

} // namespace App::SystemInfo
