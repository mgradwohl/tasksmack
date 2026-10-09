#include "SystemInfoSections.h"

#include "Domain/SystemInfoModel.h"
#include "Platform/ISystemInfoProbe.h"
#include "UI/Format.h"
#include "UI/IconsFontAwesome6.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <initializer_list>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace App::SystemInfo
{

namespace
{

constexpr std::string_view NOT_REPORTED = "Not reported by this system";
constexpr std::string_view NEEDS_ADMIN = "Requires administrator (readable by root only)";

[[nodiscard]] Row row(std::string label, std::string value, std::string_view reason = NOT_REPORTED, bool identifier = false)
{
    std::string why = value.empty() ? std::string(reason) : std::string{};
    return {.label = std::move(label), .value = std::move(value), .unavailableReason = std::move(why), .isIdentifier = identifier};
}

[[nodiscard]] bool containsIgnoringCase(std::string_view text, std::string_view needle)
{
    const auto lower = [](unsigned char c)
    {
        return std::tolower(c);
    };
    return !std::ranges::search(text, needle, {}, lower, lower).empty();
}

void appendRowsWindows(const Platform::OsInfo& os, std::vector<Row>& rows)
{
    rows.push_back(row("Build", os.build));
    rows.push_back(row("Architecture", os.architecture));
    rows.push_back(row("Installed", UI::Format::formatEpochDateTime(os.installUnixSeconds)));
}

void appendRowsLinux(const Platform::OsInfo& os, std::vector<Row>& rows)
{
    rows.push_back(row("Kernel", os.kernel));
    rows.push_back(row("Architecture", os.architecture));
    rows.push_back(row("Init system", os.initSystem, "/proc/1/comm couldn't be read"));
    rows.push_back(row("Desktop", os.desktop, "XDG_CURRENT_DESKTOP isn't set (no desktop session)"));
    rows.push_back(row("Session type", os.sessionType, "XDG_SESSION_TYPE isn't set"));
    rows.push_back(row("Virtualization", os.virtualization));
}

/// The texts joined by @p separator, empty ones skipped.
[[nodiscard]] std::string joinNonEmpty(std::initializer_list<std::string_view> parts, std::string_view separator)
{
    std::string text;
    for (const std::string_view part : parts)
    {
        if (part.empty())
        {
            continue;
        }
        if (!text.empty())
        {
            text += separator;
        }
        text += part;
    }
    return text;
}

/// The label of module @p index: its locator, or with the bank locator in front when another module has
/// the same locator; "Module N" when the table names neither.
[[nodiscard]] std::string moduleLabel(std::span<const Platform::MemoryModule> modules, std::size_t index)
{
    const Platform::MemoryModule& module = modules[index];
    if (module.locator.empty())
    {
        return module.bankLocator.empty() ? std::format("Module {}", index + 1) : module.bankLocator;
    }
    const auto sameLocator = std::ranges::count(modules, module.locator, &Platform::MemoryModule::locator);
    if (sameLocator > 1 && !module.bankLocator.empty())
    {
        return module.bankLocator + " " + module.locator;
    }
    return module.locator;
}

/// Every module's moduleLabel(), with labels that still collide numbered in table order ("Motherboard
/// #1", "Motherboard #2"): soldered memory often gives every device the same locator and no bank.
[[nodiscard]] std::vector<std::string> moduleLabels(std::span<const Platform::MemoryModule> modules)
{
    std::vector<std::string> labels;
    labels.reserve(modules.size());
    for (std::size_t i = 0; i < modules.size(); ++i)
    {
        labels.push_back(moduleLabel(modules, i));
    }
    const std::vector<std::string> plain = labels;
    for (std::size_t i = 0; i < labels.size(); ++i)
    {
        if (std::ranges::count(plain, plain[i]) > 1)
        {
            const auto before = std::count(plain.begin(), plain.begin() + static_cast<std::ptrdiff_t>(i), plain[i]);
            labels[i] = std::format("{} #{}", plain[i], before + 1);
        }
    }
    return labels;
}

/// A size that can be 0: "0 B" for nothing, otherwise formatMemoryCapacity().
[[nodiscard]] std::string sizeText(std::uint64_t bytes)
{
    return bytes == 0 ? std::string("0 B") : formatMemoryCapacity(bytes);
}

/// The page file or swap rows: one per file, labelled by its path; else one row saying none are set
/// up, or why the list is missing.
void appendPageFileRows(const Platform::CommitPagingInfo& paging, std::vector<Row>& rows)
{
    const bool windows = paging.family == Platform::OsFamily::Windows;
    const char* listLabel = windows ? "Page files" : "Swap";
    if (!paging.pageFilesRead)
    {
        rows.push_back(row(listLabel, "", windows ? "The page file list couldn't be read" : "/proc/swaps couldn't be read"));
        return;
    }
    if (paging.pageFiles.empty())
    {
        rows.push_back(row(listLabel, windows ? "None (paging is off)" : "None configured"));
        return;
    }
    for (std::size_t i = 0; i < paging.pageFiles.size(); ++i)
    {
        const Platform::PageFile& file = paging.pageFiles[i];
        std::string label = file.path.empty() ? std::format("{} {}", windows ? "Page file" : "Swap device", i + 1) : file.path;
        rows.push_back(row(std::move(label), formatPageFile(file, paging.family)));
    }
}

void appendPagingRowsWindows(const Platform::CommitPagingInfo& paging, std::vector<Row>& rows)
{
    rows.push_back(row("Peak commit", formatMemoryCapacity(paging.commitPeakBytes), "GetPerformanceInfo failed"));
    appendPageFileRows(paging, rows);
    rows.push_back(row("Compressed memory",
                       paging.compressedBytes.has_value() ? sizeText(*paging.compressedBytes) : std::string{},
                       "No Memory Compression process: compression is off, or its working set couldn't be read"));
    const std::uint64_t page = paging.pageSizeBytes;
    rows.push_back(row("Page size",
                       page != 0 && page % 1024 == 0 ? std::format("{} KiB", page / 1024) : formatMemoryCapacity(page),
                       "GetPerformanceInfo failed"));
}

void appendPagingRowsLinux(const Platform::CommitPagingInfo& paging, std::vector<Row>& rows)
{
    const char* mode = "";
    switch (paging.overcommit)
    {
    case Platform::OvercommitMode::Heuristic:
        mode = "Heuristic (0)";
        break;
    case Platform::OvercommitMode::Always:
        mode = "Always overcommit (1)";
        break;
    case Platform::OvercommitMode::Strict:
        mode = "Strict, the commit limit is enforced (2)";
        break;
    case Platform::OvercommitMode::Unknown:
        break;
    }
    rows.push_back(row("Overcommit mode", mode, "/proc/sys/vm/overcommit_memory couldn't be read"));
    appendPageFileRows(paging, rows);

    if (!paging.zramRead)
    {
        rows.push_back(row("zram", "", "/sys/block couldn't be listed"));
    }
    else if (paging.zram.empty())
    {
        rows.push_back(row("zram", "None"));
    }
    for (const Platform::ZramDevice& device : paging.zram)
    {
        rows.push_back(row(device.name, formatZramDevice(device)));
    }
    std::string zswap;
    if (paging.zswapEnabled.has_value())
    {
        zswap = *paging.zswapEnabled ? "Enabled" : "Disabled";
    }
    rows.push_back(row("zswap", std::move(zswap), "/sys/module/zswap couldn't be read (zswap isn't built in)"));

    std::string hugePages;
    if (paging.hugePagesRead)
    {
        const std::string pageSize =
            paging.hugePageSizeBytes != 0 ? std::format(" ({} pages)", formatMemoryCapacity(paging.hugePageSizeBytes)) : std::string{};
        hugePages = paging.hugePagesTotal == 0 ? "None reserved" + pageSize
                                               : std::format("{} of {} free, {} reserved, {} surplus{}",
                                                             paging.hugePagesFree,
                                                             paging.hugePagesTotal,
                                                             paging.hugePagesReserved,
                                                             paging.hugePagesSurplus,
                                                             pageSize);
    }
    rows.push_back(row("Huge pages", std::move(hugePages), "/proc/meminfo has no HugePages_ lines"));
    rows.push_back(row("Transparent huge pages", paging.transparentHugePages, "/sys/kernel/mm/transparent_hugepage couldn't be read"));
}

} // namespace

std::string formatMemoryCapacity(std::uint64_t bytes)
{
    constexpr std::uint64_t MIB = std::uint64_t{1024} * 1024;
    constexpr std::uint64_t GIB = MIB * 1024;
    if (bytes == 0)
    {
        return {};
    }
    if (bytes % GIB == 0)
    {
        return std::format("{} GiB", bytes / GIB);
    }
    if (bytes < GIB && bytes % MIB == 0)
    {
        return std::format("{} MiB", bytes / MIB);
    }
    return UI::Format::formatBytes(static_cast<double>(bytes));
}

std::uint64_t installedMemoryBytes(const Platform::MemoryModulesInfo& memory)
{
    if (memory.installedBytes != 0 || !memory.tableRead || memory.modules.empty())
    {
        return memory.installedBytes;
    }
    std::uint64_t total = 0;
    for (const Platform::MemoryModule& module : memory.modules)
    {
        if (module.sizeBytes == 0)
        {
            return 0; // an unknown size would understate the total
        }
        total += module.sizeBytes;
    }
    return total;
}

std::string formatMemorySpeed(std::uint32_t configuredMts, std::uint32_t ratedMts)
{
    if (configuredMts == 0)
    {
        return ratedMts == 0 ? std::string{} : std::format("rated {} MT/s", ratedMts);
    }
    if (ratedMts == 0 || ratedMts == configuredMts)
    {
        return std::format("{} MT/s", configuredMts);
    }
    return std::format("{} MT/s (rated {} MT/s)", configuredMts, ratedMts);
}

std::string formatMemoryModule(const Platform::MemoryModule& module)
{
    const std::string capacity = formatMemoryCapacity(module.sizeBytes);
    const std::string kind = joinNonEmpty({capacity, module.type, module.formFactor}, " ");
    const std::string speed = formatMemorySpeed(module.configuredSpeedMts, module.speedMts);
    const std::string maker = joinNonEmpty({module.manufacturer, module.partNumber}, " ");
    return joinNonEmpty({kind, speed, maker}, ", ");
}

std::string formatUtcOffset(int minutes)
{
    if (minutes == 0)
    {
        return "UTC";
    }
    const int size = std::abs(minutes);
    return std::format("UTC{}{:02}:{:02}", minutes < 0 ? '-' : '+', size / 60, size % 60);
}

std::string formatTimeZone(std::string_view name, std::optional<int> offsetMinutes)
{
    if (!offsetMinutes.has_value())
    {
        return std::string(name);
    }
    if (name.empty())
    {
        return formatUtcOffset(*offsetMinutes);
    }
    return std::format("{} ({})", name, formatUtcOffset(*offsetMinutes));
}

Section buildOsSection(const Platform::OsInfo& os, std::uint64_t readAtUnixSeconds)
{
    const bool windows = os.family == Platform::OsFamily::Windows;
    Section section{.title = "Operating system", .icon = ICON_FA_DESKTOP, .rows = {}};
    auto& rows = section.rows;
    rows.push_back(row(windows ? "Edition" : "Distribution", os.name));
    rows.push_back(row("Version", os.version));
    if (windows)
    {
        appendRowsWindows(os, rows);
    }
    else
    {
        appendRowsLinux(os, rows);
    }

    rows.push_back(row("Boot time", UI::Format::formatEpochDateTime(os.bootUnixSeconds)));
    const bool hasUptime = os.bootUnixSeconds != 0 && readAtUnixSeconds >= os.bootUnixSeconds;
    rows.push_back(row("Uptime", hasUptime ? UI::Format::formatDuration(static_cast<double>(readAtUnixSeconds - os.bootUnixSeconds)) : ""));
    rows.push_back(row("Computer name", os.computerName, NOT_REPORTED, true));
    if (windows)
    {
        // A workgroup's name ("WORKGROUP") says little; a domain's names the organisation.
        rows.push_back(row(os.joinedToDomain ? "Domain" : "Workgroup", os.domainOrWorkgroup, NOT_REPORTED, os.joinedToDomain));
    }
    rows.push_back(row("User", os.userName, NOT_REPORTED, true));
    rows.push_back(row("Locale", os.locale));
    rows.push_back(row("Time zone", formatTimeZone(os.timeZone, os.utcOffsetMinutes)));
    if (windows)
    {
        rows.push_back(row("System directory", os.systemDirectory));
        rows.push_back(row("Windows directory", os.windowsDirectory));
    }
    return section;
}

Section buildFirmwareSection(const Platform::FirmwareInfo& firmware)
{
    Section section{.title = "Firmware & board", .icon = ICON_FA_MICROCHIP, .rows = {}};
    auto& rows = section.rows;
    const std::string_view identifierReason = firmware.identifiersNeedAdmin ? NEEDS_ADMIN : NOT_REPORTED;
    rows.push_back(row("Manufacturer", firmware.systemManufacturer));
    rows.push_back(row("Model", firmware.systemModel));
    rows.push_back(row("Version", firmware.systemVersion));
    rows.push_back(row("SKU", firmware.systemSku));
    rows.push_back(row("Family", firmware.systemFamily));
    rows.push_back(row("Serial number", firmware.systemSerial, identifierReason, true));
    rows.push_back(row("UUID", firmware.systemUuid, identifierReason, true));

    rows.push_back(row("BIOS vendor", firmware.biosVendor));
    rows.push_back(row("BIOS version", firmware.biosVersion));
    rows.push_back(row("BIOS release date", firmware.biosReleaseDate));
    const char* mode = "";
    switch (firmware.firmwareMode)
    {
    case Platform::FirmwareMode::Uefi:
        mode = "UEFI";
        break;
    case Platform::FirmwareMode::Legacy:
        mode = "Legacy BIOS";
        break;
    case Platform::FirmwareMode::Unknown:
        break;
    }
    rows.push_back(row("Firmware mode", mode));
    rows.push_back(row("SMBIOS version", firmware.smbiosVersion, firmware.smbiosVersionNeedsAdmin ? NEEDS_ADMIN : NOT_REPORTED));
    if (!firmware.embeddedControllerVersion.empty())
    {
        rows.push_back(row("Embedded controller", firmware.embeddedControllerVersion));
    }

    rows.push_back(row("Board manufacturer", firmware.boardManufacturer));
    rows.push_back(row("Board product", firmware.boardProduct));
    rows.push_back(row("Board version", firmware.boardVersion));
    rows.push_back(row("Board serial number", firmware.boardSerial, identifierReason, true));

    rows.push_back(row("Chassis type", firmware.chassisType));
    rows.push_back(row("Chassis manufacturer", firmware.chassisManufacturer));
    rows.push_back(row("Platform role", firmware.platformRole));
    return section;
}

Section buildMemorySection(const Platform::MemoryModulesInfo& memory)
{
    Section section{.title = "Memory modules", .icon = ICON_FA_MEMORY, .rows = {}};
    auto& rows = section.rows;
    // Without the SMBIOS table there are no modules or slots to list; say why once, in their place.
    const std::string_view tableReason = memory.tableNeedsAdmin ? NEEDS_ADMIN : NOT_REPORTED;
    const bool hasSlots = memory.tableRead && memory.slotCount > 0;
    rows.push_back(row("Slots used", hasSlots ? std::format("{} of {}", memory.modules.size(), memory.slotCount) : "", tableReason));
    rows.push_back(row("Maximum capacity", formatMemoryCapacity(memory.maxCapacityBytes), tableReason));
    const std::uint64_t installed = installedMemoryBytes(memory);
    rows.push_back(row("Installed memory", formatMemoryCapacity(installed), tableReason));
    std::string usable = formatMemoryCapacity(memory.usableBytes);
    if (memory.usableBytes != 0 && installed > memory.usableBytes)
    {
        usable += std::format(" ({} hardware reserved)", formatMemoryCapacity(installed - memory.usableBytes));
    }
    rows.push_back(row("Usable memory", std::move(usable)));

    if (!memory.tableRead || memory.modules.empty())
    {
        rows.push_back(row("Modules", "", memory.tableRead ? std::string_view{"The SMBIOS table lists no installed memory"} : tableReason));
        return section;
    }
    std::vector<std::string> labels = moduleLabels(memory.modules);
    for (std::size_t i = 0; i < memory.modules.size(); ++i)
    {
        rows.push_back(row(std::move(labels[i]), formatMemoryModule(memory.modules[i])));
    }
    return section;
}

std::string formatCommitCharge(std::uint64_t committedBytes, std::uint64_t limitBytes)
{
    if (committedBytes == 0)
    {
        return {};
    }
    if (limitBytes == 0)
    {
        return sizeText(committedBytes);
    }
    const double percent = (static_cast<double>(committedBytes) * 100.0) / static_cast<double>(limitBytes);
    return std::format("{} / {} ({:.0f}%)", sizeText(committedBytes), sizeText(limitBytes), percent);
}

std::string formatPageFile(const Platform::PageFile& file, Platform::OsFamily family)
{
    std::string text = std::format("{} used of {}", sizeText(file.usedBytes), sizeText(file.sizeBytes));
    if (family == Platform::OsFamily::Windows)
    {
        text += std::format(", peak {}", sizeText(file.peakBytes));
        return text;
    }
    if (!file.kind.empty())
    {
        text += ", " + file.kind;
    }
    text += std::format(", priority {}", file.priority);
    return text;
}

std::string formatZramDevice(const Platform::ZramDevice& device)
{
    if (device.originalBytes == 0)
    {
        return "Empty";
    }
    std::string text = std::format("{} stored in {}", sizeText(device.originalBytes), sizeText(device.compressedBytes));
    if (device.compressedBytes != 0)
    {
        text += std::format(" ({:.1f}:1)", static_cast<double>(device.originalBytes) / static_cast<double>(device.compressedBytes));
    }
    text += std::format(", {} of RAM", sizeText(device.memoryUsedBytes));
    return text;
}

Section buildCommitPagingSection(const Platform::CommitPagingInfo& paging)
{
    Section section{.title = "Commit & paging", .icon = ICON_FA_COMPRESS, .rows = {}};
    auto& rows = section.rows;
    const bool windows = paging.family == Platform::OsFamily::Windows;
    rows.push_back(row("Commit charge",
                       formatCommitCharge(paging.committedBytes, paging.commitLimitBytes),
                       windows ? "GetPerformanceInfo failed" : "/proc/meminfo has no Committed_AS line"));
    if (windows)
    {
        appendPagingRowsWindows(paging, rows);
    }
    else
    {
        appendPagingRowsLinux(paging, rows);
    }
    return section;
}

std::string formatVulnerabilityName(std::string_view name)
{
    // Acronyms the kernel's names use, written as they are in the advisories.
    static constexpr std::array<std::string_view, 9> ACRONYMS{"mds", "srbds", "l1tf", "tsa", "tsx", "itlb", "mmio", "gds", "rfds"};
    std::string out;
    std::size_t start = 0;
    while (start <= name.size())
    {
        const std::size_t end = std::min(name.find('_', start), name.size());
        const std::string_view word = name.substr(start, end - start);
        if (!out.empty())
        {
            out += ' ';
        }
        if (std::ranges::find(ACRONYMS, word) != ACRONYMS.end())
        {
            std::ranges::transform(
                word, std::back_inserter(out), [](char c) { return static_cast<char>(std::toupper(static_cast<unsigned char>(c))); });
        }
        else if (!word.empty())
        {
            out += (out.empty() ? static_cast<char>(std::toupper(static_cast<unsigned char>(word.front()))) : word.front());
            out += word.substr(1);
        }
        start = end + 1;
    }
    return out;
}

std::string formatVulnerabilitySummary(std::span<const Platform::CpuVulnerability> vulnerabilities)
{
    std::size_t vulnerable = 0;
    std::size_t mitigated = 0;
    std::size_t notAffected = 0;
    std::size_t other = 0;
    for (const Platform::CpuVulnerability& item : vulnerabilities)
    {
        if (item.status.starts_with("Vulnerable"))
        {
            ++vulnerable;
        }
        else if (item.status.starts_with("Mitigation"))
        {
            ++mitigated;
        }
        else if (item.status.starts_with("Not affected"))
        {
            ++notAffected;
        }
        else
        {
            ++other; // "Unknown: ...", or a status a newer kernel words differently
        }
    }
    std::string text;
    const auto add = [&text](std::size_t count, std::string_view what)
    {
        if (count == 0)
        {
            return;
        }
        if (!text.empty())
        {
            text += ", ";
        }
        text += std::format("{} {}", count, what);
    };
    add(vulnerable, "vulnerable");
    add(mitigated, "mitigated");
    add(notAffected, "not affected");
    add(other, "unknown");
    return text;
}

namespace
{

/// A present TPM: "Present (TPM 2.0)", "Present (TPM 1.2)", or "Present" when the version is unknown.
[[nodiscard]] std::string tpmPresentText(std::uint32_t versionMajor)
{
    switch (versionMajor)
    {
    case 2:
        return "Present (TPM 2.0)";
    case 1:
        return "Present (TPM 1.2)";
    default:
        return "Present";
    }
}

/// @p whenTrue, @p whenFalse, or @p whenAbsent for an empty @p value.
[[nodiscard]] std::string
triStateText(std::optional<bool> value, std::string_view whenTrue, std::string_view whenFalse, std::string_view whenAbsent)
{
    if (!value.has_value())
    {
        return std::string(whenAbsent);
    }
    return std::string(*value ? whenTrue : whenFalse);
}

} // namespace

Section buildSecuritySection(const Platform::PlatformSecurityInfo& security)
{
    using Platform::SecurityFeatureState;
    Section section{.title = "Security", .icon = ICON_FA_LOCK, .rows = {}};
    auto& rows = section.rows;

    std::string secureBoot;
    switch (security.secureBoot)
    {
    case SecurityFeatureState::On:
        secureBoot = "On";
        break;
    case SecurityFeatureState::Off:
        secureBoot = "Off";
        break;
    case SecurityFeatureState::NotSupported:
        secureBoot = "Not supported (not booted with UEFI)";
        break;
    case SecurityFeatureState::Unknown:
        break;
    }
    rows.push_back(row("Secure Boot", std::move(secureBoot), "The SecureBoot EFI variable couldn't be read"));

    std::string tpm;
    switch (security.tpm)
    {
    case SecurityFeatureState::On:
        tpm = tpmPresentText(security.tpmVersionMajor);
        break;
    case SecurityFeatureState::Off:
    case SecurityFeatureState::NotSupported:
        tpm = "Not detected";
        break;
    case SecurityFeatureState::Unknown:
        break;
    }
    rows.push_back(row("TPM", std::move(tpm), "/sys/class/tpm couldn't be read"));

    std::string lsms;
    for (const std::string& name : security.lsms)
    {
        lsms += (lsms.empty() ? "" : ", ") + name;
    }
    rows.push_back(row("Security modules", std::move(lsms), "securityfs (/sys/kernel/security) isn't mounted"));
    rows.push_back(row("SELinux", triStateText(security.selinuxEnforcing, "Enforcing", "Permissive", "Not active")));
    rows.push_back(row("AppArmor", triStateText(security.apparmorEnabled, "Enabled", "Disabled", "Not loaded")));
    std::string lockdown = security.lockdown;
    if (!lockdown.empty())
    {
        lockdown.front() = static_cast<char>(std::toupper(static_cast<unsigned char>(lockdown.front())));
    }
    rows.push_back(row(
        "Kernel lockdown", std::move(lockdown), "securityfs (/sys/kernel/security) isn't mounted, or the lockdown module isn't built in"));

    if (!security.vulnerabilitiesRead)
    {
        rows.push_back(row("CPU vulnerabilities", {}, "/sys/devices/system/cpu/vulnerabilities couldn't be read"));
        return section;
    }
    rows.push_back(row("CPU vulnerabilities", formatVulnerabilitySummary(security.vulnerabilities), "The kernel lists none"));
    for (const Platform::CpuVulnerability& item : security.vulnerabilities)
    {
        rows.push_back(row(formatVulnerabilityName(item.name), item.status));
    }
    return section;
}

std::string formatSensorReading(const Platform::SensorReading& reading)
{
    using Platform::SensorKind;
    switch (reading.kind)
    {
    case SensorKind::Temperature:
    {
        std::string text = std::format("{:.1f} \u00B0C", reading.value);
        std::string limits;
        if (reading.high.has_value())
        {
            limits = std::format("high {:.1f} \u00B0C", *reading.high);
        }
        if (reading.critical.has_value())
        {
            limits += std::format("{}critical {:.1f} \u00B0C", limits.empty() ? "" : ", ", *reading.critical);
        }
        if (!limits.empty())
        {
            text += " (" + limits + ")";
        }
        return text;
    }
    case SensorKind::Fan:
        return std::format("{:.0f} RPM", reading.value);
    case SensorKind::Voltage:
        return std::format("{:.2f} V", reading.value);
    case SensorKind::Current:
        return std::format("{:.2f} A", reading.value);
    case SensorKind::Power:
        return std::format("{:.1f} W", reading.value);
    }
    return {};
}

Section buildSensorsSection(const Platform::SensorsInfo& sensors)
{
    Section section{.title = "Sensors", .icon = ICON_FA_TEMPERATURE_HALF, .rows = {}};
    for (const Platform::SensorDevice& device : sensors.devices)
    {
        for (const Platform::SensorReading& reading : device.readings)
        {
            section.rows.push_back(row(std::format("{}: {}", device.name, reading.label), formatSensorReading(reading)));
        }
    }
    if (section.rows.empty())
    {
        section.rows.push_back(row("Sensors",
                                   {},
                                   sensors.listed
                                       ? "No hwmon sensors or thermal zones are exposed (common in virtual machines, containers and WSL)"
                                       : "/sys/class/hwmon couldn't be read"));
    }
    return section;
}

std::vector<Section> buildSystemInfoSections(const Domain::SystemInfoSnapshot& snapshot)
{
    std::vector<Section> sections;
    if (snapshot.version == 0)
    {
        return sections;
    }
    if (snapshot.os.family != Platform::OsFamily::Unknown)
    {
        sections.push_back(buildOsSection(snapshot.os, snapshot.readAtUnixSeconds));
    }
    if (snapshot.firmware.available)
    {
        sections.push_back(buildFirmwareSection(snapshot.firmware));
    }
    if (snapshot.memory.available)
    {
        sections.push_back(buildMemorySection(snapshot.memory));
    }
    if (snapshot.paging.available)
    {
        sections.push_back(buildCommitPagingSection(snapshot.paging));
    }
    if (snapshot.security.available)
    {
        sections.push_back(buildSecuritySection(snapshot.security));
    }
    if (snapshot.sensors.available)
    {
        sections.push_back(buildSensorsSection(snapshot.sensors));
    }
    // Further sections follow here, in the page's order.
    return sections;
}

std::vector<VisibleSection> visibleSections(std::span<const Section> sections, std::string_view filter, bool showIdentifiers)
{
    std::vector<VisibleSection> visible;
    for (std::size_t s = 0; s < sections.size(); ++s)
    {
        const Section& section = sections[s];
        const bool titleMatches = filter.empty() || containsIgnoringCase(section.title, filter);
        VisibleSection entry{.section = s, .rows = {}};
        for (std::size_t r = 0; r < section.rows.size(); ++r)
        {
            const Row& item = section.rows[r];
            if (item.isIdentifier && !showIdentifiers)
            {
                continue;
            }
            if (titleMatches || containsIgnoringCase(item.label, filter) || containsIgnoringCase(item.value, filter))
            {
                entry.rows.push_back(r);
            }
        }
        if (!entry.rows.empty())
        {
            visible.push_back(std::move(entry));
        }
    }
    return visible;
}

std::string sectionText(const Section& section, bool showIdentifiers)
{
    std::string text = section.title + "\n";
    for (const Row& item : section.rows)
    {
        if (item.isIdentifier && !showIdentifiers)
        {
            continue;
        }
        text += item.label + ": ";
        text += item.available() ? item.value : "unavailable (" + item.unavailableReason + ")";
        text += '\n';
    }
    return text;
}

std::string allSectionsText(std::span<const Section> sections, bool showIdentifiers)
{
    std::string text;
    for (const Section& section : sections)
    {
        if (!text.empty())
        {
            text += '\n';
        }
        text += sectionText(section, showIdentifiers);
    }
    return text;
}

} // namespace App::SystemInfo
