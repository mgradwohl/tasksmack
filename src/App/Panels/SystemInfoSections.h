#pragma once

// The System Information page's content (#1399): titled sections of label/value rows, built from a
// Domain::SystemInfoSnapshot, plus the filter, identifier hiding and the Copy text. Pure (no ImGui
// calls), so all of it is unit-tested; SystemInfoView draws it.
//
// Adding a section: give it raw facts on the probe and the snapshot, a build*Section() here, and a
// line in buildSystemInfoSections() (CONTRIBUTING.md, "Adding a System Information section").

#include "Core/GraphicsHostInfo.h"
#include "Domain/SensorModel.h"
#include "Domain/SensorTracker.h"
#include "Domain/SystemInfoModel.h"
#include "Platform/ISystemInfoProbe.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace App::SystemInfo
{

/// What an unavailable value shows: an em dash, muted, with the reason as its tooltip.
inline constexpr std::string_view UNAVAILABLE_TEXT = "\xE2\x80\x94";

/// A button a row draws in its value cell, in place of its value.
enum class RowAction : std::uint8_t
{
    None,
    /// "Show Wi-Fi details", with a note that Windows may ask for location access (#1782): asks every read
    /// from then on for the Wi-Fi details that need it.
    ShowWifiDetails,
    /// "Show public IP", with a note that it asks OpenDNS (#1518): looks the public addresses up on every
    /// read from then on.
    ShowPublicIp,
    /// "Authenticate to read" (#2310): asks for an administrator's password, then reads the facts
    /// readable by root only through the privileged helper.
    AuthenticateToRead,
};

/// The note beside the "Authenticate to read" button (#2310).
inline constexpr std::string_view AUTHENTICATE_NOTE = "Asks for an administrator's password";

/// The note beside the "Show Wi-Fi details" button (#1782).
inline constexpr std::string_view WIFI_DETAILS_NOTE = "Windows may ask for location access";
/// The note beside the "Show public IP" button (#1518).
inline constexpr std::string_view PUBLIC_IP_NOTE = "Sends a DNS query to OpenDNS";

/// What a row shows in place of its text while identifiers are hidden (#2120): its label, value and
/// tooltips with the user's login name redacted (redactLoginName()).
struct RedactedText
{
    std::string label;
    std::string value;
    std::string unavailableReason;
    std::string valueTooltip;
};

/// One label/value row. An empty value is unavailable; unavailableReason says why.
struct Row
{
    std::string label;
    std::string value;
    std::string unavailableReason;
    bool isIdentifier = false; ///< Names the user or the machine: hidden (and left out of Copy) unless shown.
    /// A sensor row's kind (#1522): the row charts its history (sensorKey) in this kind's colour. Empty
    /// for every other row.
    std::optional<Platform::SensorKind> sensorKind;
    /// A sensor row's key in the sensor sampler's ranges and histories (Domain::sensorKey(),
    /// Domain::SensorPublication), not its label, which another sensor can share (#2340). Empty for
    /// every other row.
    std::string sensorKey;
    RowAction action = RowAction::None; ///< A button the view draws in the value cell.
    /// A tooltip on an available value that needs explaining, such as a CPU vulnerability Windows reports
    /// as Unknown (#1781). Empty for most rows; an unavailable row's tooltip is unavailableReason.
    std::string valueTooltip;
    /// Set when the label, value or a tooltip names the user in a path, such as a mount point under
    /// /media/<user> or a crash under /home/<user> (#2120): the text shown and copied while identifiers
    /// are hidden. Such a row stays visible, unlike an identifier, since the rest of it names nothing.
    std::optional<RedactedText> redacted;

    [[nodiscard]] bool available() const noexcept
    {
        return !value.empty();
    }

    /// The label to show: redacted while identifiers are hidden (#2120).
    [[nodiscard]] const std::string& shownLabel(bool showIdentifiers) const noexcept
    {
        return showIdentifiers || !redacted.has_value() ? label : redacted->label;
    }
    /// The value to show: redacted while identifiers are hidden (#2120).
    [[nodiscard]] const std::string& shownValue(bool showIdentifiers) const noexcept
    {
        return showIdentifiers || !redacted.has_value() ? value : redacted->value;
    }
    /// The unavailable reason to show: redacted while identifiers are hidden (#2120).
    [[nodiscard]] const std::string& shownUnavailableReason(bool showIdentifiers) const noexcept
    {
        return showIdentifiers || !redacted.has_value() ? unavailableReason : redacted->unavailableReason;
    }
    /// The value's tooltip to show: redacted while identifiers are hidden (#2120).
    [[nodiscard]] const std::string& shownValueTooltip(bool showIdentifiers) const noexcept
    {
        return showIdentifiers || !redacted.has_value() ? valueTooltip : redacted->valueTooltip;
    }
};

/// One titled section of rows.
/// buildSensorsSection()'s title: the one section a new sensor read changes (#2737).
inline constexpr std::string_view SENSORS_SECTION_TITLE = "Sensors";

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
/// With @p canAuthenticate, a fact readable by root only adds an "Authenticate to read" row (#2310).
[[nodiscard]] Section buildFirmwareSection(const Platform::FirmwareInfo& firmware, bool canAuthenticate = false);

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
/// With @p canAuthenticate, an SMBIOS table readable by root only adds an "Authenticate to read" row (#2310).
[[nodiscard]] Section buildMemorySection(const Platform::MemoryModulesInfo& memory, bool canAuthenticate = false);

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

/// A disk's partition layout (#1632): "GPT, 6 partitions" / "MBR, 1 partition" / "Not partitioned";
/// the count alone when the style is unknown (Linux without udev), empty when the layout wasn't read.
[[nodiscard]] std::string formatPartitionLayout(const Platform::PhysicalDisk& disk);

/// One partition (#1632): "Basic data, 930 GiB, offset 117 MiB \→ C:" / "nvme0n1p1, EFI System, 512 MiB,
/// offset 1 MiB \→ /boot/efi". An unknown type shows its GUID or MBR type byte, an unread one a dash.
[[nodiscard]] std::string formatPartition(const Platform::Partition& partition);

/// Whether a volume label names someone: cloud drives label theirs with the account's e-mail address
/// ("someone@example.com - Google Drive"), so a label with an '@' gets its own identifier row.
[[nodiscard]] bool volumeLabelIsIdentifier(std::string_view label);

/// The Storage section (#1517): one row per physical disk, its serial number (an identifier), where
/// read its health, and its partition layout with a row per partition (#1632); then one row per volume,
/// labelled by its mount point or "C: drive", plus an identifier row for a label that names someone.
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

/// The Graphics & displays section (#1519): each adapter, its driver, (Windows) its highest Direct3D
/// feature level (#1636) and, when the platform read Vulkan, its Vulkan API and driver versions (#1635);
/// TaskSmack's OpenGL context, the Vulkan loader's instance version, the display server (Linux), then each
/// display (Core's SDL view merged with the platform's monitor facts) with its EDID serial as an
/// identifier, and any monitor no display matched.
[[nodiscard]] Section buildGraphicsSection(const Platform::GraphicsInfo& graphics, const Core::GraphicsHostInfo& host);

/// A CPU vulnerability's name as the section labels it: the kernel's file name with its underscores as
/// spaces and well-known acronyms capitalised ("spectre_v2" -> "Spectre v2", "mds" -> "MDS").
[[nodiscard]] std::string formatVulnerabilityName(std::string_view name);

/// "3 vulnerable, 12 mitigated, 9 not affected, 2 unknown", the counts that are non-zero; empty for none.
/// Unknown counts an "Unknown" status (Windows didn't say whether the CPU needs the mitigation, #1781) and
/// any status worded otherwise.
[[nodiscard]] std::string formatVulnerabilitySummary(std::span<const Platform::CpuVulnerability> vulnerabilities);

/// A BitLocker state as the Security section shows it (#2584): "On", "Suspended", "Not encryptable"...;
/// "Unknown" when Windows didn't report it.
[[nodiscard]] std::string_view bitLockerStateText(Platform::BitLockerState state) noexcept;

/// The Security section (#1514): Secure Boot, the TPM, the security modules, SELinux and AppArmor, the
/// kernel lockdown mode and each open dm-crypt volume (#2585, "Disk encryption (nvme0n1p3)": its LUKS or
/// plain type and the mount points on it), then the CPU vulnerabilities, a summary row and one row per
/// vulnerability. On Windows, memory integrity (HVCI), Kernel DMA Protection and each fixed drive's
/// BitLocker state (#2584, read without administrator) take the place of the Linux-only rows.
[[nodiscard]] Section buildSecuritySection(const Platform::PlatformSecurityInfo& security);

/// One sensor's value in its unit: "45.0 °C (high 80.0 °C, critical 100.0 °C)", "1200 RPM",
/// "12.18 V", "1.20 A", "15.2 W". The thresholds follow a temperature only when the driver reports them.
[[nodiscard]] std::string formatSensorReading(const Platform::SensorReading& reading);

/// A sensor's range once it has moved: "min 41.0 °C, max 63.0 °C" in its kind's unit; empty before it
/// has two reads, or while it hasn't changed (#1522).
[[nodiscard]] std::string formatSensorRange(const Platform::SensorReading& reading, const Domain::SensorRange& range);

/// The Sensors section (#1522): one row per reading, labelled "device: sensor" ("coretemp: Package id
/// 0"), devices in the OS's order, with its range since the page first read it (@p ranges) once that
/// has moved. None exposed (common in VMs and WSL) shows one muted row saying so. On Windows a last row
/// says that most motherboard sensors need a vendor driver.
[[nodiscard]] Section buildSensorsSection(const Platform::SensorsInfo& sensors,
                                          const std::map<std::string, Domain::SensorRange>& ranges = {});

/// A device's name: its vendor (unless the name already starts with it) and name, then its ids,
/// "Intel Corporation Wi-Fi 6E AX211 (8086:51F0)"; "Unknown device (8086:51F0)" without names.
[[nodiscard]] std::string formatDeviceName(const Platform::Device& device);

/// A USB link speed: "1.5 Mbps", "480 Mbps", "5 Gbps", "10 Gbps"; empty for 0 (unknown).
[[nodiscard]] std::string formatUsbSpeed(double mbps);

/// The Devices section (#1520): the problem devices with their reasons, then the PCI devices grouped by
/// class, the USB devices (with the hub each hangs off, and their serial numbers as identifiers) and
/// the active audio endpoints.
[[nodiscard]] Section buildDevicesSection(const Platform::DevicesInfo& devices);

/// A driver's row value. Windows: its display name, start type, state when not running, why it failed to
/// start, "Unsigned" or "Untrusted signature", file version, company and image ("Microsoft ACPI Driver, Boot
/// start, 10.0.26100.1, Microsoft Corporation, C:\Windows\System32\drivers\ACPI.sys"). Linux: its size, what
/// uses it, version, state when not Live, "permanent" and taint flags ("1.2 MB, used by kvm_intel, tainted (OE)").
[[nodiscard]] std::string formatDriverValue(const Platform::KernelDriver& driver);

/// The Drivers section (#1521), titled "Kernel modules" on Linux: a count, then one row per driver or
/// module by name; on Linux, the tainted count and a note that built-in modules aren't listed. On Windows
/// (#1661) the loaded count is followed by "Problem drivers" (drivers that failed to start, and unsigned or
/// untrusted images) and one "Problem driver" row per problem; a driver whose signature couldn't be checked
/// gets a "<name> signature" row with no value, whose tooltip says why.
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
/// With @p canAuthenticate, other users' hidden journal entries add an "Authenticate to read" row (#2310).
[[nodiscard]] Section buildCrashesSection(const Platform::CrashesInfo& crashes, bool canAuthenticate = false);

/// One adapter's summary: "Up, Wi-Fi, MTU 1500, driver iwlwifi", unknown parts left out.
[[nodiscard]] std::string formatAdapterSummary(const Platform::NetworkAdapter& adapter);

/// Which of an adapter's addresses formatAdapterAddresses() lists.
enum class AddressScope : std::uint8_t
{
    Local,  ///< Those isLocalAddress() accepts: a plain row
    Public, ///< The rest, globally routable: an identifier row, since one can place the machine (#2119)
};

/// A loopback, unspecified, private (10/8, 172.16/12, 192.168/16), carrier-grade NAT (100.64/10),
/// link-local (169.254/16, fe80::/10), site-local (fec0::/10) or unique local (fc00::/7) address, or an
/// IPv4-mapped one of those: it can't be reached from, or place the machine on, the internet. False for
/// anything else, an address that doesn't parse included, so an unreadable one is treated as an identifier.
[[nodiscard]] bool isLocalAddress(const Platform::AdapterAddress& address);

/// An adapter's addresses in @p scope: "192.168.1.20/24, fe80::1c2a:3bff:fe4d:5e6f/64", IPv4 first.
[[nodiscard]] std::string formatAdapterAddresses(const Platform::NetworkAdapter& adapter, AddressScope scope);

/// A Wi-Fi adapter's signal: "-55 dBm (82%)" (the quality is Windows'), or whichever of the two was read;
/// empty for neither.
[[nodiscard]] std::string formatWifiSignal(const Platform::NetworkAdapter& adapter);

/// A Wi-Fi link's standard, band, channel and rate: "5 GHz, channel 36, 866.7 Mbit/s" (Linux),
/// "802.11ax (Wi-Fi 6), 5 GHz, channel 36, 866.7 Mbit/s transmit, 780.0 Mbit/s receive" (Windows),
/// unknown parts left out; a frequency outside the 2.4, 5 and 6 GHz Wi-Fi channels is shown in MHz.
[[nodiscard]] std::string formatWifiLink(const Platform::WifiLink& link);

/// A DHCP lease: "From 192.168.1.1, 1 day lease, expires <local date and time>", unknown parts left out.
[[nodiscard]] std::string formatDhcpLease(const Platform::DhcpLease& lease);

/// The Network adapters section (#1518): the default gateways and DNS servers, then per adapter its
/// summary, addresses, DHCP lease where it has one, its own DNS servers and search domains (an
/// identifier) where it has any, MAC address (an identifier), and for Wi-Fi its signal, network name
/// (an identifier) and link. With @p wifiDetailsGated (Windows, SystemInfoCapabilities), a wireless adapter
/// whose location-gated details weren't read has a "Wi-Fi details" row with the "Show Wi-Fi details"
/// button in place of its network name (#1782). A gateway or DHCP server that isLocalAddress() rejects is an
/// identifier, since a public one places the machine about as well as its own address (#2191); a public
/// adapter gateway or DHCP server is a row of its own, so the local ones stay visible. DNS servers stay plain.
[[nodiscard]] Section buildNetworkAdaptersSection(const Platform::NetworkAdaptersInfo& network, bool wifiDetailsGated = false);

/// The Network adapters section's public IP rows (#1518): none where the probe can't look them up
/// (@p hasPublicIp false); before the user asks (@p included false), one "Public IP" row with the
/// "Show public IP" button; after, a Public IPv4 and a Public IPv6 row, identifiers both.
[[nodiscard]] std::vector<Row> buildPublicIpRows(bool hasPublicIp, bool included, const Platform::PublicIpInfo& publicIp);

/// A boot phase's length: "850 ms", "4.21 s", "1 min 12.3 s".
[[nodiscard]] std::string formatBootDuration(std::uint64_t microseconds);

/// The Boot performance section (#1525): the last boot's total, then each phase systemd measured
/// (firmware, boot loader, kernel, initrd, userspace), as `systemd-analyze` prints them. On Windows: the
/// total, when the boot event was recorded, the main path (to the desktop) and the post-boot time.
[[nodiscard]] Section buildBootPerformanceSection(const Platform::BootPerformanceInfo& boot);

/// What a redacted login name reads as (#2120).
inline constexpr std::string_view REDACTED_USER = "<user>";

/// @p text with the path components that name the user by @p userName (OsInfo::userName) replaced by
/// REDACTED_USER (#2120): "/media/alice/USB" -> "/media/<user>/USB", "C:\Users\alice\x.exe" ->
/// "C:\Users\<user>\x.exe". The login name is userName after any "DOMAIN\" prefix and, for an
/// "alice@example.com" login, also the part before the '@'. Only a whole component matches: one that
/// follows a '/' or '\' and ends at one, at the end of the text, or at whitespace or one of ,;:)]}"' so
/// a short name never matches inside a longer word or file name. On Windows the match ignores ASCII
/// case, and a profile folder under Users named "<login>.<suffix>" (Windows adds ".DOMAIN" or ".000"
/// when the plain name is taken) is redacted whole. An empty login name redacts nothing.
[[nodiscard]] std::string redactLoginName(std::string_view text, std::string_view userName, Platform::OsFamily family);

/// @p text with the user's profile folder @p homeDirectory (OsInfo::homeDirectory) named REDACTED_USER
/// wherever a path is it or under it (#2197), whatever the folder is called: with "C:\Users\bob" (the
/// profile folder of a login since renamed "alice"), "C:\Users\bob\x.exe" -> "C:\Users\<user>\x.exe".
/// The path matches only where a component ends (as in redactLoginName()), so "C:\Users\bobby" is left
/// alone. On Windows ASCII case is ignored and '/' and '\' match each other. A folder with no name to
/// redact (empty, or a root such as "/") redacts nothing.
[[nodiscard]] std::string redactHomeDirectory(std::string_view text, std::string_view homeDirectory, Platform::OsFamily family);

/// Gives each row of @p sections that isn't an identifier, and whose label, value or tooltips name the
/// user (redactHomeDirectory() and redactLoginName() with @p os's homeDirectory, userName and family),
/// its redacted text (#2120, #2197).
void redactUserPaths(std::span<Section> sections, const Platform::OsInfo& os);

/// The page's sections, in display order, with redactUserPaths() applied to them all. Empty before the
/// first read. @p host is Core's graphics facts for the Graphics & displays section, captured on the UI
/// thread; @p capabilities what the probe can read; @p sensors the sensor sampler's newest read (#2737),
/// whose sensors and ranges the Sensors section shows once it has one (the page's own read until then).
[[nodiscard]] std::vector<Section> buildSystemInfoSections(const Domain::SystemInfoSnapshot& snapshot,
                                                           const Core::GraphicsHostInfo& host = {},
                                                           const Platform::SystemInfoCapabilities& capabilities = {},
                                                           const Domain::SensorPublication* sensors = nullptr);

/// The sections and rows to draw: hidden identifiers left out, then, with a filter, a section whose
/// title matches keeps all its rows and otherwise only rows whose shown label or value matches (ASCII
/// case-insensitive; the redacted text while identifiers are hidden, so a filter on the user's name
/// can't pick out the rows that name it). Sections left with no rows are dropped.
[[nodiscard]] std::vector<VisibleSection> visibleSections(std::span<const Section> sections, std::string_view filter, bool showIdentifiers);

/// A section as plain text: its title, then one "Label: Value" line per row (an unavailable value as
/// "unavailable (reason)"), hidden identifiers left out and, while they are, the redacted text (#2120)
/// of a row that names the user.
[[nodiscard]] std::string sectionText(const Section& section, bool showIdentifiers);

/// Every section's sectionText(), separated by blank lines.
[[nodiscard]] std::string allSectionsText(std::span<const Section> sections, bool showIdentifiers);

/// A saved report's format (#1866).
enum class ReportFormat : std::uint8_t
{
    Text, ///< allSectionsText()
    Json, ///< allSectionsJson()
};

/// Every section as JSON (#1866), for scripts and diffing: {"readAt", "identifiersIncluded",
/// "sections": [{"title", "rows": [{"label", "value"} or {"label", "unavailable": reason}]}]}. Rows
/// left out of Copy (identifiers, unless @p showIdentifiers) are left out here too, and a row's text is
/// redacted as Copy's is (#2120). Strings are escaped
/// as JSON requires; non-ASCII text stays UTF-8.
[[nodiscard]] std::string allSectionsJson(std::span<const Section> sections, bool showIdentifiers, std::string_view readAt);

/// @p text as a quoted JSON string: quotes, backslashes and control characters escaped.
[[nodiscard]] std::string jsonString(std::string_view text);

/// The report's default file name: "tasksmack-system-info-<computer>-<date>.<txt|json>", @p date as
/// "YYYY-MM-DD" (left out when empty). The computer name only with @p showIdentifiers (it is an
/// identifier), and only its letters, digits, '-', '_' and '.' (others become '-'); "machine"
/// otherwise or when none is left.
[[nodiscard]] std::string reportFileName(std::string_view computerName, bool showIdentifiers, std::string_view date, ReportFormat format);

/// The format to save @p path in: its typed extension when that is ".txt" or ".json" (#2144), else the
/// dialog's @p filter (1 is JSON), else text.
[[nodiscard]] ReportFormat reportFormatFor(std::string_view path, int filter);

/// @p path with the format's extension added when it has none (a name typed without one).
[[nodiscard]] std::string withReportExtension(std::string path, ReportFormat format);

/// @p path with " (@p number)" before its extension: "dir/report.txt", 2 is "dir/report (2).txt" (#2144).
[[nodiscard]] std::string numberedReportPath(std::string_view path, int number);

} // namespace App::SystemInfo
