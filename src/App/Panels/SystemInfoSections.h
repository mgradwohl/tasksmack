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

/// A memory size: whole GiB ("16 GiB") or, under 1 GiB, whole MiB ("512 MiB") when exact, else
/// UI::Format::formatBytes() ("31.7 GiB"). Empty for 0 (unknown).
[[nodiscard]] std::string formatMemoryCapacity(std::uint64_t bytes);

/// The installed memory: the platform's own figure (Windows), else the populated modules' total when
/// the table was read and every module's size is known (Linux); 0 when unknown.
[[nodiscard]] std::uint64_t installedMemoryBytes(const Platform::MemoryModulesInfo& memory);

/// "5600 MT/s", "5600 MT/s (rated 6400 MT/s)", "rated 6400 MT/s" when only the rating is known; empty
/// when neither is.
[[nodiscard]] std::string formatMemorySpeed(std::uint32_t configuredMts, std::uint32_t ratedMts);

/// One module's value: "16 GiB DDR5 SODIMM, 5600 MT/s (rated 6400 MT/s), Samsung M425R2GA3BB0-CWMOD",
/// unknown parts left out.
[[nodiscard]] std::string formatMemoryModule(const Platform::MemoryModule& module);

/// The Memory modules section (#1515): slots used of total, maximum capacity, installed and usable
/// memory (with the hardware-reserved difference), then one row per populated module, labelled by its
/// locator. Without the SMBIOS table (Linux, unprivileged) a "Modules" row says it needs administrator.
[[nodiscard]] Section buildMemorySection(const Platform::MemoryModulesInfo& memory);

/// A commit charge: "12.3 GiB / 31.7 GiB (39%)"; the used figure alone without a limit, empty when
/// the charge is unknown (0).
[[nodiscard]] std::string formatCommitCharge(std::uint64_t committedBytes, std::uint64_t limitBytes);

/// One page file or swap device: "1.2 GiB used of 16 GiB, peak 3.4 GiB" (Windows) / "512 MiB used of
/// 8 GiB, partition, priority -2" (Linux).
[[nodiscard]] std::string formatPageFile(const Platform::PageFile& file, Platform::OsFamily family);

/// One zram device: "3.2 GiB stored in 812 MiB (4.0:1), 840 MiB of RAM"; "Empty" before it holds data.
[[nodiscard]] std::string formatZramDevice(const Platform::ZramDevice& device);

/// The Commit & paging section (#1516): the commit charge against its limit, then per platform the
/// peak commit, each page file, compressed memory and the page size (Windows), or the overcommit mode,
/// each swap device, zram, zswap and huge pages (Linux).
[[nodiscard]] Section buildCommitPagingSection(const Platform::CommitPagingInfo& paging);

/// The page's sections, in display order. Empty before the first read.
/// A CPU vulnerability's name as the section labels it: the kernel's file name with its underscores as
/// spaces and well-known acronyms capitalised ("spectre_v2" -> "Spectre v2", "mds" -> "MDS").
[[nodiscard]] std::string formatVulnerabilityName(std::string_view name);

/// "3 vulnerable, 12 mitigated, 9 not affected", the counts that are non-zero; empty for none.
[[nodiscard]] std::string formatVulnerabilitySummary(std::span<const Platform::CpuVulnerability> vulnerabilities);

/// The Security section (#1514): Secure Boot, the TPM, the security modules, SELinux and AppArmor, the
/// kernel lockdown mode, then the CPU vulnerabilities, a summary row and one row per vulnerability.
[[nodiscard]] Section buildSecuritySection(const Platform::PlatformSecurityInfo& security);

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
