#include "SystemInfoSections.h"

#include "Core/GraphicsHostInfo.h"
#include "Domain/SystemInfoModel.h"
#include "Platform/ISystemInfoProbe.h"
#include "UI/Format.h"
#include "UI/IconsFontAwesome6.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <initializer_list>
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

std::string formatDisk(const Platform::PhysicalDisk& disk)
{
    std::string kind = disk.bus;
    if (disk.media != Platform::DiskMedia::Unknown)
    {
        kind = joinNonEmpty({kind, disk.media == Platform::DiskMedia::Ssd ? "SSD" : "HDD"}, " ");
    }
    const std::string firmware = disk.firmware.empty() ? std::string{} : "firmware " + disk.firmware;
    constexpr std::string_view DEGREE = "\xC2\xB0"; // U+00B0, UTF-8
    const std::string temperature =
        disk.temperatureCelsius.has_value() ? std::format("{} {}C", *disk.temperatureCelsius, DEGREE) : std::string{};
    return joinNonEmpty({disk.model, kind, formatMemoryCapacity(disk.sizeBytes), firmware, temperature}, ", ");
}

std::string formatNvmeHealth(const Platform::NvmeHealth& health)
{
    std::string text = health.criticalWarning != 0 ? std::format("Critical warning (0x{:02X}), ", health.criticalWarning) : std::string{};
    text +=
        std::format("{}% used, {}% spare left, {} media errors", health.percentageUsed, health.availableSparePercent, health.mediaErrors);
    return text;
}

std::string formatVolume(const Platform::Volume& volume)
{
    std::string fileSystem = volume.fileSystem;
    if (!volume.device.empty())
    {
        fileSystem = joinNonEmpty({fileSystem, volume.device}, " on ");
    }
    std::string space;
    if (volume.network)
    {
        space = "network, size not read";
    }
    else if (volume.sizeRead && volume.sizeBytes != 0)
    {
        const std::uint64_t used = volume.sizeBytes - std::min(volume.freeBytes, volume.sizeBytes);
        const double percent = (static_cast<double>(used) * 100.0) / static_cast<double>(volume.sizeBytes);
        space = std::format("{} free of {} ({:.0f}% used)", sizeText(volume.freeBytes), sizeText(volume.sizeBytes), percent);
    }
    return joinNonEmpty({volumeLabelIsIdentifier(volume.label) ? std::string_view{} : volume.label, fileSystem, space}, ", ");
}

bool volumeLabelIsIdentifier(std::string_view label)
{
    return label.contains('@');
}

Section buildStorageSection(const Platform::StorageInfo& storage)
{
    Section section{.title = "Storage", .icon = ICON_FA_HARD_DRIVE, .rows = {}};
    auto& rows = section.rows;
    const bool windows = storage.family == Platform::OsFamily::Windows;
    if (!storage.disksRead || storage.disks.empty())
    {
        rows.push_back(row("Disks", storage.disksRead ? "None found" : "", "/sys/block couldn't be listed"));
    }
    for (const Platform::PhysicalDisk& disk : storage.disks)
    {
        rows.push_back(row(disk.name, formatDisk(disk)));
        rows.push_back(row(disk.name + " serial number", disk.serial, "Not reported by this drive", true));
        if (disk.health.has_value() || !disk.healthUnavailableReason.empty())
        {
            rows.push_back(row(disk.name + " health",
                               disk.health.has_value() ? formatNvmeHealth(*disk.health) : std::string{},
                               disk.healthUnavailableReason));
        }
    }

    if (!storage.volumesRead || storage.volumes.empty())
    {
        rows.push_back(row("Volumes",
                           storage.volumesRead ? "None mounted" : "",
                           windows ? "GetLogicalDriveStringsW failed" : "/proc/self/mountinfo couldn't be read"));
    }
    for (const Platform::Volume& volume : storage.volumes)
    {
        // "C: drive" rather than a bare "C:", which copies as "C:: ...".
        const std::string label = windows ? volume.mountPoint + " drive" : volume.mountPoint;
        rows.push_back(row(label, formatVolume(volume), "Its file system and size couldn't be read"));
        if (volumeLabelIsIdentifier(volume.label))
        {
            rows.push_back(row(label + " label", volume.label, NOT_REPORTED, true));
        }
    }
    return section;
}

std::string formatAdapter(const Platform::GraphicsAdapter& adapter)
{
    const std::string dedicated = adapter.dedicatedBytes == 0 ? std::string{} : formatMemoryCapacity(adapter.dedicatedBytes) + " dedicated";
    const std::string shared = adapter.sharedBytes == 0 ? std::string{} : formatMemoryCapacity(adapter.sharedBytes) + " shared";
    const std::string location = adapter.location.empty() ? std::string{} : "PCI " + adapter.location;
    return joinNonEmpty({adapter.name, dedicated, shared, location}, ", ");
}

std::string formatAdapterDriver(const Platform::GraphicsAdapter& adapter)
{
    std::string text = joinNonEmpty({adapter.driver, adapter.driverVersion}, " ");
    if (!adapter.driverDate.empty())
    {
        text = joinNonEmpty({text, "(" + adapter.driverDate + ")"}, " ");
    }
    return text;
}

std::vector<std::optional<std::size_t>> matchMonitors(std::span<const Core::DisplayInfo> displays,
                                                      std::span<const Platform::Monitor> monitors)
{
    std::vector<std::optional<std::size_t>> matches(displays.size());
    std::vector<bool> used(monitors.size(), false);
    const auto claim = [&](std::size_t display, const auto& same)
    {
        for (std::size_t m = 0; m < monitors.size() && !matches[display].has_value(); ++m)
        {
            if (!used[m] && same(displays[display], monitors[m]))
            {
                matches[display] = m;
                used[m] = true;
            }
        }
    };
    for (std::size_t d = 0; d < displays.size(); ++d)
    {
        claim(d,
              [](const Core::DisplayInfo& display, const Platform::Monitor& monitor)
              {
                  return monitor.hasDesktopRect && monitor.desktopX == display.x && monitor.desktopY == display.y &&
                         monitor.desktopWidth == display.width && monitor.desktopHeight == display.height;
              });
        claim(d,
              [](const Core::DisplayInfo& display, const Platform::Monitor& monitor)
              { return !monitor.name.empty() && monitor.name == display.name; });
    }
    const auto unmatchedDisplays = std::ranges::count(matches, std::optional<std::size_t>{});
    const auto unusedMonitors = std::ranges::count(used, false);
    if (unmatchedDisplays == 1 && unusedMonitors == 1)
    {
        const auto display = static_cast<std::size_t>(std::ranges::find(matches, std::optional<std::size_t>{}) - matches.begin());
        const auto monitor = static_cast<std::size_t>(std::ranges::find(used, false) - used.begin());
        if (!monitors[monitor].hasDesktopRect)
        {
            matches[display] = monitor;
        }
    }
    return matches;
}

namespace
{

/// A monitor's physical size: "27.0" (597 × 336 mm)"; empty when unknown.
[[nodiscard]] std::string monitorSize(const Platform::Monitor& monitor)
{
    if (monitor.widthMm == 0 || monitor.heightMm == 0)
    {
        return {};
    }
    const double inches = std::hypot(static_cast<double>(monitor.widthMm), static_cast<double>(monitor.heightMm)) / 25.4;
    return std::format("{:.1f}\" ({} \xC3\x97 {} mm)", inches, monitor.widthMm, monitor.heightMm);
}

/// "HDR (BT.2020 PQ, 10-bit)" / "SDR (sRGB, 8-bit)"; empty without a colour space.
[[nodiscard]] std::string monitorColor(const Platform::Monitor& monitor)
{
    if (monitor.colorSpace.empty())
    {
        return {};
    }
    const std::string bits = monitor.bitsPerColor == 0 ? std::string{} : std::format("{}-bit", monitor.bitsPerColor);
    return std::format("{} ({})", monitor.hdr ? "HDR" : "SDR", joinNonEmpty({monitor.colorSpace, bits}, ", "));
}

void appendMonitorRows(std::string label, const Platform::Monitor& monitor, std::string value, std::vector<Row>& rows)
{
    rows.push_back(row(label, std::move(value)));
    rows.push_back(row(std::move(label) + " serial number", monitor.serial, "Not reported by this monitor", true));
}

} // namespace

std::string formatDisplay(const Core::DisplayInfo& display, const Platform::Monitor* monitor)
{
    const std::string_view name = monitor != nullptr && !monitor->name.empty() ? std::string_view{monitor->name} : display.name;
    std::string mode = display.pixelWidth > 0 ? std::format("{} \xC3\x97 {}", display.pixelWidth, display.pixelHeight) : std::string{};
    if (display.refreshHz > 0.0)
    {
        const bool whole = std::abs(display.refreshHz - std::round(display.refreshHz)) < 0.005;
        mode =
            joinNonEmpty({mode, whole ? std::format("{:.0f} Hz", display.refreshHz) : std::format("{:.2f} Hz", display.refreshHz)}, " at ");
    }
    const std::string scale = display.contentScale > 0.0F ? std::format("{:.0f}% scale", display.contentScale * 100.0F) : std::string{};
    const std::string size = monitor != nullptr ? monitorSize(*monitor) : std::string{};
    std::string color = monitor != nullptr ? monitorColor(*monitor) : std::string{};
    if (color.empty() && display.hdrEnabled)
    {
        color = "HDR";
    }
    return joinNonEmpty({name, mode, scale, size, color, display.primary ? "primary" : ""}, ", ");
}

Section buildGraphicsSection(const Platform::GraphicsInfo& graphics, const Core::GraphicsHostInfo& host)
{
    Section section{.title = "Graphics & displays", .icon = ICON_FA_TV, .rows = {}};
    auto& rows = section.rows;
    const bool windows = graphics.family == Platform::OsFamily::Windows;
    if (!graphics.adaptersRead || graphics.adapters.empty())
    {
        rows.push_back(row("GPU",
                           graphics.adaptersRead ? "None found" : "",
                           windows ? "A DXGI factory couldn't be created" : "/sys/class/drm couldn't be listed"));
    }
    for (std::size_t i = 0; i < graphics.adapters.size(); ++i)
    {
        const Platform::GraphicsAdapter& adapter = graphics.adapters[i];
        const std::string label = graphics.adapters.size() == 1 ? std::string("GPU") : std::format("GPU {}", i + 1);
        rows.push_back(row(label, formatAdapter(adapter)));
        rows.push_back(row(label + " driver",
                           formatAdapterDriver(adapter),
                           windows ? "No display device node matched this adapter" : "No kernel driver is bound to it"));
    }

    constexpr std::string_view NO_CONTEXT = "TaskSmack's OpenGL context isn't available";
    rows.push_back(row("OpenGL", host.glVersion, NO_CONTEXT));
    const std::string vendor = host.glVendor.empty() ? std::string{} : "(" + host.glVendor + ")";
    rows.push_back(row("OpenGL renderer", joinNonEmpty({host.glRenderer, vendor}, " "), NO_CONTEXT));
    if (graphics.family == Platform::OsFamily::Linux)
    {
        std::string server = graphics.displayServer;
        if (server == "Wayland" && host.videoDriver == "x11")
        {
            server += " (TaskSmack runs through XWayland)";
        }
        rows.push_back(
            row("Display server", std::move(server), "No graphical session: XDG_SESSION_TYPE, WAYLAND_DISPLAY and DISPLAY aren't set"));
    }

    const std::vector<std::optional<std::size_t>> matches = matchMonitors(host.displays, graphics.monitors);
    std::vector<bool> shown(graphics.monitors.size(), false);
    if (host.displays.empty())
    {
        rows.push_back(row("Displays", "", "SDL reported no displays"));
    }
    for (std::size_t d = 0; d < host.displays.size(); ++d)
    {
        const std::string label = std::format("Display {}", d + 1);
        const std::optional<std::size_t> match = matches[d];
        if (!match.has_value())
        {
            rows.push_back(row(label, formatDisplay(host.displays[d], nullptr)));
            continue;
        }
        const Platform::Monitor& monitor = graphics.monitors[*match];
        shown[*match] = true;
        appendMonitorRows(label, monitor, formatDisplay(host.displays[d], &monitor), rows);
    }
    for (std::size_t m = 0; m < graphics.monitors.size(); ++m)
    {
        if (shown[m])
        {
            continue;
        }
        const Platform::Monitor& monitor = graphics.monitors[m];
        const std::string label = monitor.connector.empty() ? std::format("Monitor {}", m + 1) : "Monitor " + monitor.connector;
        appendMonitorRows(label, monitor, joinNonEmpty({monitor.name, monitorSize(monitor), monitorColor(monitor)}, ", "), rows);
    }
    return section;
}

std::vector<Section> buildSystemInfoSections(const Domain::SystemInfoSnapshot& snapshot, const Core::GraphicsHostInfo& host)
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
    if (snapshot.storage.available)
    {
        sections.push_back(buildStorageSection(snapshot.storage));
    }
    if (snapshot.graphics.available)
    {
        sections.push_back(buildGraphicsSection(snapshot.graphics, host));
    }
    // Further sections (#1514 and on) follow here, in the page's order.
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
