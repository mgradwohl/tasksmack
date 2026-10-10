#pragma once

// The System Information page's content (#1399): titled sections of label/value rows, built from a
// Domain::SystemInfoSnapshot, plus the filter, identifier hiding and the Copy text. Pure (no ImGui
// calls), so all of it is unit-tested; SystemInfoView draws it.
//
// Adding a section: give it raw facts on the probe and the snapshot, a build*Section() here, and a
// line in buildSystemInfoSections() (CONTRIBUTING.md, "Adding a System Information section").

#include "Core/GraphicsHostInfo.h"
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

/// One disk's value: "Samsung SSD 980 PRO 1TB, NVMe SSD, 932 GiB, firmware 5B2QGXA7, 41 °C", unknown
/// parts left out.
[[nodiscard]] std::string formatDisk(const Platform::PhysicalDisk& disk);

/// An NVMe health log: "3% used, 100% spare left, 0 media errors", with "Critical warning (0x04), " in
/// front when any warning bit is set.
[[nodiscard]] std::string formatNvmeHealth(const Platform::NvmeHealth& health);

/// An ATA drive's SMART health (#1631): "SMART OK, 0 bad sectors, 12345 power-on hours".
[[nodiscard]] std::string formatAtaHealth(const Platform::AtaHealth& health);

/// One volume's value: "Windows, NTFS, 120 GiB free of 476 GiB (75% used)" / "ext4 on /dev/sda2, ...";
/// a network one isn't sized ("nfs4 on server:/export, network, size not read"). A label that is an
/// identifier is left out.
[[nodiscard]] std::string formatVolume(const Platform::Volume& volume);

/// Whether a volume label names someone: cloud drives label theirs with the account's e-mail address
/// ("someone@example.com - Google Drive"), so a label with an '@' gets its own identifier row.
[[nodiscard]] bool volumeLabelIsIdentifier(std::string_view label);

/// The Storage section (#1517): one row per physical disk, its serial number (an identifier) and, where
/// read, its health; then one row per volume, labelled by its mount point or "C: drive", plus an
/// identifier row for a label that names someone.
[[nodiscard]] Section buildStorageSection(const Platform::StorageInfo& storage);

/// One adapter's value: "NVIDIA GeForce RTX 4070, 12 GiB dedicated, 15.9 GiB shared, PCI 01:00.0".
[[nodiscard]] std::string formatAdapter(const Platform::GraphicsAdapter& adapter);

/// One adapter's driver: "32.0.15.6094 (2024-09-05)" / "nvidia 560.35.03" / "amdgpu"; empty when unknown.
[[nodiscard]] std::string formatAdapterDriver(const Platform::GraphicsAdapter& adapter);

/// For each display, the index of the monitor that is it, if any: the same desktop rectangle (Windows),
/// else the same name; a lone display left over pairs with a lone monitor that has no rectangle (Linux).
[[nodiscard]] std::vector<std::optional<std::size_t>> matchMonitors(std::span<const Core::DisplayInfo> displays,
                                                                    std::span<const Platform::Monitor> monitors);

/// One display's value: "DELL U2720Q, 3840 × 2160 at 60 Hz, 150% scale, 27.0" (597 × 336 mm), HDR
/// (BT.2020 PQ, 10-bit), primary", the monitor's EDID name and facts used when @p monitor is set.
[[nodiscard]] std::string formatDisplay(const Core::DisplayInfo& display, const Platform::Monitor* monitor);

/// The Graphics & displays section (#1519): each adapter and its driver, TaskSmack's OpenGL context, the
/// display server (Linux), then each display (Core's SDL view merged with the platform's monitor facts)
/// with its EDID serial as an identifier, and any monitor no display matched.
[[nodiscard]] Section buildGraphicsSection(const Platform::GraphicsInfo& graphics, const Core::GraphicsHostInfo& host);

/// A CPU vulnerability's name as the section labels it: the kernel's file name with its underscores as
/// spaces and well-known acronyms capitalised ("spectre_v2" -> "Spectre v2", "mds" -> "MDS").
[[nodiscard]] std::string formatVulnerabilityName(std::string_view name);

/// "3 vulnerable, 12 mitigated, 9 not affected", the counts that are non-zero; empty for none.
[[nodiscard]] std::string formatVulnerabilitySummary(std::span<const Platform::CpuVulnerability> vulnerabilities);

/// The Security section (#1514): Secure Boot, the TPM, the security modules, SELinux and AppArmor, the
/// kernel lockdown mode, then the CPU vulnerabilities, a summary row and one row per vulnerability.
[[nodiscard]] Section buildSecuritySection(const Platform::PlatformSecurityInfo& security);

/// One sensor's value in its unit: "45.0 °C (high 80.0 °C, critical 100.0 °C)", "1200 RPM",
/// "12.18 V", "1.20 A", "15.2 W". The thresholds follow a temperature only when the driver reports them.
[[nodiscard]] std::string formatSensorReading(const Platform::SensorReading& reading);

/// The Sensors section (#1522): one row per reading, labelled "device: sensor" ("coretemp: Package id
/// 0"), devices in the OS's order. None exposed (common in VMs and WSL) shows one muted row saying so.
[[nodiscard]] Section buildSensorsSection(const Platform::SensorsInfo& sensors);

/// A device's name: its vendor (unless the name already starts with it) and name, then its ids,
/// "Intel Corporation Wi-Fi 6E AX211 (8086:51F0)"; "Unknown device (8086:51F0)" without names.
[[nodiscard]] std::string formatDeviceName(const Platform::Device& device);

/// A USB link speed: "1.5 Mbps", "480 Mbps", "5 Gbps", "10 Gbps"; empty for 0 (unknown).
[[nodiscard]] std::string formatUsbSpeed(double mbps);

/// The Devices section (#1520): the problem devices with their reasons, then the PCI devices grouped by
/// class, the USB devices (with the hub each hangs off, and their serial numbers as identifiers) and
/// the active audio endpoints.
[[nodiscard]] Section buildDevicesSection(const Platform::DevicesInfo& devices);

/// A driver's row value. Windows: its display name, start type, state when not running, file version,
/// company and image ("Microsoft ACPI Driver, Boot start, 10.0.26100.1, Microsoft Corporation,
/// C:\Windows\System32\drivers\ACPI.sys"). Linux: its size, what uses it, version, state when not Live,
/// "permanent" and taint flags ("1.2 MB, used by kvm_intel, tainted (OE)").
[[nodiscard]] std::string formatDriverValue(const Platform::KernelDriver& driver);

/// The Drivers section (#1521), titled "Kernel modules" on Linux: a count, then one row per driver or
/// module by name; on Linux, the tainted count and a note that built-in modules aren't listed.
[[nodiscard]] Section buildDriversSection(const Platform::DriversInfo& drivers);

/// A well-known exception code's name ("access violation" for 0xC0000005); empty for others.
[[nodiscard]] std::string_view exceptionCodeName(std::string_view code);

/// A crash's row value: "notepad.exe 10.0.26100.1 crashed in ntdll.dll 10.0.26100.1, exception 0xC0000005
/// (access violation), pid 6700", "notepad.exe 10.0.26100.1 hung (Quiesce)" or, on Linux, "python3 dumped
/// core, pid 4242, uid 1000, 1.5 MiB".
[[nodiscard]] std::string formatCrashValue(const Platform::CrashEvent& event);

/// The Recent crashes & hangs section (#1524), titled "Recent crashes" on Linux, the page's last: the counts
/// of the last CRASH_HISTORY_DAYS days, a muted note when only the newest CRASH_LIST_MAX are listed, then
/// one row per event, newest first, labelled by its local time.
[[nodiscard]] Section buildCrashesSection(const Platform::CrashesInfo& crashes);

/// The page's sections, in display order. Empty before the first read. @p host is Core's graphics facts
/// for the Graphics & displays section, captured on the UI thread.
/// One adapter's summary: "Up, Wi-Fi, MTU 1500, driver iwlwifi", unknown parts left out.
[[nodiscard]] std::string formatAdapterSummary(const Platform::NetworkAdapter& adapter);

/// An adapter's addresses: "192.168.1.20/24, fe80::1c2a:3bff:fe4d:5e6f/64", IPv4 first.
[[nodiscard]] std::string formatAdapterAddresses(const Platform::NetworkAdapter& adapter);

/// The Network adapters section (#1518): the default gateways and DNS servers, then per adapter its
/// summary, addresses, MAC address (an identifier) and Wi-Fi signal.
[[nodiscard]] Section buildNetworkAdaptersSection(const Platform::NetworkAdaptersInfo& network);

/// A boot phase's length: "850 ms", "4.21 s", "1 min 12.3 s".
[[nodiscard]] std::string formatBootDuration(std::uint64_t microseconds);

/// The Boot performance section (#1525): the last boot's total, then each phase systemd measured
/// (firmware, boot loader, kernel, initrd, userspace), as `systemd-analyze` prints them.
[[nodiscard]] Section buildBootPerformanceSection(const Platform::BootPerformanceInfo& boot);

[[nodiscard]] std::vector<Section> buildSystemInfoSections(const Domain::SystemInfoSnapshot& snapshot,
                                                           const Core::GraphicsHostInfo& host = {});

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
