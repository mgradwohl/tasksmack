#include "SystemInfoSections.h"

#include "Domain/SystemInfoModel.h"
#include "Platform/ISystemInfoProbe.h"
#include "UI/Format.h"
#include "UI/IconsFontAwesome6.h"

#include <algorithm>
#include <cctype>
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
