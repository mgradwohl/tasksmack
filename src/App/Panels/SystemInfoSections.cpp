#include "SystemInfoSections.h"

#include "App/Panels/ServicesView.h"
#include "Core/GraphicsHostInfo.h"
#include "Domain/SensorModel.h"
#include "Domain/SensorTracker.h"
#include "Domain/SystemInfoModel.h"
#include "Platform/AsciiCase.h"
#include "Platform/IServiceProbe.h"
#include "Platform/ISystemInfoProbe.h"
#include "UI/Format.h"
#include "UI/IconsFontAwesome6.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <initializer_list>
#include <iterator>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
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
    return {
        .label = std::move(label),
        .value = std::move(value),
        .unavailableReason = std::move(why),
        .isIdentifier = identifier,
        .sensorKind = std::nullopt,
        .sensorKey = {},
        .action = RowAction::None,
        .valueTooltip = {},
        .redacted = std::nullopt,
    };
}

using Platform::containsIgnoreCase;

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

/// An audio endpoint's row label.
[[nodiscard]] constexpr std::string_view audioLabel(Platform::AudioFlow flow) noexcept
{
    switch (flow)
    {
    case Platform::AudioFlow::Output:
        return "Audio output";
    case Platform::AudioFlow::Input:
        return "Audio input";
    case Platform::AudioFlow::Unknown:
        break;
    }
    return "Audio device";
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

namespace
{

/// The "Authenticate to read" row a section with facts readable by root only leads with (#2310):
/// @p label and @p why it's offered.
[[nodiscard]] Row authenticateRow(std::string_view label = "Readable by root only",
                                  std::string_view why = "Some facts here need an administrator")
{
    Row ask = row(std::string(label),
                  {},
                  std::format("{}: \"Authenticate to read\" reads them through TaskSmack's helper ({})", why, AUTHENTICATE_NOTE));
    ask.action = RowAction::AuthenticateToRead;
    return ask;
}

} // namespace

Section buildFirmwareSection(const Platform::FirmwareInfo& firmware, bool canAuthenticate)
{
    Section section{.title = "Firmware & board", .icon = ICON_FA_MICROCHIP, .rows = {}};
    auto& rows = section.rows;
    if (canAuthenticate && (firmware.identifiersNeedAdmin || firmware.smbiosVersionNeedsAdmin))
    {
        rows.push_back(authenticateRow());
    }
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

Section buildMemorySection(const Platform::MemoryModulesInfo& memory, bool canAuthenticate)
{
    Section section{.title = "Memory modules", .icon = ICON_FA_MEMORY, .rows = {}};
    auto& rows = section.rows;
    if (canAuthenticate && memory.tableNeedsAdmin)
    {
        rows.push_back(authenticateRow());
    }
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
        text += " (" + UI::Format::formatFixedLocalized(
                           static_cast<double>(device.originalBytes) / static_cast<double>(device.compressedBytes), 1, ":1)");
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

std::string formatAtaHealth(const Platform::AtaHealth& health)
{
    std::string text = health.failing ? "Failing (the drive's SMART assessment predicts failure)" : "SMART OK";
    if (health.badSectors.has_value())
    {
        text += std::format(", {} bad sector{}", *health.badSectors, *health.badSectors == 1 ? "" : "s");
    }
    if (health.powerOnHours.has_value())
    {
        text += std::format(", {} power-on hours", *health.powerOnHours);
    }
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

std::string formatPartitionLayout(const Platform::PhysicalDisk& disk)
{
    if (!disk.partitionsRead)
    {
        return {};
    }
    if (disk.partitionStyle == Platform::PartitionStyle::Raw)
    {
        return "Not partitioned";
    }
    const std::size_t count = disk.partitions.size();
    const std::string partitions = UI::Format::formatCountWithLabel(count, count == 1 ? "partition" : "partitions");
    switch (disk.partitionStyle)
    {
    case Platform::PartitionStyle::Gpt:
        return "GPT, " + partitions;
    case Platform::PartitionStyle::Mbr:
        return "MBR, " + partitions;
    default:
        return count == 0 ? std::string("No partitions") : partitions;
    }
}

std::string formatPartition(const Platform::Partition& partition)
{
    std::string type = partition.typeName.empty() ? partition.typeId : partition.typeName;
    if (type.empty())
    {
        type = std::string(UNAVAILABLE_TEXT);
    }
    const std::string offset = "offset " + sizeText(partition.offsetBytes);
    std::string text = joinNonEmpty({partition.device, type, formatMemoryCapacity(partition.sizeBytes), offset}, ", ");
    if (!partition.mountPoint.empty())
    {
        text += " \xE2\x86\x92 " + partition.mountPoint; // U+2192, UTF-8
    }
    return text;
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
        if (disk.health.has_value() || disk.ataHealth.has_value() || !disk.healthUnavailableReason.empty())
        {
            std::string health;
            if (disk.health.has_value())
            {
                health = formatNvmeHealth(*disk.health);
            }
            else if (disk.ataHealth.has_value())
            {
                health = formatAtaHealth(*disk.ataHealth);
            }
            rows.push_back(row(disk.name + " health", health, disk.healthUnavailableReason));
        }
        // The layout (#1632): a summary row, then one row per partition, so a filter on the disk's name
        // keeps them together.
        if (disk.partitionsRead || !disk.partitionsUnavailableReason.empty())
        {
            rows.push_back(row(disk.name + " partitions", formatPartitionLayout(disk), disk.partitionsUnavailableReason));
        }
        for (const Platform::Partition& partition : disk.partitions)
        {
            rows.push_back(row(std::format("{} partition {}", disk.name, partition.number), formatPartition(partition)));
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
            ++other; // "Unknown" (Windows, #1781), the kernel's "Unknown: ...", or a status a newer kernel words differently
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

/// A monitor's physical size: "27.0" (597 × 336 mm)"; empty when unknown.
[[nodiscard]] std::string monitorSize(const Platform::Monitor& monitor)
{
    if (monitor.widthMm == 0 || monitor.heightMm == 0)
    {
        return {};
    }
    const double inches = std::hypot(static_cast<double>(monitor.widthMm), static_cast<double>(monitor.heightMm)) / 25.4;
    return std::format("{} ({} \xC3\x97 {} mm)", UI::Format::formatFixedLocalized(inches, 1, "\""), monitor.widthMm, monitor.heightMm);
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

/// A present TPM: "Present (TPM 2.0)", "Present (TPM 1.2)", or "Present" when the version is unknown.
[[nodiscard]] std::string_view tpmPresentText(std::uint32_t versionMajor)
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

std::string formatDisplay(const Core::DisplayInfo& display, const Platform::Monitor* monitor)
{
    const std::string_view name = monitor != nullptr && !monitor->name.empty() ? std::string_view{monitor->name} : display.name;
    std::string mode = display.pixelWidth > 0 ? std::format("{} \xC3\x97 {}", display.pixelWidth, display.pixelHeight) : std::string{};
    if (display.refreshHz > 0.0)
    {
        const bool whole = std::abs(display.refreshHz - std::round(display.refreshHz)) < 0.005;
        mode = joinNonEmpty(
            {mode, whole ? std::format("{:.0f} Hz", display.refreshHz) : UI::Format::formatFixedLocalized(display.refreshHz, 2, " Hz")},
            " at ");
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
    constexpr std::string_view NO_VULKAN_LOADER = "No Vulkan loader (vulkan-1.dll / libvulkan.so.1) is installed";
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
        if (windows)
        {
            const std::string level = adapter.d3dFeatureLevel.empty() ? std::string{} : "Feature level " + adapter.d3dFeatureLevel;
            rows.push_back(
                row("Direct3D on " + label, level, "Direct3D 12 isn't available: d3d12.dll is missing or no device could be created"));
        }
        if (graphics.vulkanRead)
        {
            const std::string driver = adapter.vulkanDriverVersion.empty() ? std::string{} : "(driver " + adapter.vulkanDriverVersion + ")";
            rows.push_back(row("Vulkan on " + label,
                               adapter.vulkanApiVersion.empty() ? std::string{} : joinNonEmpty({adapter.vulkanApiVersion, driver}, " "),
                               graphics.vulkanVersion.empty() ? NO_VULKAN_LOADER : "The Vulkan loader lists no device for this GPU"));
        }
    }

    constexpr std::string_view NO_CONTEXT = "TaskSmack's OpenGL context isn't available";
    rows.push_back(row("OpenGL", host.glVersion, NO_CONTEXT));
    const std::string vendor = host.glVendor.empty() ? std::string{} : "(" + host.glVendor + ")";
    rows.push_back(row("OpenGL renderer", joinNonEmpty({host.glRenderer, vendor}, " "), NO_CONTEXT));
    if (graphics.vulkanRead)
    {
        rows.push_back(row("Vulkan", graphics.vulkanVersion, NO_VULKAN_LOADER));
    }
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

std::string_view bitLockerStateText(Platform::BitLockerState state) noexcept
{
    using Platform::BitLockerState;
    switch (state)
    {
    case BitLockerState::On:
        return "On";
    case BitLockerState::Off:
        return "Off";
    case BitLockerState::Encrypting:
        return "Encrypting";
    case BitLockerState::Decrypting:
        return "Decrypting";
    case BitLockerState::Suspended:
        return "Suspended";
    case BitLockerState::Locked:
        return "Locked";
    case BitLockerState::NotEncryptable:
        return "Not encryptable";
    case BitLockerState::WaitingForActivation:
        return "Waiting for activation";
    case BitLockerState::Unknown:
        break;
    }
    return "Unknown";
}

namespace
{

/// One "BitLocker (C:)" row per fixed drive (#2584); one "BitLocker" row saying why when there are none.
void appendBitLockerRows(const Platform::PlatformSecurityInfo& security, std::vector<Row>& rows)
{
    if (!security.bitLockerRead || security.bitLocker.empty())
    {
        rows.push_back(row("BitLocker", {}, security.bitLockerRead ? "No fixed drives" : "Windows couldn't list the drives"));
        return;
    }
    for (const Platform::BitLockerVolume& volume : security.bitLocker)
    {
        Row bitLocker = row("BitLocker (" + volume.volume + ")", std::string(bitLockerStateText(volume.state)));
        if (volume.state == Platform::BitLockerState::Unknown)
        {
            bitLocker.valueTooltip = "Windows doesn't report BitLocker for this drive";
        }
        rows.push_back(std::move(bitLocker));
    }
}

/// One "Disk encryption (nvme0n1p3)" row per open dm-crypt volume (#2585), the Linux counterpart of the
/// BitLocker rows: "LUKS2: /, /home", or "LUKS2: not mounted". One "Disk encryption" row reading "None"
/// when there are none, or saying why it's unknown.
void appendDiskEncryptionRows(const Platform::PlatformSecurityInfo& security, std::vector<Row>& rows)
{
    if (!security.encryptedVolumesRead)
    {
        rows.push_back(row("Disk encryption", {}, "/sys/block couldn't be read"));
        return;
    }
    if (security.encryptedVolumes.empty())
    {
        Row none = row("Disk encryption", "None");
        none.valueTooltip = "No dm-crypt (LUKS or plain) volume is unlocked";
        rows.push_back(std::move(none));
        return;
    }
    for (const Platform::EncryptedVolume& volume : security.encryptedVolumes)
    {
        std::string mounts;
        for (const std::string& mountPoint : volume.mountPoints)
        {
            mounts += (mounts.empty() ? "" : ", ") + mountPoint;
        }
        Row encryption = row("Disk encryption (" + (volume.device.empty() ? volume.name : volume.device) + ")",
                             volume.type + ": " + (mounts.empty() ? std::string("not mounted") : mounts));
        if (!volume.name.empty())
        {
            encryption.valueTooltip = "Unlocked as /dev/mapper/" + volume.name;
        }
        rows.push_back(std::move(encryption));
    }
}

} // namespace

Section buildSecuritySection(const Platform::PlatformSecurityInfo& security)
{
    using Platform::SecurityFeatureState;
    Section section{.title = "Security", .icon = ICON_FA_LOCK, .rows = {}};
    auto& rows = section.rows;
    const bool windows = security.family == Platform::OsFamily::Windows;

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
    rows.push_back(
        row("Secure Boot",
            std::move(secureBoot),
            windows ? "The Secure Boot state isn't in the registry (SecureBoot\\State)" : "The SecureBoot EFI variable couldn't be read"));

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
    rows.push_back(
        row("TPM", std::move(tpm), windows ? "The TPM Base Services (tbs.dll) couldn't be asked" : "/sys/class/tpm couldn't be read"));

    if (windows)
    {
        rows.push_back(row("Memory integrity (HVCI)",
                           triStateText(security.memoryIntegrity, "On", "Off", ""),
                           "Windows didn't report its code integrity options"));
        rows.push_back(row("Kernel DMA Protection",
                           triStateText(security.kernelDmaProtection, "On", "Off", ""),
                           "Not reported by this version of Windows"));
        appendBitLockerRows(security, rows);
    }
    else
    {
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
        rows.push_back(row("Kernel lockdown",
                           std::move(lockdown),
                           "securityfs (/sys/kernel/security) isn't mounted, or the lockdown module isn't built in"));
        appendDiskEncryptionRows(security, rows);
    }

    if (!security.vulnerabilitiesRead)
    {
        rows.push_back(row("CPU vulnerabilities",
                           {},
                           windows ? "Windows didn't report its speculation control settings"
                                   : "/sys/devices/system/cpu/vulnerabilities couldn't be read"));
        return section;
    }
    rows.push_back(row("CPU vulnerabilities",
                       formatVulnerabilitySummary(security.vulnerabilities),
                       windows ? "Windows reports none" : "The kernel lists none"));
    for (const Platform::CpuVulnerability& item : security.vulnerabilities)
    {
        Row vulnerability = row(formatVulnerabilityName(item.name), item.status);
        if (windows && item.status == "Unknown")
        {
            vulnerability.valueTooltip = "Windows didn't report whether this CPU needs this mitigation";
        }
        rows.push_back(std::move(vulnerability));
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
        std::string text = UI::Format::formatFixedLocalized(reading.value, 1, " \u00B0C");
        std::string limits;
        if (reading.high.has_value())
        {
            limits = "high " + UI::Format::formatFixedLocalized(*reading.high, 1, " \u00B0C");
        }
        if (reading.critical.has_value())
        {
            limits += std::format(
                "{}critical {}", limits.empty() ? "" : ", ", UI::Format::formatFixedLocalized(*reading.critical, 1, " \u00B0C"));
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
        return UI::Format::formatFixedLocalized(reading.value, 2, " V");
    case SensorKind::Current:
        return UI::Format::formatFixedLocalized(reading.value, 2, " A");
    case SensorKind::Power:
        return UI::Format::formatFixedLocalized(reading.value, 1, " W");
    }
    return {};
}

std::string formatSensorRange(const Platform::SensorReading& reading, const Domain::SensorRange& range)
{
    if (range.samples < 2 || range.max <= range.min)
    {
        return {};
    }
    // In the reading's own unit and precision, without its thresholds.
    Platform::SensorReading bound = reading;
    bound.high.reset();
    bound.critical.reset();
    bound.value = range.min;
    const std::string min = formatSensorReading(bound);
    bound.value = range.max;
    return "min " + min + ", max " + formatSensorReading(bound);
}

Section buildSensorsSection(const Platform::SensorsInfo& sensors, const std::map<std::string, Domain::SensorRange>& ranges)
{
    Section section{.title = std::string(SENSORS_SECTION_TITLE), .icon = ICON_FA_TEMPERATURE_HALF, .rows = {}};
    for (const Platform::SensorDevice& device : sensors.devices)
    {
        for (const Platform::SensorReading& reading : device.readings)
        {
            std::string key = Domain::sensorKey(device, reading);
            std::string value = formatSensorReading(reading);
            // Since the page first read it, while it shows (#1522); kept by the sensor's identity, not its
            // label (#2340).
            if (const auto it = ranges.find(key); it != ranges.end())
            {
                if (const std::string range = formatSensorRange(reading, it->second); !range.empty())
                {
                    value += " \u00B7 " + range;
                }
            }
            Row item = row(std::format("{}: {}", device.name, reading.label), std::move(value));
            item.sensorKind = reading.kind; // its history is charted beside it (#1522)
            item.sensorKey = std::move(key);
            section.rows.push_back(std::move(item));
        }
    }
    const bool windows = sensors.family == Platform::OsFamily::Windows;
    if (section.rows.empty())
    {
        std::string_view reason;
        if (windows)
        {
            reason = sensors.listed ? "No ACPI thermal zones are exposed (common on desktops and in virtual machines)"
                                    : "The Thermal Zone Information performance counters couldn't be read";
        }
        else
        {
            reason = sensors.listed ? "No hwmon sensors or thermal zones are exposed (common in virtual machines, containers and WSL)"
                                    : "/sys/class/hwmon couldn't be read";
        }
        section.rows.push_back(row("Sensors", {}, reason));
    }
    if (windows)
    {
        section.rows.push_back(row("Other sensors",
                                   "Most motherboard sensors (fans, voltages, CPU and board temperatures) need a vendor driver; "
                                   "GPU temperatures are on the GPU tab"));
    }
    return section;
}

std::string formatDeviceName(const Platform::Device& device)
{
    const std::string_view vendorWord = std::string_view(device.vendor).substr(0, device.vendor.find(' '));
    const bool nameHasVendor = !vendorWord.empty() && device.name.size() >= vendorWord.size() &&
                               containsIgnoreCase(std::string_view(device.name).substr(0, vendorWord.size()), vendorWord);
    std::string text = joinNonEmpty({nameHasVendor ? std::string_view{} : std::string_view(device.vendor), device.name}, " ");
    if (text.empty())
    {
        text = "Unknown device";
    }
    else if (device.name.empty())
    {
        text += " device";
    }
    if (device.vendorId != 0)
    {
        text += std::format(" ({:04X}:{:04X})", device.vendorId, device.productId);
    }
    return text;
}

std::string formatAdapterSummary(const Platform::NetworkAdapter& adapter)
{
    std::string text = adapter.up ? "Up" : "Down";
    if (adapter.wireless)
    {
        text += ", Wi-Fi";
    }
    if (adapter.mtu != 0)
    {
        text += std::format(", MTU {}", adapter.mtu);
    }
    if (!adapter.driver.empty())
    {
        text += ", driver " + adapter.driver;
    }
    if (!adapter.description.empty())
    {
        text += ", " + adapter.description;
    }
    return text;
}

std::string formatUsbSpeed(double mbps)
{
    if (mbps <= 0.0)
    {
        return {};
    }
    if (mbps >= 1000.0)
    {
        return UI::Format::withLocaleDecimalPoint(std::format("{:g} Gbps", mbps / 1000.0));
    }
    return UI::Format::withLocaleDecimalPoint(std::format("{:g} Mbps", mbps));
}

Section buildDevicesSection(const Platform::DevicesInfo& devices)
{
    const bool windows = devices.family == Platform::OsFamily::Windows;
    Section section{.title = "Devices", .icon = ICON_FA_SITEMAP, .rows = {}};
    std::vector<Row>& rows = section.rows;
    const std::string_view listFailed = windows ? "SetupAPI couldn't list the devices" : "/sys/bus/pci/devices couldn't be listed";

    std::string problemCount = devices.problems.empty() ? "None" : std::format("{}", devices.problems.size());
    rows.push_back(row("Problem devices", devices.problemsRead ? std::move(problemCount) : std::string{}, listFailed));
    for (const Platform::Device& device : devices.problems)
    {
        rows.push_back(row("Problem device", std::format("{}: {}", formatDeviceName(device), device.problem)));
    }

    std::vector<const Platform::Device*> pci;
    pci.reserve(devices.pci.size());
    for (const Platform::Device& device : devices.pci)
    {
        pci.push_back(&device);
    }
    std::ranges::stable_sort(pci, {}, [](const Platform::Device* device) { return std::string_view(device->className); });
    for (const Platform::Device* device : pci)
    {
        const std::string location = device->location.empty() ? std::string{} : "PCI " + device->location;
        const std::string driver = device->driver.empty() ? std::string{} : "driver " + device->driver;
        rows.push_back(row(device->className.empty() ? "PCI device" : device->className,
                           joinNonEmpty({formatDeviceName(*device), location, driver, device->problem}, ", ")));
    }
    if (pci.empty())
    {
        rows.push_back(row("PCI devices", devices.pciRead ? "None found" : std::string{}, listFailed));
    }

    for (std::size_t i = 0; i < devices.usb.size(); ++i)
    {
        const Platform::Device& device = devices.usb[i];
        const bool hasParent = device.parent.has_value() && *device.parent < devices.usb.size();
        const std::string via = hasParent ? "via " + formatDeviceName(devices.usb[*device.parent]) : std::string{};
        const std::string label = std::format("USB {}", i + 1);
        rows.push_back(row(label, joinNonEmpty({formatDeviceName(device), formatUsbSpeed(device.speedMbps), via, device.problem}, ", ")));
        if (!device.serial.empty())
        {
            rows.push_back(row(label + " serial number", device.serial, NOT_REPORTED, true));
        }
    }
    if (devices.usb.empty())
    {
        rows.push_back(row("USB devices",
                           devices.usbRead ? "None found" : std::string{},
                           windows ? listFailed : "/sys/bus/usb/devices couldn't be listed"));
    }

    for (const Platform::AudioEndpoint& endpoint : devices.audio)
    {
        rows.push_back(
            row(std::string(audioLabel(endpoint.flow)), endpoint.name.empty() ? std::string("Unnamed endpoint") : endpoint.name));
    }
    if (devices.audio.empty())
    {
        rows.push_back(
            row("Audio",
                devices.audioRead ? "No active devices" : std::string{},
                windows ? "The Windows audio device API couldn't be used" : "/proc/asound/cards couldn't be read (no ALSA sound driver)"));
    }
    return section;
}

namespace
{

/// A Windows driver signature that is a problem, as a row says it; empty for a signed or unchecked image.
[[nodiscard]] std::string_view driverSignatureProblem(Platform::DriverSignature signature)
{
    switch (signature)
    {
    case Platform::DriverSignature::Unsigned:
        return "Unsigned";
    case Platform::DriverSignature::Untrusted:
        return "Untrusted signature";
    default:
        return {};
    }
}

/// The "Problem drivers" count and the "Problem driver" rows (#1661): drivers that failed to start, then
/// unsigned or untrusted images, each in name order.
void addDriverProblemRows(std::vector<Row>& rows, bool listed, std::span<const Platform::KernelDriver* const> sorted)
{
    std::vector<Row> problems;
    std::size_t problemDrivers = 0;
    for (const Platform::KernelDriver* driver : sorted)
    {
        if (!driver->startError.empty())
        {
            problems.push_back(row("Problem driver", std::format("Failed to start: {} ({})", driver->name, driver->startError)));
        }
    }
    for (const Platform::KernelDriver* driver : sorted)
    {
        const std::string_view signature = driverSignatureProblem(driver->signature);
        if (!signature.empty())
        {
            const std::string_view why = driver->signature == Platform::DriverSignature::Untrusted ? driver->signatureNote : driver->path;
            problems.push_back(
                row("Problem driver", std::format("{}: {}{}", signature, driver->name, why.empty() ? "" : std::format(" ({})", why))));
        }
        if (!driver->startError.empty() || !signature.empty())
        {
            ++problemDrivers;
        }
    }
    std::string count = problemDrivers == 0 ? "None" : std::format("{}", problemDrivers);
    rows.push_back(
        row("Problem drivers", listed ? std::move(count) : std::string{}, "The Service Control Manager couldn't list the drivers"));
    std::ranges::move(problems, std::back_inserter(rows));
}

} // namespace

std::string formatDriverValue(const Platform::KernelDriver& driver)
{
    if (!driver.moduleState.empty()) // a Linux kernel module
    {
        std::string uses;
        if (!driver.usedBy.empty())
        {
            uses = "used by";
            for (std::size_t i = 0; i < driver.usedBy.size(); ++i)
            {
                uses += (i == 0 ? " " : ", ") + driver.usedBy[i];
            }
        }
        else if (driver.useCount.has_value())
        {
            const std::uint32_t refs = *driver.useCount;
            uses = refs == 0 ? std::string("not in use") : std::format("{} {}", refs, refs == 1 ? "reference" : "references");
        }
        const std::string size = driver.sizeBytes > 0 ? UI::Format::formatBytes(static_cast<double>(driver.sizeBytes)) : std::string{};
        const std::string version = driver.version.empty() ? std::string{} : "version " + driver.version;
        const std::string tainted = driver.taints.empty() ? std::string{} : "tainted (" + driver.taints + ")";
        return joinNonEmpty(
            {
                size,
                uses,
                version,
                driver.moduleState == "Live" ? std::string_view{} : std::string_view(driver.moduleState),
                driver.permanent ? "permanent" : "",
                tainted,
            },
            ", ");
    }
    const bool sameName = Platform::equalsIgnoreCase(driver.displayName, driver.name);
    const std::string_view startType = serviceStartTypeLabel(driver.startType);
    const std::string start = startType.empty() ? std::string{} : std::string(startType) + " start";
    const bool running = driver.state == Platform::ServiceState::Running || driver.state == Platform::ServiceState::Unknown;
    const std::string failed = driver.startError.empty() ? std::string{} : "failed to start (" + driver.startError + ")";
    return joinNonEmpty(
        {
            sameName ? std::string_view{} : std::string_view(driver.displayName),
            driver.fileSystem ? "file system driver" : "",
            start,
            running ? std::string_view{} : serviceStateLabel(driver.state),
            failed,
            driverSignatureProblem(driver.signature),
            driver.version,
            driver.company,
            driver.path,
        },
        ", ");
}

Section buildDriversSection(const Platform::DriversInfo& drivers)
{
    const bool windows = drivers.family == Platform::OsFamily::Windows;
    Section section{.title = windows ? "Drivers" : "Kernel modules", .icon = ICON_FA_GEARS, .rows = {}};
    std::vector<Row>& rows = section.rows;
    // A Windows driver that failed to start is listed, but isn't loaded.
    const auto loaded =
        std::ranges::count_if(drivers.drivers, [](const Platform::KernelDriver& driver) { return driver.startError.empty(); });
    std::string count = loaded == 0 ? "None found" : std::format("{}", loaded);
    rows.push_back(row(windows ? "Loaded drivers" : "Loaded modules",
                       drivers.listed ? std::move(count) : std::string{},
                       windows ? "The Service Control Manager couldn't list the drivers"
                               : "/proc/modules couldn't be read (a kernel without loadable module support)"));
    if (!windows)
    {
        const auto tainted =
            std::ranges::count_if(drivers.drivers, [](const Platform::KernelDriver& module) { return !module.taints.empty(); });
        if (tainted > 0)
        {
            rows.push_back(row("Tainted modules", std::format("{}", tainted)));
        }
        rows.push_back(row("Built-in modules", "Not listed: built into the kernel, they aren't in /proc/modules"));
    }
    std::vector<const Platform::KernelDriver*> sorted;
    sorted.reserve(drivers.drivers.size());
    for (const Platform::KernelDriver& driver : drivers.drivers)
    {
        sorted.push_back(&driver);
    }
    std::ranges::stable_sort(sorted,
                             [](const Platform::KernelDriver* a, const Platform::KernelDriver* b)
                             { return Platform::compareIgnoreCase(a->name, b->name) < 0; });
    if (windows)
    {
        addDriverProblemRows(rows, drivers.listed, sorted);
    }
    for (const Platform::KernelDriver* driver : sorted)
    {
        const std::string label = driver->name.empty() ? "Unnamed driver" : driver->name;
        rows.push_back(row(label, formatDriverValue(*driver)));
        if (driver->signature == Platform::DriverSignature::Unknown)
        {
            rows.push_back(row(label + " signature", {}, "The signature couldn't be checked: " + driver->signatureNote));
        }
    }
    return section;
}

std::string_view exceptionCodeName(std::string_view code)
{
    static constexpr std::array<std::pair<std::string_view, std::string_view>, 10> NAMES{
        {
            {"0xC0000005", "access violation"},
            {"0xC0000409", "stack buffer overrun / fail fast"},
            {"0xC00000FD", "stack overflow"},
            {"0xC0000374", "heap corruption"},
            {"0x80000003", "breakpoint"},
            {"0xC000001D", "illegal instruction"},
            {"0xC0000094", "integer divide by zero"},
            {"0xC0000420", "assertion failure"},
            {"0xE0434352", ".NET exception"},
            {"0xE06D7363", "C++ exception"},
        },
    };
    for (const auto& [hex, name] : NAMES)
    {
        if (hex == code)
        {
            return name;
        }
    }
    return {};
}

std::string formatCrashValue(const Platform::CrashEvent& event)
{
    const std::string app =
        joinNonEmpty({event.application.empty() ? std::string_view("Unknown application") : event.application, event.appVersion}, " ");
    const std::string pid = event.pid.has_value() ? std::format("pid {}", *event.pid) : std::string{};
    const bool coreKept = event.coreKept || event.coreBytes > 0;
    if (event.uid.has_value() || coreKept || !event.signal.empty()) // a Linux core dump
    {
        // "python3.12 dumped core (SIGSEGV), /usr/bin/python3.12, pid 4242, uid 1000, 3.1 MiB", or
        // "... crashed (SIGABRT), ..., core not kept" when the journal recorded a crash without its core (#1674).
        const std::string what = coreKept ? app + " dumped core" : app + " crashed";
        const std::string headline = event.signal.empty() ? what : std::format("{} ({})", what, event.signal);
        const std::string uid = event.uid.has_value() ? std::format("uid {}", *event.uid) : std::string{};
        std::string core = "core not kept";
        if (coreKept)
        {
            core = event.coreBytes > 0 ? UI::Format::formatBytes(static_cast<double>(event.coreBytes)) : std::string{};
        }
        return joinNonEmpty({headline, event.executable, pid, uid, core}, ", ");
    }
    if (event.hang)
    {
        return joinNonEmpty({event.hangType.empty() ? app + " hung" : std::format("{} hung ({})", app, event.hangType), pid}, ", ");
    }
    const std::string module = joinNonEmpty({event.module, event.moduleVersion}, " ");
    const std::string_view name = exceptionCodeName(event.exceptionCode);
    std::string exception;
    if (!event.exceptionCode.empty())
    {
        exception = name.empty() ? "exception " + event.exceptionCode : std::format("exception {} ({})", event.exceptionCode, name);
    }
    return joinNonEmpty({module.empty() ? app + " crashed" : std::format("{} crashed in {}", app, module), exception, pid}, ", ");
}

Section buildCrashesSection(const Platform::CrashesInfo& crashes, bool canAuthenticate)
{
    const bool windows = crashes.family == Platform::OsFamily::Windows;
    Section section{.title = windows ? "Recent crashes & hangs" : "Recent crashes", .icon = ICON_FA_TRIANGLE_EXCLAMATION, .rows = {}};
    std::vector<Row>& rows = section.rows;
    if (canAuthenticate && crashes.otherUsersHidden)
    {
        rows.push_back(authenticateRow("Other users' crashes",
                                       "The journal shows you only your own crashes' details, and other users' core files by name"));
    }
    const std::string period = std::format("Last {} days", Platform::CRASH_HISTORY_DAYS);
    const auto plural = [](std::size_t count, std::string_view one, std::string_view many)
    {
        return std::format("{} {}", count, count == 1 ? one : many);
    };
    std::string count;
    if (crashes.listed)
    {
        const auto hangs = static_cast<std::size_t>(std::ranges::count_if(crashes.events, &Platform::CrashEvent::hang));
        const std::size_t crashed = crashes.events.size() - hangs;
        if (crashes.events.empty())
        {
            if (windows)
            {
                count = "No crashes or hangs";
            }
            else
            {
                count = crashes.journalRead ? "No crashes" : "No core dumps";
            }
        }
        else
        {
            // With the journal read, a crash may be one whose core wasn't kept (#1697).
            if (windows)
            {
                count = std::format("{}, {}", plural(crashed, "crash", "crashes"), plural(hangs, "hang", "hangs"));
            }
            else
            {
                count = crashes.journalRead ? plural(crashed, "crash", "crashes") : plural(crashed, "core dump", "core dumps");
            }
        }
    }
    rows.push_back(
        row(period, std::move(count), crashes.unavailableReason.empty() ? std::string_view(NOT_REPORTED) : crashes.unavailableReason));
    if (crashes.capped)
    {
        rows.push_back(row("Older", "", std::format("Not listed: only the {} newest are shown", Platform::CRASH_LIST_MAX)));
    }
    if (!windows && crashes.listed)
    {
        // What was read, and why the journal's details are missing when they are (#1697).
        std::string source = "systemd-coredump's journal entries and core files";
        if (!crashes.journalRead)
        {
            source = crashes.journalUnavailableReason.empty()
                       ? "systemd-coredump's core files; the journal's details (signal, executable) weren't read"
                       : "systemd-coredump's core files; the journal's details weren't read: " + crashes.journalUnavailableReason;
        }
        rows.push_back(row("Source", std::move(source)));
    }
    for (const Platform::CrashEvent& event : crashes.events)
    {
        std::string when = UI::Format::formatEpochDateTime(event.unixSeconds);
        rows.push_back(row(when.empty() ? std::string("Unknown time") : std::move(when), formatCrashValue(event)));
    }
    return section;
}

namespace
{

/// "192.168.1.20" as its four bytes; nullopt for anything else.
[[nodiscard]] std::optional<std::array<std::uint8_t, 4>> parseIpv4(std::string_view text)
{
    std::array<std::uint8_t, 4> bytes{};
    for (std::size_t i = 0; i < bytes.size(); ++i)
    {
        const bool last = i + 1 == bytes.size();
        const std::size_t dot = text.find('.');
        if (last != (dot == std::string_view::npos))
        {
            return std::nullopt;
        }
        const std::string_view part = last ? text : text.substr(0, dot);
        unsigned value = 0;
        const auto [stop, error] = std::from_chars(part.data(), part.data() + part.size(), value);
        if (part.empty() || part.size() > 3 || error != std::errc{} || stop != part.data() + part.size() || value > 255U)
        {
            return std::nullopt;
        }
        bytes.at(i) = static_cast<std::uint8_t>(value);
        if (!last)
        {
            text.remove_prefix(dot + 1);
        }
    }
    return bytes;
}

/// The colon-separated groups of one side of an IPv6 address's "::" onto @p groups; the last may be an
/// embedded IPv4 address ("::ffff:192.168.1.20") when @p lastMayBeIpv4. False when they don't parse.
[[nodiscard]] bool parseIpv6Groups(std::string_view text, bool lastMayBeIpv4, std::vector<std::uint16_t>& groups)
{
    while (!text.empty())
    {
        const std::size_t colon = text.find(':');
        const std::string_view group = text.substr(0, colon);
        if (group.contains('.'))
        {
            const std::optional<std::array<std::uint8_t, 4>> v4 = parseIpv4(group);
            if (!lastMayBeIpv4 || colon != std::string_view::npos || !v4)
            {
                return false;
            }
            groups.push_back(static_cast<std::uint16_t>((v4->at(0) << 8U) | v4->at(1)));
            groups.push_back(static_cast<std::uint16_t>((v4->at(2) << 8U) | v4->at(3)));
            return true;
        }
        std::uint16_t value = 0;
        const auto [stop, error] = std::from_chars(group.data(), group.data() + group.size(), value, 16);
        if (group.empty() || group.size() > 4 || error != std::errc{} || stop != group.data() + group.size())
        {
            return false;
        }
        groups.push_back(value);
        if (colon == std::string_view::npos)
        {
            return true;
        }
        text.remove_prefix(colon + 1);
        if (text.empty())
        {
            return false; // a trailing single colon
        }
    }
    return true;
}

/// "fe80::1c2a:3bff:fe4d:5e6f" (a "%zone" ignored) as its eight groups; nullopt for anything else.
[[nodiscard]] std::optional<std::array<std::uint16_t, 8>> parseIpv6(std::string_view text)
{
    if (const std::size_t zone = text.find('%'); zone != std::string_view::npos)
    {
        text = text.substr(0, zone);
    }
    std::vector<std::uint16_t> head;
    std::vector<std::uint16_t> tail;
    const std::size_t gap = text.find("::");
    if (gap == std::string_view::npos)
    {
        if (!parseIpv6Groups(text, true, head) || head.size() != 8)
        {
            return std::nullopt;
        }
    }
    else if (text.find("::", gap + 1) != std::string_view::npos || !parseIpv6Groups(text.substr(0, gap), false, head) ||
             !parseIpv6Groups(text.substr(gap + 2), true, tail) || head.size() + tail.size() > 7)
    {
        return std::nullopt;
    }
    std::array<std::uint16_t, 8> groups{};
    std::ranges::copy(head, groups.begin());
    std::ranges::copy(tail, groups.end() - static_cast<std::ptrdiff_t>(tail.size()));
    return groups;
}

/// 0/8, 10/8, 127/8, 100.64/10 (carrier-grade NAT), 169.254/16 (link-local), 172.16/12 and 192.168/16.
[[nodiscard]] bool isLocalIpv4(std::uint8_t first, std::uint8_t second)
{
    return first == 0 || first == 10 || first == 127 || (first == 100 && (second & 0xC0U) == 64U) || (first == 169 && second == 254) ||
           (first == 172 && (second & 0xF0U) == 16U) || (first == 192 && second == 168);
}

} // namespace

bool isLocalAddress(const Platform::AdapterAddress& address)
{
    if (!address.v6)
    {
        const std::optional<std::array<std::uint8_t, 4>> bytes = parseIpv4(address.address);
        return bytes && isLocalIpv4(bytes->at(0), bytes->at(1));
    }
    const std::optional<std::array<std::uint16_t, 8>> parsed = parseIpv6(address.address);
    if (!parsed)
    {
        return false; // unreadable: kept with the identifiers, the side that leaks nothing
    }
    const std::array<std::uint16_t, 8>& groups = *parsed;
    const bool zeroPrefix = std::ranges::all_of(std::span(groups).first<5>(), [](std::uint16_t group) { return group == 0; });
    if (zeroPrefix && groups[5] == 0 && groups[6] == 0 && groups[7] <= 1)
    {
        return true; // :: and ::1
    }
    if (zeroPrefix && groups[5] == 0xFFFFU)
    {
        // An IPv4-mapped address is its IPv4 address.
        return isLocalIpv4(static_cast<std::uint8_t>(groups[6] >> 8U), static_cast<std::uint8_t>(groups[6] & 0xFFU));
    }
    // fe80::/10 link-local, fec0::/10 (the deprecated site-local) and fc00::/7 unique local.
    return (groups[0] & 0xFF80U) == 0xFE80U || (groups[0] & 0xFE00U) == 0xFC00U;
}

namespace
{

/// isLocalAddress() for an address alone, as the gateway and DHCP server fields hold it: one with a colon is IPv6.
[[nodiscard]] bool isLocalAddressText(const std::string& address)
{
    return isLocalAddress({.address = address, .prefix = 0, .v6 = address.contains(':')});
}

} // namespace

std::string formatAdapterAddresses(const Platform::NetworkAdapter& adapter, AddressScope scope)
{
    std::string text;
    for (const bool v6 : {false, true})
    {
        for (const Platform::AdapterAddress& address : adapter.addresses)
        {
            if (address.v6 != v6 || isLocalAddress(address) != (scope == AddressScope::Local))
            {
                continue;
            }
            if (!text.empty())
            {
                text += ", ";
            }
            text += std::format("{}/{}", address.address, address.prefix);
        }
    }
    return text;
}

std::string formatWifiSignal(const Platform::NetworkAdapter& adapter)
{
    const std::optional<std::uint32_t> quality = adapter.wifiLink.has_value() ? adapter.wifiLink->signalQuality : std::nullopt;
    if (adapter.wifiSignalDbm.has_value() && quality.has_value())
    {
        return std::format("{} dBm ({}%)", *adapter.wifiSignalDbm, *quality);
    }
    if (adapter.wifiSignalDbm.has_value())
    {
        return std::format("{} dBm", *adapter.wifiSignalDbm);
    }
    return quality.has_value() ? std::format("{}%", *quality) : std::string{};
}

std::string formatWifiLink(const Platform::WifiLink& link)
{
    std::string text;
    const auto append = [&text](std::string_view part)
    {
        if (!text.empty())
        {
            text += ", ";
        }
        text += part;
    };
    if (!link.phy.empty())
    {
        append(link.phy);
    }
    const std::uint32_t mhz = link.frequencyMhz;
    // The channel numbering of IEEE 802.11: 2.4 GHz from 2407 MHz (channel 14 apart, at 2484), 5 GHz
    // from 5000 MHz, 6 GHz from 5950 MHz (channel 2 apart, at 5935), 5 MHz a channel.
    if (mhz == 2484)
    {
        append("2.4 GHz, channel 14");
    }
    else if (mhz >= 2412 && mhz <= 2472)
    {
        append(std::format("2.4 GHz, channel {}", (mhz - 2407) / 5));
    }
    else if (mhz >= 5150 && mhz <= 5895)
    {
        append(std::format("5 GHz, channel {}", (mhz - 5000) / 5));
    }
    else if (mhz == 5935)
    {
        append("6 GHz, channel 2");
    }
    else if (mhz >= 5955 && mhz <= 7115)
    {
        append(std::format("6 GHz, channel {}", (mhz - 5950) / 5));
    }
    else if (mhz != 0)
    {
        append(std::format("{} MHz", mhz));
    }
    else if (link.channel != 0)
    {
        append(std::format("channel {}", link.channel)); // Windows, when the frequency couldn't be read
    }
    const auto rate = [](std::uint32_t kbps)
    {
        return UI::Format::formatFixedLocalized(static_cast<double>(kbps) / 1000.0, 1, " Mbit/s");
    };
    if (link.rxBitrateKbps == 0)
    {
        if (link.bitrateKbps != 0)
        {
            append(rate(link.bitrateKbps));
        }
    }
    else
    {
        // Windows reports both directions; Linux only the transmit rate, shown alone as before.
        if (link.bitrateKbps != 0)
        {
            append(rate(link.bitrateKbps) + " transmit");
        }
        append(rate(link.rxBitrateKbps) + " receive");
    }
    return text;
}

std::string formatDhcpLease(const Platform::DhcpLease& lease)
{
    std::string text;
    const auto append = [&text](std::string_view part)
    {
        if (!text.empty())
        {
            text += ", ";
        }
        text += part;
    };
    if (!lease.server.empty())
    {
        append("From " + lease.server);
    }
    if (lease.leaseSeconds != 0)
    {
        constexpr std::uint32_t MINUTE = 60;
        constexpr std::uint32_t HOUR = 60 * MINUTE;
        constexpr std::uint32_t DAY = 24 * HOUR;
        const std::uint32_t seconds = lease.leaseSeconds;
        std::string length;
        if (seconds >= DAY)
        {
            const std::uint32_t days = seconds / DAY;
            const std::uint32_t hours = (seconds % DAY) / HOUR;
            length = std::format("{} {}", days, days == 1 ? "day" : "days");
            if (hours != 0)
            {
                length += std::format(" {} h", hours);
            }
        }
        else if (seconds >= HOUR)
        {
            const std::uint32_t minutes = (seconds % HOUR) / MINUTE;
            length = minutes != 0 ? std::format("{} h {} min", seconds / HOUR, minutes) : std::format("{} h", seconds / HOUR);
        }
        else if (seconds >= MINUTE)
        {
            length = std::format("{} min", seconds / MINUTE);
        }
        else
        {
            length = std::format("{} s", seconds);
        }
        append(length + " lease");
    }
    if (lease.expiresUnix != 0)
    {
        if (const std::string when = UI::Format::formatEpochDateTime(lease.expiresUnix); !when.empty())
        {
            append("expires " + when);
        }
    }
    return text;
}

Section buildNetworkAdaptersSection(const Platform::NetworkAdaptersInfo& network, bool wifiDetailsGated)
{
    Section section{.title = "Network adapters", .icon = ICON_FA_ETHERNET, .rows = {}};
    auto& rows = section.rows;
    const auto gateway = [](const std::string& address, const std::string& adapter)
    {
        return address.empty() ? std::string{} : std::format("{} ({})", address, adapter);
    };
    // A public gateway is in the machine's own public subnet (a VPS's x.y.z.1, or a global IPv6 prefix), so it
    // places the machine about as well as its own address: an identifier, like the public addresses (#2191).
    const auto isPublic = [](const std::string& address)
    {
        return !address.empty() && !isLocalAddressText(address);
    };
    rows.push_back(
        row("Default gateway", gateway(network.gatewayV4, network.gatewayV4Adapter), "No IPv4 default route", isPublic(network.gatewayV4)));
    if (!network.gatewayV6.empty())
    {
        rows.push_back(
            row("IPv6 gateway", gateway(network.gatewayV6, network.gatewayV6Adapter), NOT_REPORTED, isPublic(network.gatewayV6)));
    }
    // The DNS servers stay visible: usually a public resolver (1.1.1.1, 8.8.8.8) or the local router, which say
    // nothing about where the machine is (#2191).
    std::string dns;
    for (const std::string& server : network.dnsServers)
    {
        dns += (dns.empty() ? "" : ", ") + server;
    }
    if (network.dnsIsLocalStub)
    {
        dns += " (systemd-resolved; its upstream servers couldn't be read)";
    }
    rows.push_back(row("DNS servers", std::move(dns), network.dnsRead ? "None configured" : "The resolver configuration couldn't be read"));
    if (!network.searchDomains.empty())
    {
        std::string domains;
        for (const std::string& domain : network.searchDomains)
        {
            domains += (domains.empty() ? "" : ", ") + domain;
        }
        // A search domain can name the organisation, like a domain name in the Operating system section.
        rows.push_back(row("Search domains", std::move(domains), NOT_REPORTED, true));
    }

    if (network.adapters.empty())
    {
        rows.push_back(
            row("Adapters",
                network.listed ? "None found" : "",
                network.family == Platform::OsFamily::Windows ? "GetAdaptersAddresses failed" : "/sys/class/net couldn't be listed"));
        return section;
    }
    for (const Platform::NetworkAdapter& adapter : network.adapters)
    {
        rows.push_back(row(adapter.name, formatAdapterSummary(adapter)));
        // A globally routable address can place the machine, like the public IP: on IPv6 without NAT it is the
        // public address, and an EUI-64 one embeds the MAC. So those are an identifier row of their own (#2119).
        std::string local = formatAdapterAddresses(adapter, AddressScope::Local);
        std::string global = formatAdapterAddresses(adapter, AddressScope::Public);
        if (!local.empty() || global.empty())
        {
            rows.push_back(row(adapter.name + " addresses", std::move(local), "No IP address assigned"));
        }
        if (!global.empty())
        {
            rows.push_back(row(adapter.name + " public addresses", std::move(global), NOT_REPORTED, true));
        }
        if (adapter.dhcpLease && isPublic(adapter.dhcpLease->server))
        {
            // A public DHCP server is the provider's, in or near the machine's subnet: the lease's length and
            // expiry stay a plain row, and the server is an identifier row of its own (#2191).
            Platform::DhcpLease lease = *adapter.dhcpLease;
            lease.server.clear();
            if (std::string rest = formatDhcpLease(lease); !rest.empty())
            {
                rows.push_back(row(adapter.name + " DHCP lease", std::move(rest)));
            }
            rows.push_back(row(adapter.name + " DHCP server", adapter.dhcpLease->server, NOT_REPORTED, true));
        }
        else if (adapter.dhcpLease)
        {
            rows.push_back(row(adapter.name + " DHCP lease", formatDhcpLease(*adapter.dhcpLease)));
        }
        else if (adapter.dhcpEnabled.has_value())
        {
            // Windows says whether the adapter asks DHCP at all, so the one row can say why there is no lease.
            rows.push_back(row(adapter.name + " DHCP lease",
                               *adapter.dhcpEnabled ? std::string{} : std::string("None: a static IPv4 address"),
                               "DHCP is on, but no lease is held (not connected)"));
        }
        // Its own DNS settings, only where it has any: otherwise the system-wide servers above apply.
        const auto joined = [](const std::vector<std::string>& items, const auto& keep)
        {
            std::string text;
            for (const std::string& item : items)
            {
                if (keep(item))
                {
                    text += (text.empty() ? "" : ", ") + item;
                }
            }
            return text;
        };
        const auto all = [](const std::string& /*item*/)
        {
            return true;
        };
        if (!adapter.linkDnsServers.empty())
        {
            rows.push_back(row(adapter.name + " DNS servers", joined(adapter.linkDnsServers, all)));
        }
        if (!adapter.linkSearchDomains.empty())
        {
            // Like the system-wide search domains, these can name the organisation: an identifier.
            rows.push_back(row(adapter.name + " search domains", joined(adapter.linkSearchDomains, all), NOT_REPORTED, true));
        }
        if (!adapter.mac.empty())
        {
            rows.push_back(row(adapter.name + " MAC address", adapter.mac, NOT_REPORTED, true));
        }
        // Windows only so far: Linux leaves these empty, so their rows are left out there.
        // Split like the addresses: local gateways a plain row, public ones an identifier row (#2191).
        std::string localGateways = joined(adapter.gateways, isLocalAddressText);
        std::string publicGateways = joined(adapter.gateways, isPublic);
        if (!localGateways.empty())
        {
            rows.push_back(row(adapter.name + " gateways", std::move(localGateways)));
        }
        if (!publicGateways.empty())
        {
            rows.push_back(row(adapter.name + " public gateways", std::move(publicGateways), NOT_REPORTED, true));
        }
        if (adapter.wireless)
        {
            rows.push_back(row(adapter.name + " Wi-Fi signal", formatWifiSignal(adapter), "Not reported by the driver, or not connected"));
            if (wifiDetailsGated && adapter.wifiDetailsWithheld)
            {
                // Reading the network's name can raise a Windows location prompt, so it waits for a click
                // (#1782). Not an identifier: the row names nothing, and its button must stay visible.
                Row details = row(adapter.name + " Wi-Fi details",
                                  {},
                                  std::format("Not read yet: \"Show Wi-Fi details\" reads the network name, signal quality, "
                                              "standard, band and rates ({})",
                                              WIFI_DETAILS_NOTE));
                details.action = RowAction::ShowWifiDetails;
                rows.push_back(std::move(details));
            }
            else
            {
                // The network's name can place the machine, like a MAC address: an identifier.
                const std::string_view linkReason = adapter.wifiLinkUnavailable.empty() ? NOT_REPORTED : adapter.wifiLinkUnavailable;
                rows.push_back(
                    row(adapter.name + " Wi-Fi network", adapter.wifiLink ? adapter.wifiLink->ssid : std::string{}, linkReason, true));
            }
            if (adapter.wifiLink)
            {
                rows.push_back(row(adapter.name + " Wi-Fi link", formatWifiLink(*adapter.wifiLink)));
            }
        }
    }
    return section;
}

std::string formatBootDuration(std::uint64_t microseconds)
{
    constexpr std::uint64_t MS = 1000;
    constexpr std::uint64_t SECOND = 1000 * MS;
    constexpr std::uint64_t MINUTE = 60 * SECOND;
    if (microseconds < SECOND)
    {
        return std::format("{} ms", microseconds / MS);
    }
    if (microseconds < MINUTE)
    {
        return UI::Format::formatFixedLocalized(static_cast<double>(microseconds) / static_cast<double>(SECOND), 2, " s");
    }
    return std::format("{} min {}",
                       microseconds / MINUTE,
                       UI::Format::formatFixedLocalized(static_cast<double>(microseconds % MINUTE) / static_cast<double>(SECOND), 1, " s"));
}

Section buildBootPerformanceSection(const Platform::BootPerformanceInfo& boot)
{
    Section section{.title = "Boot performance", .icon = ICON_FA_POWER_OFF, .rows = {}};
    auto& rows = section.rows;
    if (!boot.timingsRead)
    {
        rows.push_back(row("Last boot", {}, boot.unavailableReason.empty() ? std::string(NOT_REPORTED) : boot.unavailableReason));
        return section;
    }
    const auto duration = [](const std::optional<std::uint64_t>& microseconds)
    {
        return microseconds.has_value() ? formatBootDuration(*microseconds) : std::string{};
    };
    if (boot.family == Platform::OsFamily::Windows)
    {
        rows.push_back(row("Last boot", duration(boot.totalUs)));
        rows.push_back(row("Recorded", UI::Format::formatEpochDateTime(boot.recordedUnixSeconds)));
        rows.push_back(row("Main path (to the desktop)", duration(boot.mainPathUs)));
        rows.push_back(row("Post-boot (desktop to idle)", duration(boot.postBootUs)));
        return section;
    }
    if (!boot.finished)
    {
        rows.push_back(row("Last boot", "Still starting up"));
    }
    else
    {
        rows.push_back(row("Last boot", boot.totalUs.has_value() ? formatBootDuration(*boot.totalUs) : std::string{}));
    }
    const auto phase = [&rows](const char* label, const std::optional<std::uint64_t>& microseconds, std::string_view reason)
    {
        rows.push_back(row(label, microseconds.has_value() ? formatBootDuration(*microseconds) : std::string{}, reason));
    };
    constexpr std::string_view NOT_MEASURED_BY_LOADER =
        "Not measured: the boot loader didn't record it (systemd-boot does), or not booted with UEFI";
    phase("Firmware", boot.firmwareUs, NOT_MEASURED_BY_LOADER);
    phase("Boot loader", boot.loaderUs, NOT_MEASURED_BY_LOADER);
    phase("Kernel", boot.kernelUs, NOT_REPORTED);
    if (boot.initrdUs.has_value())
    {
        phase("Initrd", boot.initrdUs, NOT_REPORTED); // left out entirely without an initramfs
    }
    phase("Userspace", boot.userspaceUs, boot.finished ? NOT_REPORTED : std::string_view("Startup hasn't finished yet"));
    return section;
}

std::vector<Row> buildPublicIpRows(bool hasPublicIp, bool included, const Platform::PublicIpInfo& publicIp)
{
    std::vector<Row> rows;
    if (!hasPublicIp)
    {
        return rows;
    }
    if (!included)
    {
        // Looking the addresses up sends a query off the machine, so it waits for a click. Not an
        // identifier: the row names nothing, and its button must stay visible.
        Row ask = row("Public IP",
                      {},
                      std::format("Not looked up: \"Show public IP\" asks OpenDNS's resolver what address this machine's "
                                  "queries come from ({})",
                                  PUBLIC_IP_NOTE));
        ask.action = RowAction::ShowPublicIp;
        rows.push_back(std::move(ask));
        return rows;
    }
    // A public address can place the machine: an identifier, like a MAC address.
    const auto address = [&](const char* label, const std::string& value, const std::string& unavailable)
    {
        Row line = row(label, value, unavailable.empty() ? NOT_REPORTED : std::string_view(unavailable), true);
        if (line.available() && !publicIp.source.empty())
        {
            line.valueTooltip = std::format("As seen by {}", publicIp.source);
        }
        return line;
    };
    rows.push_back(address("Public IPv4", publicIp.v4, publicIp.v4Unavailable));
    rows.push_back(address("Public IPv6", publicIp.v6, publicIp.v6Unavailable));
    return rows;
}

namespace
{

[[nodiscard]] constexpr bool isPathSeparator(char c) noexcept
{
    return c == '/' || c == '\\';
}

/// Whether a path component that started earlier ends at @p at: at a separator, the end of the text,
/// whitespace or punctuation that ends a path in a row's text ("..., pid 42", "(/home/alice)").
[[nodiscard]] bool endsComponent(std::string_view text, std::size_t at) noexcept
{
    constexpr std::string_view ENDS = " \t\r\n,;:)]}\"'";
    return at >= text.size() || isPathSeparator(text[at]) || ENDS.contains(text[at]);
}

[[nodiscard]] bool namesEqual(std::string_view a, std::string_view b, bool ignoreCase) noexcept
{
    return ignoreCase ? Platform::equalsIgnoreCase(a, b) : a == b;
}

/// Whether the component that ends at the separator @p separator is "Users" (Windows' profile folders).
[[nodiscard]] bool followsUsersFolder(std::string_view text, std::size_t separator) noexcept
{
    constexpr std::string_view USERS = "users";
    if (separator < USERS.size())
    {
        return false;
    }
    const std::size_t start = separator - USERS.size();
    // A view built from the pointer, not substr(), which may throw: in range, as checked above.
    return namesEqual(std::string_view(text.data() + start, USERS.size()), USERS, true) && (start == 0 || isPathSeparator(text[start - 1]));
}

/// The length of the path component at @p at when it names @p login, else 0.
[[nodiscard]] std::size_t loginComponentLength(std::string_view text, std::size_t at, std::string_view login, bool windows) noexcept
{
    if (at == 0 || !isPathSeparator(text[at - 1]) || text.size() - at < login.size() ||
        !namesEqual(std::string_view(text.data() + at, login.size()), login, windows))
    {
        return 0;
    }
    std::size_t end = at + login.size();
    if (endsComponent(text, end))
    {
        return login.size();
    }
    // Windows names a profile folder "<login>.<DOMAIN>" or "<login>.000" when the plain name is taken.
    if (windows && text[end] == '.' && followsUsersFolder(text, at - 1))
    {
        while (!endsComponent(text, end))
        {
            ++end;
        }
        return end - at;
    }
    return 0;
}

[[nodiscard]] std::string redactLogin(std::string_view text, std::string_view login, bool windows)
{
    std::string out;
    out.reserve(text.size());
    std::size_t at = 0;
    while (at < text.size())
    {
        if (const std::size_t length = loginComponentLength(text, at, login, windows); length != 0)
        {
            out += REDACTED_USER;
            at += length;
        }
        else
        {
            out += text[at];
            ++at;
        }
    }
    return out;
}

/// Whether @p a and @p b name the same path: on Windows ignoring ASCII case, with '/' and '\' alike.
[[nodiscard]] bool pathsEqual(std::string_view a, std::string_view b, bool windows) noexcept
{
    if (!windows)
    {
        return a == b;
    }
    constexpr auto fold = [](char c) noexcept
    {
        return isPathSeparator(c) ? '/' : Platform::foldAscii(c);
    };
    return std::ranges::equal(a, b, {}, fold, fold);
}

} // namespace

std::string redactHomeDirectory(std::string_view text, std::string_view homeDirectory, Platform::OsFamily family)
{
    std::string_view home = homeDirectory;
    while (!home.empty() && isPathSeparator(home.back()))
    {
        home.remove_suffix(1); // "/home/bob/" names the same folder
    }
    std::size_t leaf = home.size();
    while (leaf > 0 && !isPathSeparator(home[leaf - 1]))
    {
        --leaf;
    }
    if (leaf == 0 || leaf == home.size())
    {
        return std::string(text); // no folder name to redact: none at all, a root, or no separator
    }
    const bool windows = family == Platform::OsFamily::Windows;
    std::string out;
    out.reserve(text.size());
    std::size_t at = 0;
    while (at < text.size())
    {
        if (text.size() - at >= home.size() && pathsEqual(std::string_view(text.data() + at, home.size()), home, windows) &&
            endsComponent(text, at + home.size()))
        {
            out.append(text.data() + at, leaf); // the parent, as the text writes it
            out += REDACTED_USER;
            at += home.size();
        }
        else
        {
            out += text[at];
            ++at;
        }
    }
    return out;
}

std::string redactLoginName(std::string_view text, std::string_view userName, Platform::OsFamily family)
{
    // Windows gives "DOMAIN\user"; Linux the login name, which winbind writes as "DOMAIN\user" too and
    // SSSD may write as "user@domain", with the home directory under either form.
    std::string_view login = userName;
    if (const std::size_t slash = login.rfind('\\'); slash != std::string_view::npos)
    {
        login.remove_prefix(slash + 1);
    }
    const bool windows = family == Platform::OsFamily::Windows;
    std::string redacted = login.empty() ? std::string(text) : redactLogin(text, login, windows);
    if (const std::size_t at = login.find('@'); at != std::string_view::npos && at != 0)
    {
        redacted = redactLogin(redacted, login.substr(0, at), windows);
    }
    return redacted;
}

void redactUserPaths(std::span<Section> sections, const Platform::OsInfo& os)
{
    for (Section& section : sections)
    {
        for (Row& item : section.rows)
        {
            if (item.isIdentifier)
            {
                continue; // hidden whole
            }
            // The profile folder as well as the login name (#2197): the folder's name need not be the login's.
            const auto redact = [&os](std::string_view text)
            {
                return redactLoginName(redactHomeDirectory(text, os.homeDirectory, os.family), os.userName, os.family);
            };
            RedactedText redacted{
                .label = redact(item.label),
                .value = redact(item.value),
                .unavailableReason = redact(item.unavailableReason),
                .valueTooltip = redact(item.valueTooltip),
            };
            if (redacted.label != item.label || redacted.value != item.value || redacted.unavailableReason != item.unavailableReason ||
                redacted.valueTooltip != item.valueTooltip)
            {
                item.redacted = std::move(redacted);
            }
        }
    }
}

std::vector<Section> buildSystemInfoSections(const Domain::SystemInfoSnapshot& snapshot,
                                             const Core::GraphicsHostInfo& host,
                                             const Platform::SystemInfoCapabilities& capabilities,
                                             const Domain::SensorPublication* sensors)
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
        sections.push_back(buildFirmwareSection(snapshot.firmware, capabilities.canReadAuthorized));
    }
    if (snapshot.memory.available)
    {
        sections.push_back(buildMemorySection(snapshot.memory, capabilities.canReadAuthorized));
    }
    if (snapshot.paging.available)
    {
        sections.push_back(buildCommitPagingSection(snapshot.paging));
    }
    if (snapshot.storage.available)
    {
        sections.push_back(buildStorageSection(snapshot.storage));
    }
    if (snapshot.security.available)
    {
        sections.push_back(buildSecuritySection(snapshot.security));
    }
    // The sensor sampler's newest read once it has one (#2737), the page's own until then.
    if (sensors != nullptr && sensors->version != 0)
    {
        if (sensors->sensors.available)
        {
            sections.push_back(buildSensorsSection(sensors->sensors, sensors->ranges));
        }
    }
    else if (snapshot.sensors.available)
    {
        sections.push_back(buildSensorsSection(snapshot.sensors, {}));
    }
    if (snapshot.graphics.available)
    {
        sections.push_back(buildGraphicsSection(snapshot.graphics, host));
    }
    if (snapshot.adapters.available)
    {
        Section network = buildNetworkAdaptersSection(snapshot.adapters, capabilities.wifiDetailsGated);
        std::vector<Row> publicIp = buildPublicIpRows(capabilities.hasPublicIp, snapshot.publicIpIncluded, snapshot.publicIp);
        network.rows.insert(network.rows.begin(), std::make_move_iterator(publicIp.begin()), std::make_move_iterator(publicIp.end()));
        sections.push_back(std::move(network));
    }
    if (snapshot.boot.available)
    {
        sections.push_back(buildBootPerformanceSection(snapshot.boot));
    }
    if (snapshot.devices.available)
    {
        sections.push_back(buildDevicesSection(snapshot.devices));
    }
    if (snapshot.drivers.available)
    {
        sections.push_back(buildDriversSection(snapshot.drivers));
    }
    if (snapshot.crashes.available)
    {
        sections.push_back(buildCrashesSection(snapshot.crashes, capabilities.canReadAuthorized)); // the page's last section
    }
    // Further sections (#1514 and on) follow here, in the page's order.

    // Last, so no section can name the user in a path with identifiers hidden (#2120).
    redactUserPaths(sections, snapshot.os);
    return sections;
}

std::vector<VisibleSection> visibleSections(std::span<const Section> sections, std::string_view filter, bool showIdentifiers)
{
    std::vector<VisibleSection> visible;
    for (std::size_t s = 0; s < sections.size(); ++s)
    {
        const Section& section = sections[s];
        const bool titleMatches = containsIgnoreCase(section.title, filter);
        VisibleSection entry{.section = s, .rows = {}};
        for (std::size_t r = 0; r < section.rows.size(); ++r)
        {
            const Row& item = section.rows[r];
            if (item.isIdentifier && !showIdentifiers)
            {
                continue;
            }
            if (titleMatches || containsIgnoreCase(item.shownLabel(showIdentifiers), filter) ||
                containsIgnoreCase(item.shownValue(showIdentifiers), filter))
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
        text += item.shownLabel(showIdentifiers) + ": ";
        text += item.available() ? item.shownValue(showIdentifiers) : "unavailable (" + item.shownUnavailableReason(showIdentifiers) + ")";
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

std::string jsonString(std::string_view text)
{
    std::string out;
    out.reserve(text.size() + 2);
    out += '"';
    for (const char c : text)
    {
        switch (c)
        {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20U)
            {
                out += std::format("\\u{:04x}", static_cast<unsigned>(static_cast<unsigned char>(c)));
            }
            else
            {
                out += c; // UTF-8 bytes pass through
            }
        }
    }
    out += '"';
    return out;
}

std::string allSectionsJson(std::span<const Section> sections, bool showIdentifiers, std::string_view readAt)
{
    std::string json = "{\n";
    json += std::format("  \"readAt\": {},\n", jsonString(readAt));
    json += std::format("  \"identifiersIncluded\": {},\n", showIdentifiers ? "true" : "false");
    json += "  \"sections\": [";
    bool firstSection = true;
    for (const Section& section : sections)
    {
        json += firstSection ? "\n" : ",\n";
        firstSection = false;
        json += std::format("    {{\n      \"title\": {},\n      \"rows\": [", jsonString(section.title));
        bool firstRow = true;
        for (const Row& item : section.rows)
        {
            if (item.isIdentifier && !showIdentifiers)
            {
                continue;
            }
            json += firstRow ? "\n" : ",\n";
            firstRow = false;
            const std::string label = jsonString(item.shownLabel(showIdentifiers));
            json += item.available()
                      ? std::format(R"(        {{"label": {}, "value": {}}})", label, jsonString(item.shownValue(showIdentifiers)))
                      : std::format(R"(        {{"label": {}, "unavailable": {}}})",
                                    label,
                                    jsonString(item.shownUnavailableReason(showIdentifiers)));
        }
        json += firstRow ? "]\n    }" : "\n      ]\n    }";
    }
    json += firstSection ? "]\n}\n" : "\n  ]\n}\n";
    return json;
}

std::string reportFileName(std::string_view computerName, bool showIdentifiers, std::string_view date, ReportFormat format)
{
    std::string machine;
    if (showIdentifiers)
    {
        for (const char c : computerName)
        {
            const bool safe =
                (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
            machine += safe ? c : '-';
        }
        // Not a hidden file, nor one that ends in a dot.
        while (!machine.empty() && (machine.front() == '.' || machine.front() == '-'))
        {
            machine.erase(machine.begin());
        }
        while (!machine.empty() && (machine.back() == '.' || machine.back() == '-'))
        {
            machine.pop_back();
        }
    }
    if (machine.empty())
    {
        machine = "machine";
    }
    return std::format(
        "tasksmack-system-info-{}{}{}.{}", machine, date.empty() ? "" : "-", date, format == ReportFormat::Json ? "json" : "txt");
}

ReportFormat reportFormatFor(std::string_view path, int filter)
{
    // A typed ".txt" or ".json" says what the file is, whichever filter is selected (#2144).
    const std::size_t slash = path.find_last_of("/\\");
    const std::string_view name = slash == std::string_view::npos ? path : path.substr(slash + 1);
    const std::size_t dot = name.rfind('.');
    if (dot != std::string_view::npos && dot != 0)
    {
        std::string extension(name.substr(dot + 1));
        std::ranges::transform(extension, extension.begin(), Platform::foldAscii);
        if (extension == "json")
        {
            return ReportFormat::Json;
        }
        if (extension == "txt")
        {
            return ReportFormat::Text;
        }
    }
    return filter == 1 ? ReportFormat::Json : ReportFormat::Text;
}

std::string withReportExtension(std::string path, ReportFormat format)
{
    const std::size_t slash = path.find_last_of("/\\");
    const std::size_t nameStart = slash == std::string::npos ? 0 : slash + 1;
    const std::size_t dot = path.rfind('.');
    if (dot == std::string::npos || dot <= nameStart)
    {
        path += format == ReportFormat::Json ? ".json" : ".txt";
    }
    return path;
}

std::string numberedReportPath(std::string_view path, int number)
{
    const std::size_t slash = path.find_last_of("/\\");
    const std::size_t nameStart = slash == std::string_view::npos ? 0 : slash + 1;
    std::size_t dot = path.rfind('.');
    if (dot == std::string_view::npos || dot <= nameStart)
    {
        dot = path.size();
    }
    return std::format("{} ({}){}", path.substr(0, dot), number, path.substr(dot));
}

} // namespace App::SystemInfo
