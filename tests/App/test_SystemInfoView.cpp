/// @file test_SystemInfoView.cpp
/// @brief The System Information page (#1399): the Operating system section's rows (#1512), the
/// Firmware & board section's rows (#1513), the Memory modules section's rows (#1515), the Commit & paging rows (#1516), the Storage
/// rows (#1517), the Security rows (#1514), the Sensors rows (#1522), the Graphics & displays rows
/// (#1519), the Network adapters rows (#1518), the Boot performance rows (#1525), the Devices rows
/// (#1520), the filter,
/// identifier hiding, the Copy text and unavailable values; then the view headless: the unsupported and loading states, sections drawn, the
/// filter narrowing and the identifier toggle.

#include "App/Panels/SystemInfoSections.h"
#include "App/Panels/SystemInfoView.h"
#include "Core/GraphicsHostInfo.h"
#include "Domain/SensorModel.h"
#include "Domain/SensorTracker.h"
#include "Domain/SystemInfoModel.h"
#include "Mocks/UnsupportedSystemInfoProbe.h"
#include "Platform/IServiceProbe.h"
#include "Platform/ISystemInfoProbe.h"
#include "UI/Format.h"
#include "UI/Sparkline.h"

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h> // GImGui->LogBuffer, FindWindowByName(): the tooltip a hover opens
#include <implot.h>

#include <algorithm>
#include <array>
#include <cfloat>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace App
{
namespace
{

using SystemInfo::Row;
using SystemInfo::Section;

[[nodiscard]] Platform::OsInfo windowsOs()
{
    Platform::OsInfo os;
    os.family = Platform::OsFamily::Windows;
    os.name = "Windows 11 Home";
    os.version = "25H2";
    os.build = "26300.1000";
    os.architecture = "x64";
    os.bootUnixSeconds = 1'700'000'000;
    os.computerName = "MATTS-PC";
    os.domainOrWorkgroup = "WORKGROUP";
    os.userName = "MATTS-PC\\matt";
    os.locale = "en-US";
    os.timeZone = "Pacific Standard Time";
    os.utcOffsetMinutes = -420;
    os.systemDirectory = "C:\\WINDOWS\\system32";
    os.windowsDirectory = "C:\\WINDOWS";
    return os; // no install date: unavailable
}

[[nodiscard]] Domain::SystemInfoSnapshot snapshot()
{
    Domain::SystemInfoSnapshot read;
    read.version = 3;
    read.readAtUnixSeconds = 1'700'000'000 + 93'784;
    read.os = windowsOs();
    return read;
}

[[nodiscard]] const Row* findRow(const Section& section, std::string_view label)
{
    const auto it = std::ranges::find(section.rows, label, &Row::label);
    return it != section.rows.end() ? &*it : nullptr;
}

TEST(SystemInfoSectionsTest, FormatsTimeZones)
{
    EXPECT_EQ(SystemInfo::formatUtcOffset(-480), "UTC-08:00");
    EXPECT_EQ(SystemInfo::formatUtcOffset(330), "UTC+05:30");
    EXPECT_EQ(SystemInfo::formatUtcOffset(0), "UTC");
    EXPECT_EQ(SystemInfo::formatTimeZone("Pacific Standard Time", -420), "Pacific Standard Time (UTC-07:00)");
    EXPECT_EQ(SystemInfo::formatTimeZone("Europe/Berlin", std::nullopt), "Europe/Berlin");
    EXPECT_EQ(SystemInfo::formatTimeZone("", 60), "UTC+01:00");
}

TEST(SystemInfoSectionsTest, WindowsOsSectionRows)
{
    const Section os = SystemInfo::buildOsSection(windowsOs(), 1'700'000'000 + 93'784);
    EXPECT_EQ(os.title, "Operating system");
    ASSERT_NE(findRow(os, "Edition"), nullptr);
    EXPECT_EQ(findRow(os, "Edition")->value, "Windows 11 Home");
    EXPECT_EQ(findRow(os, "Build")->value, "26300.1000");
    EXPECT_EQ(findRow(os, "Uptime")->value, "1d 02h");
    EXPECT_EQ(findRow(os, "Time zone")->value, "Pacific Standard Time (UTC-07:00)");
    EXPECT_EQ(findRow(os, "Workgroup")->value, "WORKGROUP");
    EXPECT_FALSE(findRow(os, "Workgroup")->isIdentifier); // a workgroup isn't identifying; a domain is
    EXPECT_TRUE(findRow(os, "User")->isIdentifier);
    EXPECT_TRUE(findRow(os, "Computer name")->isIdentifier);
    EXPECT_EQ(findRow(os, "Kernel"), nullptr); // Linux only

    const Row* installed = findRow(os, "Installed");
    ASSERT_NE(installed, nullptr);
    EXPECT_FALSE(installed->available());
    EXPECT_FALSE(installed->unavailableReason.empty());
}

TEST(SystemInfoSectionsTest, LinuxOsSectionRows)
{
    Platform::OsInfo os;
    os.family = Platform::OsFamily::Linux;
    os.name = "Ubuntu 24.04.1 LTS";
    os.kernel = "Linux 6.8.0";
    os.virtualization = "None detected";
    const Section section = SystemInfo::buildOsSection(os, 0);
    EXPECT_EQ(findRow(section, "Distribution")->value, "Ubuntu 24.04.1 LTS");
    EXPECT_EQ(findRow(section, "Kernel")->value, "Linux 6.8.0");
    EXPECT_FALSE(findRow(section, "Desktop")->available());
    EXPECT_TRUE(findRow(section, "Desktop")->unavailableReason.contains("XDG_CURRENT_DESKTOP"));
    EXPECT_EQ(findRow(section, "Windows directory"), nullptr);
    EXPECT_EQ(findRow(section, "Workgroup"), nullptr);
}

TEST(SystemInfoSectionsTest, FilterNarrowsAndIdentifiersStayHidden)
{
    const auto sections = SystemInfo::buildSystemInfoSections(snapshot());
    ASSERT_EQ(sections.size(), 1U);
    const auto all = SystemInfo::visibleSections(sections, "", false);
    ASSERT_EQ(all.size(), 1U);
    const auto shown = SystemInfo::visibleSections(sections, "", true);
    EXPECT_EQ(shown[0].rows.size(), all[0].rows.size() + 2); // computer name and user

    // A label or a value matches, case-insensitively; a hidden identifier never does.
    const auto zone = SystemInfo::visibleSections(sections, "PACIFIC", false);
    ASSERT_EQ(zone.size(), 1U);
    ASSERT_EQ(zone[0].rows.size(), 1U);
    EXPECT_EQ(sections[0].rows[zone[0].rows[0]].label, "Time zone");
    EXPECT_TRUE(SystemInfo::visibleSections(sections, "matt", false).empty());
    EXPECT_FALSE(SystemInfo::visibleSections(sections, "matt", true).empty());
    // A section whose title matches keeps all its (visible) rows.
    EXPECT_EQ(SystemInfo::visibleSections(sections, "operating", false)[0].rows.size(), all[0].rows.size());
}

// #1866: Save report's JSON: the same rows as Copy all, identifiers left out unless shown, unavailable
// rows with their reason, strings escaped.
TEST(SystemInfoSectionsTest, ReportJsonHasTheRowsCopyHas)
{
    const auto sections = SystemInfo::buildSystemInfoSections(snapshot());
    const std::string hidden = SystemInfo::allSectionsJson(sections, false, "Read at 10:00:00");
    EXPECT_TRUE(hidden.starts_with("{\n  \"readAt\": \"Read at 10:00:00\",\n  \"identifiersIncluded\": false,\n  \"sections\": [\n"))
        << hidden;
    EXPECT_TRUE(hidden.contains("\"title\": \"Operating system\"")) << hidden;
    EXPECT_TRUE(hidden.contains("{\"label\": \"Edition\", \"value\": \"Windows 11 Home\"}")) << hidden;
    EXPECT_TRUE(hidden.contains("{\"label\": \"Installed\", \"unavailable\": \"Not reported by this system\"}")) << hidden;
    EXPECT_FALSE(hidden.contains("MATTS-PC"));
    EXPECT_TRUE(hidden.ends_with("\n  ]\n}\n"));

    const std::string shown = SystemInfo::allSectionsJson(sections, true, "");
    EXPECT_TRUE(shown.contains("\"identifiersIncluded\": true"));
    EXPECT_TRUE(shown.contains("{\"label\": \"User\", \"value\": \"MATTS-PC\\\\matt\"}")) << "the backslash escaped";

    EXPECT_EQ(SystemInfo::allSectionsJson({}, false, "t"),
              "{\n  \"readAt\": \"t\",\n  \"identifiersIncluded\": false,\n  \"sections\": []\n}\n");
    const std::vector<Section> empty{{.title = "Empty", .icon = "", .rows = {}}};
    EXPECT_TRUE(SystemInfo::allSectionsJson(empty, false, "").contains("\"title\": \"Empty\",\n      \"rows\": []\n    }"));
}

TEST(SystemInfoSectionsTest, JsonStringEscapes)
{
    EXPECT_EQ(SystemInfo::jsonString("plain"), "\"plain\"");
    EXPECT_EQ(SystemInfo::jsonString("a\"b\\c"), "\"a\\\"b\\\\c\"");
    EXPECT_EQ(SystemInfo::jsonString("\n\r\t\b\f"), "\"\\n\\r\\t\\b\\f\"");
    EXPECT_EQ(SystemInfo::jsonString(std::string_view("\x01\x1f", 2)), "\"\\u0001\\u001f\"");
    EXPECT_EQ(SystemInfo::jsonString(std::string_view("\0", 1)), "\"\\u0000\"");
    EXPECT_EQ(SystemInfo::jsonString("52,5 \xC2\xB0"
                                     "C"),
              "\"52,5 \xC2\xB0"
              "C\"")
        << "UTF-8 kept as it is";
    EXPECT_EQ(SystemInfo::jsonString("\x7F"), "\"\x7F\"");
}

TEST(SystemInfoSectionsTest, ReportFileNameFormatAndExtension)
{
    using SystemInfo::ReportFormat;
    EXPECT_EQ(SystemInfo::reportFileName("MATTS-PC", true, "2026-10-12", ReportFormat::Text),
              "tasksmack-system-info-MATTS-PC-2026-10-12.txt");
    EXPECT_EQ(SystemInfo::reportFileName("MATTS-PC", false, "2026-10-12", ReportFormat::Json),
              "tasksmack-system-info-machine-2026-10-12.json")
        << "the name is an identifier";
    EXPECT_EQ(SystemInfo::reportFileName("my box/../x", true, "2026-10-12", ReportFormat::Text),
              "tasksmack-system-info-my-box-..-x-2026-10-12.txt");
    EXPECT_EQ(SystemInfo::reportFileName("..", true, "2026-10-12", ReportFormat::Text), "tasksmack-system-info-machine-2026-10-12.txt");
    EXPECT_EQ(SystemInfo::reportFileName("", true, "", ReportFormat::Text), "tasksmack-system-info-machine.txt");
    // #1891: trailing dots and dashes are trimmed too, so the name never ends the stem in one.
    EXPECT_EQ(SystemInfo::reportFileName("box-.", true, "", ReportFormat::Text), "tasksmack-system-info-box.txt");

    // #2144: a typed ".txt" or ".json" wins over the dialog's filter; the filter decides otherwise.
    EXPECT_EQ(SystemInfo::reportFormatFor("/tmp/a.txt", 1), ReportFormat::Text) << "the typed extension wins";
    EXPECT_EQ(SystemInfo::reportFormatFor("x.json", 0), ReportFormat::Json) << "the typed extension wins";
    EXPECT_EQ(SystemInfo::reportFormatFor("/tmp/a.TXT", 1), ReportFormat::Text);
    EXPECT_EQ(SystemInfo::reportFormatFor("/tmp/a", 1), ReportFormat::Json);
    EXPECT_EQ(SystemInfo::reportFormatFor("/tmp/a", 0), ReportFormat::Text);
    EXPECT_EQ(SystemInfo::reportFormatFor("/tmp/a.log", 1), ReportFormat::Json) << "another extension: the filter";
    EXPECT_EQ(SystemInfo::reportFormatFor("/tmp/a.JSON", -1), ReportFormat::Json);
    EXPECT_EQ(SystemInfo::reportFormatFor("/tmp/a.json.txt", -1), ReportFormat::Text);
    EXPECT_EQ(SystemInfo::reportFormatFor("/tmp/dir.json/report", -1), ReportFormat::Text);
    EXPECT_EQ(SystemInfo::reportFormatFor("C:\\Users\\m\\.json", -1), ReportFormat::Text) << "a dotfile, not an extension";

    EXPECT_EQ(SystemInfo::withReportExtension("/tmp/report", ReportFormat::Json), "/tmp/report.json");
    EXPECT_EQ(SystemInfo::withReportExtension("/tmp/report.txt", ReportFormat::Json), "/tmp/report.txt") << "a chosen extension stays";
    EXPECT_EQ(SystemInfo::withReportExtension("/tmp/dir.d/report", ReportFormat::Text), "/tmp/dir.d/report.txt");
    EXPECT_EQ(SystemInfo::withReportExtension("/tmp/.report", ReportFormat::Text), "/tmp/.report.txt");

    EXPECT_EQ(SystemInfo::numberedReportPath("/tmp/report.txt", 2), "/tmp/report (2).txt");
    EXPECT_EQ(SystemInfo::numberedReportPath("C:\\d.d\\report.json", 3), "C:\\d.d\\report (3).json");
    EXPECT_EQ(SystemInfo::numberedReportPath("/tmp/dir.d/report", 2), "/tmp/dir.d/report (2)");
    EXPECT_EQ(SystemInfo::numberedReportPath("/tmp/.report", 2), "/tmp/.report (2)");
}

TEST(SystemInfoSectionsTest, CopyTextLeavesHiddenIdentifiersOut)
{
    const auto sections = SystemInfo::buildSystemInfoSections(snapshot());
    const std::string hidden = SystemInfo::allSectionsText(sections, false);
    EXPECT_TRUE(hidden.starts_with("Operating system\nEdition: Windows 11 Home\nVersion: 25H2\n")) << hidden;
    EXPECT_FALSE(hidden.contains("MATTS-PC"));
    EXPECT_TRUE(hidden.contains("Installed: unavailable (Not reported by this system)\n"));
    const std::string shown = SystemInfo::sectionText(sections[0], true);
    EXPECT_TRUE(shown.contains("User: MATTS-PC\\matt\n"));
    EXPECT_TRUE(shown.contains("Computer name: MATTS-PC\n"));
}

[[nodiscard]] Platform::FirmwareInfo firmware()
{
    Platform::FirmwareInfo info;
    info.available = true;
    info.systemManufacturer = "Contoso";
    info.systemModel = "Surface Pro";
    info.systemSerial = "SN-123";
    info.systemUuid = "00112233-4455-6677-8899-AABBCCDDEEFF";
    info.biosVendor = "Contoso";
    info.biosVersion = "1.2.3";
    info.firmwareMode = Platform::FirmwareMode::Uefi;
    info.smbiosVersion = "3.4";
    info.boardSerial = "BSN-1";
    info.chassisType = "Notebook";
    info.platformRole = "Mobile";
    return info; // no SKU, no embedded controller
}

TEST(SystemInfoSectionsTest, FirmwareSectionRows)
{
    const Section section = SystemInfo::buildFirmwareSection(firmware());
    EXPECT_EQ(section.title, "Firmware & board");
    EXPECT_EQ(findRow(section, "Manufacturer")->value, "Contoso");
    EXPECT_EQ(findRow(section, "Firmware mode")->value, "UEFI");
    EXPECT_EQ(findRow(section, "SMBIOS version")->value, "3.4");
    EXPECT_EQ(findRow(section, "Chassis type")->value, "Notebook");
    EXPECT_EQ(findRow(section, "Platform role")->value, "Mobile");
    EXPECT_EQ(findRow(section, "Embedded controller"), nullptr); // only when there is one
    EXPECT_FALSE(findRow(section, "SKU")->available());
    EXPECT_EQ(findRow(section, "SKU")->unavailableReason, "Not reported by this system");

    // Serials and the UUID are identifiers; nothing else is.
    for (const Row& item : section.rows)
    {
        const bool identifier = item.label == "Serial number" || item.label == "UUID" || item.label == "Board serial number";
        EXPECT_EQ(item.isIdentifier, identifier) << item.label;
    }
    const auto hidden = SystemInfo::visibleSections(std::span(&section, 1), "", false);
    const auto shown = SystemInfo::visibleSections(std::span(&section, 1), "", true);
    EXPECT_EQ(shown[0].rows.size(), hidden[0].rows.size() + 3);
    EXPECT_FALSE(SystemInfo::sectionText(section, false).contains("SN-123"));
    EXPECT_TRUE(SystemInfo::sectionText(section, true).contains("Serial number: SN-123\n"));

    Platform::FirmwareInfo ec = firmware();
    ec.embeddedControllerVersion = "1.23";
    ec.firmwareMode = Platform::FirmwareMode::Legacy;
    const Section withEc = SystemInfo::buildFirmwareSection(ec);
    EXPECT_EQ(findRow(withEc, "Embedded controller")->value, "1.23");
    EXPECT_EQ(findRow(withEc, "Firmware mode")->value, "Legacy BIOS");
    EXPECT_FALSE(SystemInfo::buildFirmwareSection(Platform::FirmwareInfo{}).rows.empty());
    EXPECT_FALSE(findRow(SystemInfo::buildFirmwareSection(Platform::FirmwareInfo{}), "Firmware mode")->available());
}

TEST(SystemInfoSectionsTest, FirmwareRootOnlyRowsSayWhy)
{
    Platform::FirmwareInfo rootOnly;
    rootOnly.available = true;
    rootOnly.identifiersNeedAdmin = true;
    rootOnly.smbiosVersionNeedsAdmin = true;
    const Section section = SystemInfo::buildFirmwareSection(rootOnly);
    for (const std::string_view label : {"Serial number", "UUID", "Board serial number", "SMBIOS version"})
    {
        const Row* item = findRow(section, label);
        ASSERT_NE(item, nullptr) << label;
        EXPECT_FALSE(item->available());
        EXPECT_TRUE(item->unavailableReason.contains("administrator")) << label;
    }
    EXPECT_FALSE(findRow(section, "Model")->unavailableReason.contains("administrator"));
    EXPECT_EQ(findRow(section, "Readable by root only"), nullptr) << "nothing to offer without the helper";

    // With the helper installed, the section leads with "Authenticate to read" (#2310)
    const Section offered = SystemInfo::buildFirmwareSection(rootOnly, /*canAuthenticate=*/true);
    ASSERT_FALSE(offered.rows.empty());
    EXPECT_EQ(offered.rows.front().label, "Readable by root only");
    EXPECT_EQ(offered.rows.front().action, SystemInfo::RowAction::AuthenticateToRead);
    EXPECT_FALSE(offered.rows.front().isIdentifier) << "its button must stay visible";

    Platform::FirmwareInfo readable = rootOnly;
    readable.identifiersNeedAdmin = false;
    readable.smbiosVersionNeedsAdmin = false;
    EXPECT_EQ(findRow(SystemInfo::buildFirmwareSection(readable, true), "Readable by root only"), nullptr);
}

TEST(SystemInfoSectionsTest, FirmwareSectionFollowsTheOsSection)
{
    Domain::SystemInfoSnapshot both = snapshot();
    both.firmware = firmware();
    const auto sections = SystemInfo::buildSystemInfoSections(both);
    ASSERT_EQ(sections.size(), 2U);
    EXPECT_EQ(sections[0].title, "Operating system");
    EXPECT_EQ(sections[1].title, "Firmware & board");
}

constexpr std::uint64_t GIB = std::uint64_t{1024} * 1024 * 1024;

[[nodiscard]] Platform::MemoryModulesInfo memory()
{
    Platform::MemoryModulesInfo info;
    info.available = true;
    info.tableRead = true;
    info.slotCount = 4;
    info.maxCapacityBytes = 128 * GIB;
    info.installedBytes = 32 * GIB;
    info.usableBytes = (32 * GIB) - (std::uint64_t{312} * 1024 * 1024);
    info.modules.push_back({
        .locator = "DIMM A1",
        .bankLocator = "BANK 0",
        .sizeBytes = 16 * GIB,
        .type = "DDR5",
        .formFactor = "DIMM",
        .speedMts = 6400,
        .configuredSpeedMts = 5600,
        .manufacturer = "Samsung",
        .partNumber = "M323R2GA3BB0-CQKOD",
    });
    info.modules.push_back({
        .locator = "DIMM B1",
        .bankLocator = "BANK 1",
        .sizeBytes = 16 * GIB,
        .type = "DDR5",
        .formFactor = "DIMM",
        .speedMts = 5600,
        .configuredSpeedMts = 5600,
        .manufacturer = "",
        .partNumber = "",
    });
    return info;
}

TEST(SystemInfoSectionsTest, FormatsMemoryValues)
{
    EXPECT_EQ(SystemInfo::formatMemoryCapacity(0), "");
    EXPECT_EQ(SystemInfo::formatMemoryCapacity(16 * GIB), "16 GiB");
    EXPECT_EQ(SystemInfo::formatMemoryCapacity(512ULL * 1024 * 1024), "512 MiB");
    EXPECT_EQ(SystemInfo::formatMemorySpeed(5600, 6400), "5600 MT/s (rated 6400 MT/s)");
    EXPECT_EQ(SystemInfo::formatMemorySpeed(5600, 5600), "5600 MT/s");
    EXPECT_EQ(SystemInfo::formatMemorySpeed(5600, 0), "5600 MT/s");
    EXPECT_EQ(SystemInfo::formatMemorySpeed(0, 6400), "rated 6400 MT/s");
    EXPECT_EQ(SystemInfo::formatMemorySpeed(0, 0), "");
    EXPECT_EQ(SystemInfo::formatMemoryModule(Platform::MemoryModule{}), "");
}

TEST(SystemInfoSectionsTest, MemorySectionRows)
{
    const Section section = SystemInfo::buildMemorySection(memory());
    EXPECT_EQ(section.title, "Memory modules");
    EXPECT_EQ(findRow(section, "Slots used")->value, "2 of 4");
    EXPECT_EQ(findRow(section, "Maximum capacity")->value, "128 GiB");
    EXPECT_EQ(findRow(section, "Installed memory")->value, "32 GiB");
    EXPECT_TRUE(findRow(section, "Usable memory")->value.ends_with("(312 MiB hardware reserved)"))
        << findRow(section, "Usable memory")->value;
    ASSERT_NE(findRow(section, "DIMM A1"), nullptr);
    EXPECT_EQ(findRow(section, "DIMM A1")->value, "16 GiB DDR5 DIMM, 5600 MT/s (rated 6400 MT/s), Samsung M323R2GA3BB0-CQKOD");
    EXPECT_EQ(findRow(section, "DIMM B1")->value, "16 GiB DDR5 DIMM, 5600 MT/s");
    EXPECT_EQ(findRow(section, "Modules"), nullptr);
    for (const Row& item : section.rows)
    {
        EXPECT_FALSE(item.isIdentifier) << item.label;
    }
    // The filter finds a module by its part number or type.
    EXPECT_EQ(SystemInfo::visibleSections(std::span(&section, 1), "CQKOD", false)[0].rows.size(), 1U);
}

TEST(SystemInfoSectionsTest, MemoryModuleLabels)
{
    // Two modules named "DIMM 0" in different banks get the bank in front; a module with no locator
    // falls back to its bank, then to its position.
    Platform::MemoryModulesInfo info = memory();
    info.modules.at(0).locator = "DIMM 0";
    info.modules.at(1).locator = "DIMM 0";
    Platform::MemoryModule unnamed;
    unnamed.sizeBytes = 8 * GIB;
    info.modules.push_back(unnamed);
    unnamed.bankLocator = "BANK 3";
    info.modules.push_back(unnamed);
    const Section section = SystemInfo::buildMemorySection(info);
    EXPECT_NE(findRow(section, "BANK 0 DIMM 0"), nullptr);
    EXPECT_NE(findRow(section, "BANK 1 DIMM 0"), nullptr);
    EXPECT_EQ(findRow(section, "Module 3")->value, "8 GiB");
    EXPECT_NE(findRow(section, "BANK 3"), nullptr);

    // Soldered memory: every device "Motherboard", no bank. They are numbered in table order.
    Platform::MemoryModulesInfo soldered = memory();
    for (Platform::MemoryModule& module : soldered.modules)
    {
        module.locator = "Motherboard";
        module.bankLocator.clear();
    }
    const Section numbered = SystemInfo::buildMemorySection(soldered);
    EXPECT_NE(findRow(numbered, "Motherboard #1"), nullptr);
    EXPECT_NE(findRow(numbered, "Motherboard #2"), nullptr);
    EXPECT_EQ(findRow(numbered, "Motherboard"), nullptr);
}

TEST(SystemInfoSectionsTest, InstalledMemoryFallsBackToTheModulesTotal)
{
    Platform::MemoryModulesInfo info = memory();
    EXPECT_EQ(SystemInfo::installedMemoryBytes(info), 32 * GIB); // the OS's own figure
    info.installedBytes = 0;                                     // Linux: none of its own
    info.modules.at(1).sizeBytes = 8 * GIB;
    EXPECT_EQ(SystemInfo::installedMemoryBytes(info), 24 * GIB);
    EXPECT_EQ(findRow(SystemInfo::buildMemorySection(info), "Installed memory")->value, "24 GiB");
    info.modules.at(1).sizeBytes = 0; // an unknown size: no total
    EXPECT_EQ(SystemInfo::installedMemoryBytes(info), 0U);
    info.tableRead = false;
    EXPECT_EQ(SystemInfo::installedMemoryBytes(info), 0U);
}

TEST(SystemInfoSectionsTest, MemoryWithoutTheTableSaysWhy)
{
    // Linux, unprivileged: the table is root-only, so only the usable total is known.
    Platform::MemoryModulesInfo rootOnly;
    rootOnly.available = true;
    rootOnly.tableNeedsAdmin = true;
    rootOnly.usableBytes = 16 * GIB;
    const Section section = SystemInfo::buildMemorySection(rootOnly);
    EXPECT_EQ(findRow(section, "Usable memory")->value, "16 GiB");
    EXPECT_EQ(findRow(section, "Readable by root only"), nullptr);
    EXPECT_EQ(SystemInfo::buildMemorySection(rootOnly, /*canAuthenticate=*/true).rows.front().action,
              SystemInfo::RowAction::AuthenticateToRead); // #2310
    for (const std::string_view label : {"Slots used", "Maximum capacity", "Installed memory", "Modules"})
    {
        const Row* item = findRow(section, label);
        ASSERT_NE(item, nullptr) << label;
        EXPECT_FALSE(item->available());
        EXPECT_TRUE(item->unavailableReason.contains("administrator")) << label;
    }

    // A table that lists no installed memory says so instead.
    Platform::MemoryModulesInfo empty;
    empty.available = true;
    empty.tableRead = true;
    const Section emptySection = SystemInfo::buildMemorySection(empty);
    const Row* modules = findRow(emptySection, "Modules");
    ASSERT_NE(modules, nullptr);
    EXPECT_FALSE(modules->unavailableReason.contains("administrator"));
}

TEST(SystemInfoSectionsTest, MemorySectionFollowsTheFirmwareSection)
{
    Domain::SystemInfoSnapshot all = snapshot();
    all.firmware = firmware();
    all.memory = memory();
    const auto sections = SystemInfo::buildSystemInfoSections(all);
    ASSERT_EQ(sections.size(), 3U);
    EXPECT_EQ(sections[2].title, "Memory modules");
}

constexpr std::uint64_t MIB = std::uint64_t{1024} * 1024;

[[nodiscard]] Platform::CommitPagingInfo windowsPaging()
{
    Platform::CommitPagingInfo info;
    info.available = true;
    info.family = Platform::OsFamily::Windows;
    info.committedBytes = 12 * GIB;
    info.commitLimitBytes = 48 * GIB;
    info.commitPeakBytes = 20 * GIB;
    info.pageSizeBytes = 4096;
    info.pageFilesRead = true;
    info.pageFiles.push_back(
        {.path = "C:\\pagefile.sys", .kind = {}, .sizeBytes = 16 * GIB, .usedBytes = 512 * MIB, .peakBytes = 2 * GIB, .priority = 0});
    info.compressedBytes = 300 * MIB;
    return info;
}

[[nodiscard]] Platform::CommitPagingInfo linuxPaging()
{
    Platform::CommitPagingInfo info;
    info.available = true;
    info.family = Platform::OsFamily::Linux;
    info.committedBytes = 8 * GIB;
    info.commitLimitBytes = 32 * GIB;
    info.overcommit = Platform::OvercommitMode::Heuristic;
    info.pageFilesRead = true;
    info.pageFiles.push_back(
        {.path = "/dev/nvme0n1p3", .kind = "partition", .sizeBytes = 8 * GIB, .usedBytes = 0, .peakBytes = 0, .priority = -2});
    info.zramRead = true;
    info.zram.push_back({.name = "zram0", .originalBytes = 4 * GIB, .compressedBytes = 1 * GIB, .memoryUsedBytes = 1100 * MIB});
    info.zswapEnabled = true;
    info.hugePagesRead = true;
    info.hugePagesTotal = 4;
    info.hugePagesFree = 3;
    info.hugePagesReserved = 1;
    info.hugePageSizeBytes = 2 * MIB;
    info.transparentHugePages = "madvise";
    return info;
}

TEST(SystemInfoSectionsTest, FormatsCommitAndPagingValues)
{
    EXPECT_EQ(SystemInfo::formatCommitCharge(12 * GIB, 48 * GIB), "12 GiB / 48 GiB (25%)");
    EXPECT_EQ(SystemInfo::formatCommitCharge(12 * GIB, 0), "12 GiB");
    EXPECT_EQ(SystemInfo::formatCommitCharge(0, 48 * GIB), "");
    EXPECT_EQ(
        SystemInfo::formatZramDevice({.name = "zram0", .originalBytes = 4 * GIB, .compressedBytes = 1 * GIB, .memoryUsedBytes = 1 * GIB}),
        "4 GiB stored in 1 GiB (4.0:1), 1 GiB of RAM");
    EXPECT_EQ(SystemInfo::formatZramDevice({}), "Empty");
}

TEST(SystemInfoSectionsTest, CommitPagingRowsWindows)
{
    const Section section = SystemInfo::buildCommitPagingSection(windowsPaging());
    EXPECT_EQ(section.title, "Commit & paging");
    EXPECT_EQ(findRow(section, "Commit charge")->value, "12 GiB / 48 GiB (25%)");
    EXPECT_EQ(findRow(section, "Peak commit")->value, "20 GiB");
    ASSERT_NE(findRow(section, "C:\\pagefile.sys"), nullptr);
    EXPECT_EQ(findRow(section, "C:\\pagefile.sys")->value, "512 MiB used of 16 GiB, peak 2 GiB");
    EXPECT_EQ(findRow(section, "Compressed memory")->value, "300 MiB");
    EXPECT_EQ(findRow(section, "Page size")->value, "4 KiB");
    EXPECT_EQ(findRow(section, "Overcommit mode"), nullptr); // Linux only
    for (const Row& item : section.rows)
    {
        EXPECT_FALSE(item.isIdentifier) << item.label;
    }

    // No page file, no compression process, GetPerformanceInfo failing.
    Platform::CommitPagingInfo bare;
    bare.available = true;
    bare.family = Platform::OsFamily::Windows;
    bare.pageFilesRead = true;
    const Section none = SystemInfo::buildCommitPagingSection(bare);
    EXPECT_EQ(findRow(none, "Page files")->value, "None (paging is off)");
    for (const std::string_view label : {"Commit charge", "Peak commit", "Compressed memory", "Page size"})
    {
        ASSERT_NE(findRow(none, label), nullptr) << label;
        EXPECT_FALSE(findRow(none, label)->available()) << label;
        EXPECT_FALSE(findRow(none, label)->unavailableReason.empty()) << label;
    }
}

TEST(SystemInfoSectionsTest, CommitPagingRowsLinux)
{
    const Section section = SystemInfo::buildCommitPagingSection(linuxPaging());
    EXPECT_EQ(findRow(section, "Commit charge")->value, "8 GiB / 32 GiB (25%)");
    EXPECT_EQ(findRow(section, "Overcommit mode")->value, "Heuristic (0)");
    EXPECT_EQ(findRow(section, "/dev/nvme0n1p3")->value, "0 B used of 8 GiB, partition, priority -2");
    EXPECT_EQ(findRow(section, "zram0")->value, "4 GiB stored in 1 GiB (4.0:1), 1.1 GiB of RAM");
    EXPECT_EQ(findRow(section, "zswap")->value, "Enabled");
    EXPECT_EQ(findRow(section, "Huge pages")->value, "3 of 4 free, 1 reserved, 0 surplus (2 MiB pages)");
    EXPECT_EQ(findRow(section, "Transparent huge pages")->value, "madvise");
    EXPECT_EQ(findRow(section, "Peak commit"), nullptr); // Windows only
    EXPECT_EQ(findRow(section, "zram"), nullptr);        // devices listed instead
}

TEST(SystemInfoSectionsTest, SecurityRows)
{
    Platform::PlatformSecurityInfo security;
    security.available = true;
    security.secureBoot = Platform::SecurityFeatureState::On;
    security.tpm = Platform::SecurityFeatureState::On;
    security.tpmVersionMajor = 2;
    security.lsmRead = true;
    security.lsms = {"lockdown", "capability", "apparmor"};
    security.selinuxEnforcing = std::nullopt;
    security.apparmorEnabled = true;
    security.lockdown = "integrity";
    security.vulnerabilitiesRead = true;
    security.vulnerabilities = {{.name = "meltdown", .status = "Not affected"},
                                {.name = "spectre_v2", .status = "Mitigation: Enhanced / Automatic IBRS"},
                                {.name = "mds", .status = "Vulnerable: Clear CPU buffers attempted, no microcode"}};
    const Section section = SystemInfo::buildSecuritySection(security);
    EXPECT_EQ(section.title, "Security");
    EXPECT_EQ(findRow(section, "Secure Boot")->value, "On");
    EXPECT_EQ(findRow(section, "TPM")->value, "Present (TPM 2.0)");
    EXPECT_EQ(findRow(section, "Security modules")->value, "lockdown, capability, apparmor");
    EXPECT_EQ(findRow(section, "SELinux")->value, "Not active");
    EXPECT_EQ(findRow(section, "AppArmor")->value, "Enabled");
    EXPECT_EQ(findRow(section, "Kernel lockdown")->value, "Integrity");
    EXPECT_EQ(findRow(section, "CPU vulnerabilities")->value, "1 vulnerable, 1 mitigated, 1 not affected");
    EXPECT_EQ(findRow(section, "Meltdown")->value, "Not affected");
    EXPECT_EQ(findRow(section, "Spectre v2")->value, "Mitigation: Enhanced / Automatic IBRS");
    EXPECT_EQ(findRow(section, "MDS")->value, "Vulnerable: Clear CPU buffers attempted, no microcode");
    EXPECT_TRUE(std::ranges::none_of(section.rows, [](const Row& item) { return item.isIdentifier; })); // nothing names the machine
}

// #1891: the value texts the other tests' fixtures don't reach.
TEST(SystemInfoSectionsTest, OvercommitModesAlwaysAndStrict)
{
    Platform::CommitPagingInfo paging = linuxPaging();
    paging.overcommit = Platform::OvercommitMode::Always;
    EXPECT_EQ(findRow(SystemInfo::buildCommitPagingSection(paging), "Overcommit mode")->value, "Always overcommit (1)");
    paging.overcommit = Platform::OvercommitMode::Strict;
    EXPECT_EQ(findRow(SystemInfo::buildCommitPagingSection(paging), "Overcommit mode")->value, "Strict, the commit limit is enforced (2)");
}

TEST(SystemInfoSectionsTest, SecureBootOffAndOlderOrUnversionedTpms)
{
    Platform::PlatformSecurityInfo security;
    security.available = true;
    security.secureBoot = Platform::SecurityFeatureState::Off;
    security.tpm = Platform::SecurityFeatureState::On;
    security.tpmVersionMajor = 1;
    Section section = SystemInfo::buildSecuritySection(security);
    EXPECT_EQ(findRow(section, "Secure Boot")->value, "Off");
    EXPECT_EQ(findRow(section, "TPM")->value, "Present (TPM 1.2)");

    security.tpmVersionMajor = 0; // present, version not reported
    EXPECT_EQ(findRow(SystemInfo::buildSecuritySection(security), "TPM")->value, "Present");
    security.tpm = Platform::SecurityFeatureState::Off;
    EXPECT_EQ(findRow(SystemInfo::buildSecuritySection(security), "TPM")->value, "Not detected");
}

TEST(SystemInfoSectionsTest, AnAudioEndpointOfUnknownFlowIsAnAudioDevice)
{
    Platform::DevicesInfo devices;
    devices.available = true;
    devices.audio = {{.name = "HDMI 0", .flow = Platform::AudioFlow::Unknown}};
    const Section section = SystemInfo::buildDevicesSection(devices);
    const Row* item = findRow(section, "Audio device");
    ASSERT_NE(item, nullptr);
    EXPECT_EQ(item->value, "HDMI 0");
}

TEST(SystemInfoSectionsTest, BootWithAnInitramfsListsTheInitrdPhase)
{
    Platform::BootPerformanceInfo boot;
    boot.available = true;
    boot.timingsRead = true;
    boot.finished = true;
    boot.kernelUs = 1'000'000;
    boot.initrdUs = 750'000;
    boot.userspaceUs = 2'000'000;
    boot.totalUs = 3'750'000;
    const Section section = SystemInfo::buildBootPerformanceSection(boot); // findRow points into it
    const Row* initrd = findRow(section, "Initrd");
    ASSERT_NE(initrd, nullptr);
    EXPECT_EQ(initrd->value, "750 ms");
}

TEST(SystemInfoSectionsTest, SecurityUnknownsAreMutedAndNotSupportedSaysSo)
{
    Platform::PlatformSecurityInfo unread;
    unread.available = true;
    const Section section = SystemInfo::buildSecuritySection(unread);
    for (const std::string_view label : {"Secure Boot", "TPM", "Security modules", "Kernel lockdown", "CPU vulnerabilities"})
    {
        const Row* item = findRow(section, label);
        ASSERT_NE(item, nullptr) << label;
        EXPECT_FALSE(item->available()) << label;
        EXPECT_FALSE(item->unavailableReason.empty()) << label;
    }
    EXPECT_EQ(findRow(section, "SELinux")->value, "Not active");
    EXPECT_EQ(findRow(section, "AppArmor")->value, "Not loaded");

    unread.secureBoot = Platform::SecurityFeatureState::NotSupported;
    unread.tpm = Platform::SecurityFeatureState::NotSupported;
    unread.selinuxEnforcing = false;
    const Section legacy = SystemInfo::buildSecuritySection(unread);
    EXPECT_EQ(findRow(legacy, "Secure Boot")->value, "Not supported (not booted with UEFI)");
    EXPECT_EQ(findRow(legacy, "TPM")->value, "Not detected");
    EXPECT_EQ(findRow(legacy, "SELinux")->value, "Permissive");
}

TEST(SystemInfoSectionsTest, VulnerabilityNamesAndSummary)
{
    EXPECT_EQ(SystemInfo::formatVulnerabilityName("spectre_v2"), "Spectre v2");
    EXPECT_EQ(SystemInfo::formatVulnerabilityName("spec_store_bypass"), "Spec store bypass");
    EXPECT_EQ(SystemInfo::formatVulnerabilityName("mds"), "MDS");
    EXPECT_EQ(SystemInfo::formatVulnerabilityName("tsx_async_abort"), "TSX async abort");
    EXPECT_EQ(SystemInfo::formatVulnerabilityName("itlb_multihit"), "ITLB multihit");
    EXPECT_EQ(SystemInfo::formatVulnerabilityName("mmio_stale_data"), "MMIO stale data");
    EXPECT_EQ(SystemInfo::formatVulnerabilityName("l1tf"), "L1TF");
    EXPECT_EQ(SystemInfo::formatVulnerabilitySummary({}), "");
    const std::vector<Platform::CpuVulnerability> odd{{.name = "x", .status = "Unknown: no microcode"},
                                                      {.name = "y", .status = "Not affected"}};
    EXPECT_EQ(SystemInfo::formatVulnerabilitySummary(odd), "1 not affected, 1 unknown");
}

TEST(SystemInfoSectionsTest, WindowsUnknownVulnerabilitiesAreCountedAndExplained)
{
    Platform::PlatformSecurityInfo security;
    security.available = true;
    security.family = Platform::OsFamily::Windows;
    security.vulnerabilitiesRead = true;
    security.vulnerabilities = {{.name = "l1tf", .status = "Unknown"},
                                {.name = "mds", .status = "Unknown"},
                                {.name = "meltdown", .status = "Not affected"},
                                {.name = "spec_store_bypass", .status = "Vulnerable: Speculative Store Bypass Disable is off"},
                                {.name = "spectre_v2", .status = "Mitigation: Enhanced IBRS"},
                                {.name = "gather_data_sampling", .status = "Mitigation: Microcode"}};
    const Section section = SystemInfo::buildSecuritySection(security);
    EXPECT_EQ(findRow(section, "CPU vulnerabilities")->value, "1 vulnerable, 2 mitigated, 1 not affected, 2 unknown");
    const Row* l1tf = findRow(section, "L1TF");
    ASSERT_NE(l1tf, nullptr);
    EXPECT_EQ(l1tf->value, "Unknown");
    EXPECT_EQ(l1tf->valueTooltip, "Windows didn't report whether this CPU needs this mitigation");
    EXPECT_FALSE(findRow(section, "MDS")->valueTooltip.empty());
    EXPECT_TRUE(findRow(section, "Meltdown")->valueTooltip.empty());
    EXPECT_TRUE(findRow(section, "Spectre v2")->valueTooltip.empty());

    // The Linux kernel's own "Unknown: ..." wording explains itself; no tooltip there.
    security.family = Platform::OsFamily::Linux;
    security.vulnerabilities = {{.name = "mmio_stale_data", .status = "Unknown: No mitigations"}};
    const Section onLinux = SystemInfo::buildSecuritySection(security);
    EXPECT_EQ(findRow(onLinux, "CPU vulnerabilities")->value, "1 unknown");
    EXPECT_TRUE(findRow(onLinux, "MMIO stale data")->valueTooltip.empty());
}

TEST(SystemInfoSectionsTest, SensorRows)
{
    using Platform::SensorKind;
    using Platform::SensorReading;
    EXPECT_EQ(SystemInfo::formatSensorReading(
                  {.kind = SensorKind::Temperature, .label = "t", .id = {}, .value = 52.0, .high = 100.0, .critical = 105.0}),
              "52.0 \u00B0C (high 100.0 \u00B0C, critical 105.0 \u00B0C)");
    EXPECT_EQ(SystemInfo::formatSensorReading(
                  {.kind = SensorKind::Temperature, .label = "t", .id = {}, .value = 38.86, .high = std::nullopt, .critical = 85.0}),
              "38.9 \u00B0C (critical 85.0 \u00B0C)");
    EXPECT_EQ(
        SystemInfo::formatSensorReading({.kind = SensorKind::Fan, .label = "f", .id = {}, .value = 1180.0, .high = {}, .critical = {}}),
        "1180 RPM");
    EXPECT_EQ(
        SystemInfo::formatSensorReading({.kind = SensorKind::Voltage, .label = "v", .id = {}, .value = 12.18, .high = {}, .critical = {}}),
        "12.18 V");
    EXPECT_EQ(
        SystemInfo::formatSensorReading({.kind = SensorKind::Current, .label = "c", .id = {}, .value = 1.2, .high = {}, .critical = {}}),
        "1.20 A");
    EXPECT_EQ(
        SystemInfo::formatSensorReading({.kind = SensorKind::Power, .label = "p", .id = {}, .value = 15.24, .high = {}, .critical = {}}),
        "15.2 W");

    Platform::SensorsInfo sensors;
    sensors.available = true;
    sensors.listed = true;
    sensors.devices = {
        {.name = "coretemp",
         .id = {},
         .readings = {SensorReading{
             .kind = SensorKind::Temperature, .label = "Package id 0", .id = {}, .value = 52.0, .high = {}, .critical = {}}}},
        {.name = "nvme #2",
         .id = {},
         .readings = {SensorReading{
             .kind = SensorKind::Temperature, .label = "Composite", .id = {}, .value = 41.0, .high = {}, .critical = {}}}},
    };
    const Section section = SystemInfo::buildSensorsSection(sensors);
    EXPECT_EQ(section.title, "Sensors");
    ASSERT_EQ(section.rows.size(), 2U);
    EXPECT_EQ(section.rows[0].label, "coretemp: Package id 0");
    EXPECT_EQ(section.rows[0].value, "52.0 \u00B0C");
    EXPECT_EQ(section.rows[1].label, "nvme #2: Composite");
    EXPECT_TRUE(std::ranges::none_of(section.rows, [](const Row& item) { return item.isIdentifier; }));
}

// #1648, slice E: System Information's values, and the text Copy puts on the clipboard, use the
// display locale's punctuation (here de-DE's), so what is copied reads as what is shown.
TEST(SystemInfoSectionsTest, ValuesAndCopyTextUseTheDisplayLocale)
{
    using Platform::SensorKind;
    using Platform::SensorReading;
    const UI::Format::ScopedDisplayPunctuation deDe({.decimalPoint = ',', .thousandsSep = ".", .grouping = "\3"});
    EXPECT_EQ(SystemInfo::formatSensorReading(
                  {.kind = SensorKind::Temperature, .label = "t", .id = {}, .value = 52.0, .high = 100.0, .critical = 105.0}),
              "52,0 °C (high 100,0 °C, critical 105,0 °C)");
    EXPECT_EQ(
        SystemInfo::formatSensorReading({.kind = SensorKind::Voltage, .label = "v", .id = {}, .value = 12.18, .high = {}, .critical = {}}),
        "12,18 V");
    EXPECT_EQ(
        SystemInfo::formatSensorReading({.kind = SensorKind::Current, .label = "c", .id = {}, .value = 1.2, .high = {}, .critical = {}}),
        "1,20 A");
    EXPECT_EQ(
        SystemInfo::formatSensorReading({.kind = SensorKind::Power, .label = "p", .id = {}, .value = 15.24, .high = {}, .critical = {}}),
        "15,2 W");
    EXPECT_EQ(SystemInfo::formatUsbSpeed(480.0), "480 Mbps");
    EXPECT_EQ(SystemInfo::formatUsbSpeed(2500.0), "2,5 Gbps");

    Platform::SensorsInfo sensors;
    sensors.available = true;
    sensors.listed = true;
    sensors.devices = {
        {.name = "coretemp",
         .id = {},
         .readings = {SensorReading{
             .kind = SensorKind::Temperature, .label = "Package id 0", .id = {}, .value = 52.5, .high = {}, .critical = {}}}},
    };
    const std::vector<Section> sections{SystemInfo::buildSensorsSection(sensors)};
    EXPECT_TRUE(SystemInfo::allSectionsText(sections, false).contains("coretemp: Package id 0: 52,5 °C"))
        << SystemInfo::allSectionsText(sections, false);
}

TEST(SystemInfoSectionsTest, NoSensorsIsOneMutedRow)
{
    Platform::SensorsInfo none;
    none.available = true;
    none.listed = true;
    const Section empty = SystemInfo::buildSensorsSection(none);
    ASSERT_EQ(empty.rows.size(), 1U);
    EXPECT_FALSE(empty.rows[0].available());
    EXPECT_TRUE(empty.rows[0].unavailableReason.contains("No hwmon sensors")) << empty.rows[0].unavailableReason;

    none.listed = false;
    const Section unread = SystemInfo::buildSensorsSection(none);
    ASSERT_EQ(unread.rows.size(), 1U);
    EXPECT_TRUE(unread.rows[0].unavailableReason.contains("couldn't be read")) << unread.rows[0].unavailableReason;
}

TEST(SystemInfoSectionsTest, NetworkAdapterRows)
{
    Platform::NetworkAdaptersInfo network;
    network.available = true;
    network.listed = true;
    network.gatewayV4 = "192.168.0.1";
    network.gatewayV4Adapter = "eth0";
    network.gatewayV6 = "fe80::1";
    network.gatewayV6Adapter = "eth0";
    network.dnsRead = true;
    network.dnsServers = {"192.168.0.1", "1.1.1.1"};
    network.searchDomains = {"corp.example.com"};
    Platform::NetworkAdapter eth0;
    eth0.name = "eth0";
    eth0.mac = "a4:5e:60:12:34:56";
    eth0.mtu = 1500;
    eth0.driver = "e1000e";
    eth0.up = true;
    eth0.addresses = {{.address = "fe80::1c2a:3bff:fe4d:5e6f", .prefix = 64, .v6 = true},
                      {.address = "192.168.0.20", .prefix = 24, .v6 = false}};
    Platform::NetworkAdapter wifi;
    wifi.name = "wlp3s0";
    wifi.wireless = true;
    wifi.wifiSignalDbm = -61;
    network.adapters = {eth0, wifi};

    const Section section = SystemInfo::buildNetworkAdaptersSection(network);
    EXPECT_EQ(section.title, "Network adapters");
    EXPECT_EQ(findRow(section, "Default gateway")->value, "192.168.0.1 (eth0)");
    EXPECT_EQ(findRow(section, "IPv6 gateway")->value, "fe80::1 (eth0)");
    EXPECT_EQ(findRow(section, "DNS servers")->value, "192.168.0.1, 1.1.1.1");
    EXPECT_TRUE(findRow(section, "Search domains")->isIdentifier);
    EXPECT_EQ(findRow(section, "eth0")->value, "Up, MTU 1500, driver e1000e");
    EXPECT_EQ(findRow(section, "eth0 addresses")->value, "192.168.0.20/24, fe80::1c2a:3bff:fe4d:5e6f/64"); // IPv4 first
    EXPECT_TRUE(findRow(section, "eth0 MAC address")->isIdentifier);
    EXPECT_EQ(findRow(section, "wlp3s0")->value, "Down, Wi-Fi");
    EXPECT_FALSE(findRow(section, "wlp3s0 addresses")->available());
    EXPECT_EQ(findRow(section, "wlp3s0 MAC address"), nullptr); // none reported
    EXPECT_EQ(findRow(section, "wlp3s0 Wi-Fi signal")->value, "-61 dBm");
    EXPECT_EQ(findRow(section, "eth0 Wi-Fi signal"), nullptr);
    // No link read: the network row says so, and there is no link row.
    EXPECT_FALSE(findRow(section, "wlp3s0 Wi-Fi network")->available());
    EXPECT_EQ(findRow(section, "wlp3s0 Wi-Fi link"), nullptr);
    EXPECT_EQ(findRow(section, "eth0 Wi-Fi network"), nullptr);
}

TEST(SystemInfoSectionsTest, LocalAddressesAreThoseThatCantPlaceTheMachine)
{
    const auto local = [](std::string text, bool v6)
    {
        return SystemInfo::isLocalAddress({.address = std::move(text), .prefix = 0, .v6 = v6});
    };
    for (const char* v4 : {"10.1.2.3",
                           "172.16.0.1",
                           "172.31.255.255",
                           "192.168.1.5",
                           "169.254.10.20",
                           "100.64.0.1",
                           "100.127.255.254",
                           "127.0.0.1",
                           "0.0.0.0"})
    {
        EXPECT_TRUE(local(v4, false)) << v4;
    }
    for (const char* v4 : {"8.8.8.8",
                           "172.15.0.1",
                           "172.32.0.1",
                           "100.63.0.1",
                           "100.128.0.1",
                           "192.169.0.1",
                           "81.2.69.160",
                           "",
                           "1.2.3",
                           "1.2.3.4.5",
                           "256.1.1.1",
                           "a.b.c.d"})
    {
        EXPECT_FALSE(local(v4, false)) << v4 << ": public, or unreadable and so kept hidden";
    }
    for (const char* v6 : {"fe80::1",
                           "fe80::1c2a:3bff:fe4d:5e6f%12",
                           "FE80::1",
                           "febf::1",
                           "fec0::1",
                           "fd12:3456:789a::1",
                           "fc00::1",
                           "::1",
                           "::",
                           "::ffff:192.168.1.5"})
    {
        EXPECT_TRUE(local(v6, true)) << v6;
    }
    for (const char* v6 : {"2a02:6b8::5",
                           "2a02:6b8:0:1:a65e:60ff:fe12:3456",
                           "2001:db8::5",
                           "2002:c000:204::1",
                           "::ffff:8.8.8.8",
                           "::2",
                           "1:2:3:4:5:6:7:8",
                           "",
                           "fe80:::1",
                           "fe80::1::2",
                           "12345::1",
                           "1:2:3:4:5:6:7",
                           "1:2:3:4:5:6:7:8:9",
                           "fe80:",
                           "1.2.3.4::"})
    {
        EXPECT_FALSE(local(v6, true)) << v6 << ": public, or unreadable and so kept hidden";
    }
}

// #2119: a globally routable adapter address is the machine's public address, so it is an identifier: left
// out of the page, Copy and Save report unless identifiers are shown.
TEST(SystemInfoSectionsTest, PublicAdapterAddressesAreIdentifiers)
{
    Platform::NetworkAdaptersInfo network;
    network.available = true;
    network.listed = true;
    Platform::NetworkAdapter wifi;
    wifi.name = "wlan0";
    wifi.up = true;
    wifi.addresses = {{.address = "2a02:6b8:0:1:a65e:60ff:fe12:3456", .prefix = 64, .v6 = true},
                      {.address = "fe80::1", .prefix = 64, .v6 = true},
                      {.address = "192.168.1.5", .prefix = 24, .v6 = false},
                      {.address = "81.2.69.160", .prefix = 28, .v6 = false}};
    Platform::NetworkAdapter vps;
    vps.name = "eth0";
    vps.up = true;
    vps.addresses = {{.address = "81.2.69.161", .prefix = 24, .v6 = false}};
    network.adapters = {wifi, vps};

    const std::vector<Section> sections = {SystemInfo::buildNetworkAdaptersSection(network)};
    EXPECT_EQ(findRow(sections[0], "wlan0 addresses")->value, "192.168.1.5/24, fe80::1/64");
    EXPECT_FALSE(findRow(sections[0], "wlan0 addresses")->isIdentifier);
    const Row* publicRow = findRow(sections[0], "wlan0 public addresses");
    ASSERT_NE(publicRow, nullptr);
    EXPECT_EQ(publicRow->value, "81.2.69.160/28, 2a02:6b8:0:1:a65e:60ff:fe12:3456/64");
    EXPECT_TRUE(publicRow->isIdentifier);
    // Only a public address: no plain row claiming none is assigned.
    EXPECT_EQ(findRow(sections[0], "eth0 addresses"), nullptr);
    EXPECT_TRUE(findRow(sections[0], "eth0 public addresses")->isIdentifier);

    for (const std::string& hidden : {SystemInfo::allSectionsText(sections, false), SystemInfo::allSectionsJson(sections, false, "")})
    {
        EXPECT_TRUE(hidden.contains("192.168.1.5")) << hidden;
        EXPECT_TRUE(hidden.contains("fe80::1/64")) << hidden;
        EXPECT_FALSE(hidden.contains("2a02:6b8")) << hidden;
        EXPECT_FALSE(hidden.contains("81.2.69.16")) << hidden;
    }
    for (const std::string& shown : {SystemInfo::allSectionsText(sections, true), SystemInfo::allSectionsJson(sections, true, "")})
    {
        EXPECT_TRUE(shown.contains("192.168.1.5")) << shown;
        EXPECT_TRUE(shown.contains("2a02:6b8:0:1:a65e:60ff:fe12:3456/64")) << shown;
        EXPECT_TRUE(shown.contains("81.2.69.160/28")) << shown;
        EXPECT_TRUE(shown.contains("81.2.69.161/24")) << shown;
    }
}

// #2191: a public gateway or DHCP server sits in the machine's own public subnet, so it places the machine
// about as well as its address: an identifier. Local gateways and servers stay visible either way.
TEST(SystemInfoSectionsTest, PublicGatewaysAndDhcpServersAreIdentifiers)
{
    Platform::NetworkAdaptersInfo network;
    network.available = true;
    network.listed = true;
    network.gatewayV4 = "81.2.69.1";
    network.gatewayV4Adapter = "eth0";
    network.gatewayV6 = "2a02:6b8::1";
    network.gatewayV6Adapter = "eth0";
    network.dnsRead = true;
    network.dnsServers = {"1.1.1.1"};
    Platform::NetworkAdapter vps;
    vps.name = "eth0";
    vps.up = true;
    vps.gateways = {"192.168.0.1", "2a02:6b8::1", "fe80::1"};
    vps.dhcpLease = Platform::DhcpLease{.server = "81.2.69.2", .leaseSeconds = 86'400, .expiresUnix = 0};
    Platform::NetworkAdapter lan;
    lan.name = "eth1";
    lan.up = true;
    lan.dhcpLease = Platform::DhcpLease{.server = "192.168.0.254", .leaseSeconds = 3'600, .expiresUnix = 0};
    Platform::NetworkAdapter serverOnly;
    serverOnly.name = "eth2";
    serverOnly.dhcpLease = Platform::DhcpLease{.server = "2a02:6b8::2", .leaseSeconds = 0, .expiresUnix = 0};
    network.adapters = {vps, lan, serverOnly};

    const std::vector<Section> sections = {SystemInfo::buildNetworkAdaptersSection(network)};
    EXPECT_TRUE(findRow(sections[0], "Default gateway")->isIdentifier);
    EXPECT_TRUE(findRow(sections[0], "IPv6 gateway")->isIdentifier);
    EXPECT_FALSE(findRow(sections[0], "DNS servers")->isIdentifier); // public resolvers say nothing of the machine
    EXPECT_EQ(findRow(sections[0], "eth0 gateways")->value, "192.168.0.1, fe80::1");
    EXPECT_FALSE(findRow(sections[0], "eth0 gateways")->isIdentifier);
    EXPECT_EQ(findRow(sections[0], "eth0 public gateways")->value, "2a02:6b8::1");
    EXPECT_TRUE(findRow(sections[0], "eth0 public gateways")->isIdentifier);
    EXPECT_EQ(findRow(sections[0], "eth0 DHCP lease")->value, "1 day lease"); // the rest of the lease stays
    EXPECT_FALSE(findRow(sections[0], "eth0 DHCP lease")->isIdentifier);
    EXPECT_EQ(findRow(sections[0], "eth0 DHCP server")->value, "81.2.69.2");
    EXPECT_TRUE(findRow(sections[0], "eth0 DHCP server")->isIdentifier);
    EXPECT_EQ(findRow(sections[0], "eth1 DHCP lease")->value, "From 192.168.0.254, 1 h lease");
    EXPECT_EQ(findRow(sections[0], "eth1 DHCP server"), nullptr);
    EXPECT_EQ(findRow(sections[0], "eth2 DHCP lease"), nullptr); // nothing left but the server
    EXPECT_TRUE(findRow(sections[0], "eth2 DHCP server")->isIdentifier);

    for (const std::string& hidden : {SystemInfo::allSectionsText(sections, false), SystemInfo::allSectionsJson(sections, false, "")})
    {
        EXPECT_FALSE(hidden.contains("81.2.69.")) << hidden;
        EXPECT_FALSE(hidden.contains("2a02:6b8")) << hidden;
        EXPECT_TRUE(hidden.contains("192.168.0.1")) << hidden;
        EXPECT_TRUE(hidden.contains("fe80::1")) << hidden;
        EXPECT_TRUE(hidden.contains("192.168.0.254")) << hidden;
        EXPECT_TRUE(hidden.contains("1 day lease")) << hidden;
        EXPECT_TRUE(hidden.contains("1.1.1.1")) << hidden;
    }
    for (const std::string& shown : {SystemInfo::allSectionsText(sections, true), SystemInfo::allSectionsJson(sections, true, "")})
    {
        EXPECT_TRUE(shown.contains("81.2.69.1 (eth0)")) << shown;
        EXPECT_TRUE(shown.contains("2a02:6b8::1 (eth0)")) << shown;
        EXPECT_TRUE(shown.contains("81.2.69.2")) << shown;
        EXPECT_TRUE(shown.contains("2a02:6b8::2")) << shown;
        EXPECT_TRUE(shown.contains("192.168.0.1")) << shown;
        EXPECT_TRUE(shown.contains("192.168.0.254")) << shown;
    }

    // A local default gateway stays a plain row.
    network.gatewayV4 = "192.168.0.1";
    network.gatewayV6 = "fe80::1%eth0";
    const Section local = SystemInfo::buildNetworkAdaptersSection(network);
    EXPECT_FALSE(findRow(local, "Default gateway")->isIdentifier);
    EXPECT_FALSE(findRow(local, "IPv6 gateway")->isIdentifier);
}

// #2120: with identifiers hidden, the user's login name must not reach the page, Copy or Save report
// through a path: a removable drive's mount point under /media/<user> or a crash under /home/<user>.
TEST(SystemInfoSectionsTest, LoginNameInPathsIsRedactedWhileIdentifiersAreHidden)
{
    Domain::SystemInfoSnapshot read;
    read.version = 1;
    read.os.family = Platform::OsFamily::Linux;
    read.os.userName = "alice";
    read.storage.available = true;
    read.storage.family = Platform::OsFamily::Linux;
    read.storage.disksRead = true;
    Platform::PhysicalDisk disk;
    disk.name = "sdb";
    disk.partitionsRead = true;
    disk.partitions = {{.number = 1,
                        .device = "sdb1",
                        .typeId = "0x0c",
                        .typeName = "FAT32",
                        .offsetBytes = 1'048'576,
                        .sizeBytes = 16ULL << 30U,
                        .mountPoint = "/media/alice/USB"}};
    read.storage.disks = {disk};
    read.storage.volumesRead = true;
    read.storage.volumes = {{.mountPoint = "/media/alice/USB",
                             .label = "USB",
                             .fileSystem = "vfat",
                             .device = "/dev/sdb1",
                             .network = false,
                             .sizeRead = false,
                             .sizeBytes = 0,
                             .freeBytes = 0}};
    read.crashes.available = true;
    read.crashes.family = Platform::OsFamily::Linux;
    read.crashes.listed = true;
    read.crashes.journalRead = true;
    Platform::CrashEvent crash;
    crash.unixSeconds = 1'790'863'402;
    crash.application = "x";
    crash.executable = "/home/alice/bin/x";
    crash.signal = "SIGSEGV";
    crash.pid = 4242;
    read.crashes.events = {crash};

    const std::vector<Section> sections = SystemInfo::buildSystemInfoSections(read);
    for (const std::string& hidden : {SystemInfo::allSectionsText(sections, false), SystemInfo::allSectionsJson(sections, false, "")})
    {
        EXPECT_FALSE(hidden.contains("alice")) << hidden;
        EXPECT_TRUE(hidden.contains("/media/<user>/USB")) << hidden;
        EXPECT_TRUE(hidden.contains("/home/<user>/bin/x, pid 4242")) << hidden; // the rest of the row stays useful
    }
    for (const std::string& shown : {SystemInfo::allSectionsText(sections, true), SystemInfo::allSectionsJson(sections, true, "")})
    {
        EXPECT_TRUE(shown.contains("/media/alice/USB")) << shown;
        EXPECT_TRUE(shown.contains("/home/alice/bin/x")) << shown;
        EXPECT_TRUE(shown.contains("\xE2\x86\x92 /media/alice/USB")) << shown; // the partition row's mount point
    }

    // The page itself: the filter can't pick out the rows that name the user, and they show redacted.
    EXPECT_TRUE(SystemInfo::visibleSections(sections, "alice", false).empty());
    EXPECT_FALSE(SystemInfo::visibleSections(sections, "alice", true).empty());
    const Section& storage = sections[1];
    const Row* volume = findRow(storage, "/media/alice/USB");
    ASSERT_NE(volume, nullptr);
    EXPECT_FALSE(volume->isIdentifier); // redacted, not hidden: the rest of the row names nothing
    EXPECT_EQ(volume->shownLabel(false), "/media/<user>/USB");
    EXPECT_EQ(volume->shownLabel(true), "/media/alice/USB");
    EXPECT_EQ(volume->shownValue(false), volume->value); // nothing to redact in it
    EXPECT_FALSE(findRow(storage, "sdb")->redacted.has_value());
}

TEST(SystemInfoSectionsTest, RedactsTheLoginNameOnlyAsAWholePathComponent)
{
    constexpr auto LINUX = Platform::OsFamily::Linux;
    constexpr auto WINDOWS = Platform::OsFamily::Windows;
    EXPECT_EQ(SystemInfo::redactLoginName("/home/alice", "alice", LINUX), "/home/<user>");
    EXPECT_EQ(SystemInfo::redactLoginName("/run/media/alice/KINGSTON", "alice", LINUX), "/run/media/<user>/KINGSTON");
    EXPECT_EQ(SystemInfo::redactLoginName("x crashed, /home/alice/src/x, pid 1", "alice", LINUX), "x crashed, /home/<user>/src/x, pid 1");
    EXPECT_EQ(SystemInfo::redactLoginName("nfs4 on server:/home/alice, network", "alice", LINUX), "nfs4 on server:/home/<user>, network");
    EXPECT_EQ(SystemInfo::redactLoginName("(/home/alice)", "alice", LINUX), "(/home/<user>)");
    // Linux paths are case-sensitive, and a component only matches whole.
    EXPECT_EQ(SystemInfo::redactLoginName("/home/Alice", "alice", LINUX), "/home/Alice");
    EXPECT_EQ(SystemInfo::redactLoginName("/home/alice2/x /srv/malice /home/alice.old", "alice", LINUX),
              "/home/alice2/x /srv/malice /home/alice.old");
    EXPECT_EQ(SystemInfo::redactLoginName("alice's /home/alice", "alice", LINUX), "alice's /home/<user>"); // not a path
    // A one-letter name only redacts a component that is it: never "a.out", "/bin/bash" or "/var/a2".
    EXPECT_EQ(SystemInfo::redactLoginName("/home/a/a.out, /bin/bash, /var/a2, /media/a", "a", LINUX),
              "/home/<user>/a.out, /bin/bash, /var/a2, /media/<user>");
    // Nothing to redact by.
    EXPECT_EQ(SystemInfo::redactLoginName("/home/alice", "", LINUX), "/home/alice");
    EXPECT_EQ(SystemInfo::redactLoginName("/home/alice", "CONTOSO\\", LINUX), "/home/alice");
    EXPECT_EQ(SystemInfo::redactLoginName("", "alice", LINUX), "");
    // winbind's DOMAIN\user and SSSD's user@domain logins.
    EXPECT_EQ(SystemInfo::redactLoginName("/home/CONTOSO/alice/x", "CONTOSO\\alice", LINUX), "/home/CONTOSO/<user>/x");
    EXPECT_EQ(SystemInfo::redactLoginName("/home/alice@contoso.com/x and /home/alice/y", "alice@contoso.com", LINUX),
              "/home/<user>/x and /home/<user>/y");

    // Windows: DOMAIN\user, ASCII case ignored, either separator.
    EXPECT_EQ(SystemInfo::redactLoginName("C:\\Users\\Alice\\AppData\\x.exe", "CONTOSO\\alice", WINDOWS),
              "C:\\Users\\<user>\\AppData\\x.exe");
    EXPECT_EQ(SystemInfo::redactLoginName("C:/users/ALICE/x", "PC\\alice", WINDOWS), "C:/users/<user>/x");
    EXPECT_EQ(SystemInfo::redactLoginName("D:\\Games\\alice drive", "PC\\alice", WINDOWS), "D:\\Games\\<user> drive");
    // A profile folder Windows gave a suffix is redacted whole; elsewhere a dotted name is another file.
    EXPECT_EQ(SystemInfo::redactLoginName("C:\\Users\\alice.CONTOSO\\x", "CONTOSO\\alice", WINDOWS), "C:\\Users\\<user>\\x");
    EXPECT_EQ(SystemInfo::redactLoginName("C:\\Users\\a.000", "PC\\a", WINDOWS), "C:\\Users\\<user>");
    EXPECT_EQ(SystemInfo::redactLoginName("C:\\Users\\a\\a.exe", "PC\\a", WINDOWS), "C:\\Users\\<user>\\a.exe");
    EXPECT_EQ(SystemInfo::redactLoginName("C:\\Users\\Public\\x, C:\\Windows\\a", "PC\\a", WINDOWS),
              "C:\\Users\\Public\\x, C:\\Windows\\<user>");
}

TEST(SystemInfoSectionsTest, RedactsTheProfileFolderWhateverItIsNamed)
{
    constexpr auto LINUX = Platform::OsFamily::Linux;
    constexpr auto WINDOWS = Platform::OsFamily::Windows;
    // #2197: a profile folder not named after the login, such as one kept through an account rename.
    EXPECT_EQ(SystemInfo::redactHomeDirectory(R"(C:\Users\bob\AppData\x.exe)", R"(C:\Users\bob)", WINDOWS),
              R"(C:\Users\<user>\AppData\x.exe)");
    EXPECT_EQ(SystemInfo::redactHomeDirectory(R"(x crashed, c:/users/BOB, pid 1)", R"(C:\Users\bob\)", WINDOWS),
              "x crashed, c:/users/<user>, pid 1");
    EXPECT_EQ(SystemInfo::redactHomeDirectory(R"(C:\Users\bobby\x, D:\Users\bob)", R"(C:\Users\bob)", WINDOWS),
              R"(C:\Users\bobby\x, D:\Users\bob)");
    EXPECT_EQ(SystemInfo::redactHomeDirectory("/home/bob/x and /home/bob", "/home/bob", LINUX), "/home/<user>/x and /home/<user>");
    // Linux paths are case-sensitive.
    EXPECT_EQ(SystemInfo::redactHomeDirectory("/home/Bob/x /home/bobby", "/home/bob", LINUX), "/home/Bob/x /home/bobby");
    // Nothing to redact by.
    EXPECT_EQ(SystemInfo::redactHomeDirectory("/home/bob", "", LINUX), "/home/bob");
    EXPECT_EQ(SystemInfo::redactHomeDirectory("/home/bob", "/", LINUX), "/home/bob");
    EXPECT_EQ(SystemInfo::redactHomeDirectory("C:\\x", "C:\\", WINDOWS), "C:\\x");
    EXPECT_EQ(SystemInfo::redactHomeDirectory("", "/home/bob", LINUX), "");
}

// #2197: with identifiers hidden, a profile folder not named after the login doesn't reach the page or
// Copy; the login-name rule still covers other paths that name the user.
TEST(SystemInfoSectionsTest, RedactUserPathsCoversAProfileFolderNotNamedAfterTheLogin)
{
    Platform::OsInfo os;
    os.family = Platform::OsFamily::Windows;
    os.userName = "PC\\alice";
    os.homeDirectory = R"(C:\Users\bob)";
    std::vector<Section> sections(1);
    sections[0].rows.resize(3);
    sections[0].rows[0].label = "Crash";
    sections[0].rows[0].value = R"(x.exe crashed, C:\Users\Bob\Downloads\x.exe)";
    sections[0].rows[1].label = R"(D:\Share\alice)";
    sections[0].rows[1].value = "NTFS";
    sections[0].rows[2].label = "Program";
    sections[0].rows[2].value = R"(C:\Users\bobby\x.exe)";
    SystemInfo::redactUserPaths(sections, os);
    const std::vector<Row>& rows = sections[0].rows;
    EXPECT_EQ(rows[0].shownValue(false), R"(x.exe crashed, C:\Users\<user>\Downloads\x.exe)");
    EXPECT_EQ(rows[0].shownValue(true), R"(x.exe crashed, C:\Users\Bob\Downloads\x.exe)");
    EXPECT_EQ(rows[1].shownLabel(false), R"(D:\Share\<user>)");
    EXPECT_FALSE(rows[2].redacted.has_value());
    const std::string hidden = SystemInfo::allSectionsText(sections, false);
    EXPECT_FALSE(hidden.contains(R"(Users\Bob)")) << hidden;
}

// #2120 on Windows: a row's label, value, unavailable reason and tooltip are each redacted, whatever the
// name's case; an identifier row is left alone, since it is hidden whole.
TEST(SystemInfoSectionsTest, RedactUserPathsCoversEveryTextOfARowButNotIdentifiers)
{
    Platform::OsInfo os;
    os.family = Platform::OsFamily::Windows;
    os.userName = "PC\\Alice";
    const auto makeRow = [](std::string label, std::string value, std::string reason = {}, std::string tooltip = {})
    {
        Row made;
        made.label = std::move(label);
        made.value = std::move(value);
        made.unavailableReason = std::move(reason);
        made.valueTooltip = std::move(tooltip);
        return made;
    };
    std::vector<Section> sections(1);
    sections[0].rows = {makeRow(R"(C:\Users\alice\Mount drive)", "NTFS"),
                        makeRow("Crash", "x.exe crashed", {}, R"(c:\users\ALICE\x.exe)"),
                        makeRow("Dump", "", R"(C:\Users\Alice\dump couldn't be read)"),
                        makeRow("User", R"(PC\Alice)"),
                        makeRow("Kernel", R"(C:\Windows\system32\ntoskrnl.exe)")};
    sections[0].rows[3].isIdentifier = true;
    SystemInfo::redactUserPaths(sections, os);
    const std::vector<Row>& rows = sections[0].rows;
    ASSERT_TRUE(rows[0].redacted.has_value());
    EXPECT_EQ(rows[0].shownLabel(false), "C:\\Users\\<user>\\Mount drive");
    EXPECT_EQ(rows[1].shownValueTooltip(false), "c:\\users\\<user>\\x.exe");
    EXPECT_EQ(rows[1].shownValueTooltip(true), "c:\\users\\ALICE\\x.exe");
    EXPECT_EQ(rows[2].shownUnavailableReason(false), "C:\\Users\\<user>\\dump couldn't be read");
    EXPECT_FALSE(rows[3].redacted.has_value()); // an identifier is hidden whole instead
    EXPECT_FALSE(rows[4].redacted.has_value());

    const std::string hidden = SystemInfo::allSectionsText(sections, false);
    EXPECT_FALSE(hidden.contains("lice")) << hidden;
    EXPECT_FALSE(hidden.contains("LICE")) << hidden;
    EXPECT_TRUE(SystemInfo::allSectionsText(sections, true).contains("C:\\Users\\alice\\Mount drive"));
}

TEST(SystemInfoSectionsTest, AnAdaptersOwnDnsSettingsOnlyWhereItHasAny)
{
    Platform::NetworkAdaptersInfo network;
    network.available = true;
    network.listed = true;
    Platform::NetworkAdapter vpn;
    vpn.name = "wg0";
    vpn.linkDnsServers = {"10.0.0.53", "2001:db8::53"};
    vpn.linkSearchDomains = {"corp.example", "~vpn.example"};
    Platform::NetworkAdapter plain;
    plain.name = "eth0";
    network.adapters = {plain, vpn};

    const Section section = SystemInfo::buildNetworkAdaptersSection(network);
    EXPECT_EQ(findRow(section, "wg0 DNS servers")->value, "10.0.0.53, 2001:db8::53");
    EXPECT_FALSE(findRow(section, "wg0 DNS servers")->isIdentifier);
    EXPECT_EQ(findRow(section, "wg0 search domains")->value, "corp.example, ~vpn.example");
    EXPECT_TRUE(findRow(section, "wg0 search domains")->isIdentifier);
    // None of its own: the system-wide servers apply, and no row says otherwise.
    EXPECT_EQ(findRow(section, "eth0 DNS servers"), nullptr);
    EXPECT_EQ(findRow(section, "eth0 search domains"), nullptr);
}

TEST(SystemInfoSectionsTest, DhcpLeaseRowWhereThereIsOne)
{
    using SystemInfo::formatDhcpLease;
    EXPECT_EQ(formatDhcpLease({.server = "192.168.1.1", .leaseSeconds = 86'400, .expiresUnix = 0}), "From 192.168.1.1, 1 day lease");
    EXPECT_EQ(formatDhcpLease({.server = "", .leaseSeconds = (3 * 86'400) + 7200, .expiresUnix = 0}), "3 days 2 h lease");
    EXPECT_EQ(formatDhcpLease({.server = "", .leaseSeconds = 5400, .expiresUnix = 0}), "1 h 30 min lease");
    EXPECT_EQ(formatDhcpLease({.server = "", .leaseSeconds = 7200, .expiresUnix = 0}), "2 h lease");
    EXPECT_EQ(formatDhcpLease({.server = "", .leaseSeconds = 600, .expiresUnix = 0}), "10 min lease");
    EXPECT_EQ(formatDhcpLease({.server = "", .leaseSeconds = 45, .expiresUnix = 0}), "45 s lease");
    const std::uint64_t expiry = 1'760'000'000;
    EXPECT_EQ(formatDhcpLease({.server = "10.0.0.1", .leaseSeconds = 0, .expiresUnix = expiry}),
              "From 10.0.0.1, expires " + UI::Format::formatEpochDateTime(expiry));

    Platform::NetworkAdaptersInfo network;
    network.available = true;
    network.listed = true;
    Platform::NetworkAdapter dhcp;
    dhcp.name = "eth0";
    dhcp.dhcpLease = Platform::DhcpLease{.server = "192.168.1.1", .leaseSeconds = 86'400, .expiresUnix = 0};
    Platform::NetworkAdapter fixed;
    fixed.name = "eth1";
    network.adapters = {dhcp, fixed};
    const Section section = SystemInfo::buildNetworkAdaptersSection(network);
    EXPECT_EQ(findRow(section, "eth0 DHCP lease")->value, "From 192.168.1.1, 1 day lease");
    EXPECT_EQ(findRow(section, "eth1 DHCP lease"), nullptr);
}

// #1522: a sensor's range since the page first read it, in its unit, once it has moved.
TEST(SystemInfoSectionsTest, SensorRowsShowTheirRangeOnceItHasMoved)
{
    const Platform::SensorReading temperature{.kind = Platform::SensorKind::Temperature,
                                              .label = "Package id 0",
                                              .id = {},
                                              .value = 55.0,
                                              .high = 80.0,
                                              .critical = std::nullopt};
    EXPECT_EQ(SystemInfo::formatSensorRange(temperature, {.min = 41.0, .max = 63.0, .samples = 5}),
              "min " + UI::Format::formatFixedLocalized(41.0, 1, " \u00B0C") + ", max " +
                  UI::Format::formatFixedLocalized(63.0, 1, " \u00B0C"));                                // no thresholds repeated
    EXPECT_EQ(SystemInfo::formatSensorRange(temperature, {.min = 55.0, .max = 55.0, .samples = 5}), ""); // unchanged
    EXPECT_EQ(SystemInfo::formatSensorRange(temperature, {.min = 50.0, .max = 55.0, .samples = 1}), ""); // one read

    const Platform::SensorReading fan{
        .kind = Platform::SensorKind::Fan, .label = "fan1", .id = {}, .value = 1200.0, .high = {}, .critical = {}};
    EXPECT_EQ(SystemInfo::formatSensorRange(fan, {.min = 900.0, .max = 1500.0, .samples = 3}), "min 900 RPM, max 1500 RPM");

    Platform::SensorsInfo sensors;
    sensors.available = true;
    sensors.listed = true;
    sensors.devices.push_back({.name = "coretemp", .id = {}, .readings = {temperature}});
    sensors.devices.push_back({.name = "it8792", .id = {}, .readings = {fan}});
    const std::map<std::string, Domain::SensorRange> ranges{
        {Domain::sensorKey(sensors.devices[0], temperature), {.min = 41.0, .max = 63.0, .samples = 5}},
        {Domain::sensorKey(sensors.devices[1], fan), {.min = 1200.0, .max = 1200.0, .samples = 5}},
    };
    const Section section = SystemInfo::buildSensorsSection(sensors, ranges);
    const auto* temp = findRow(section, "coretemp: Package id 0");
    ASSERT_NE(temp, nullptr);
    EXPECT_TRUE(temp->value.contains(SystemInfo::formatSensorReading(temperature))) << temp->value;
    EXPECT_TRUE(temp->value.contains("min ")) << temp->value;
    EXPECT_EQ(findRow(section, "it8792: fan1")->value, "1200 RPM"); // its range hasn't moved
    // Without ranges (Windows, or before a second read) the rows are as before.
    EXPECT_EQ(findRow(SystemInfo::buildSensorsSection(sensors), "coretemp: Package id 0")->value,
              SystemInfo::formatSensorReading(temperature));
}

TEST(SystemInfoSectionsTest, WifiLinkBandChannelAndRate)
{
    const auto link = [](std::uint32_t mhz, std::uint32_t kbps)
    {
        return SystemInfo::formatWifiLink({
            .ssid = "",
            .frequencyMhz = mhz,
            .bitrateKbps = kbps,
            .signalQuality = std::nullopt,
            .phy = "",
            .channel = 0,
            .rxBitrateKbps = 0,
        });
    };
    EXPECT_EQ(link(2412, 0), "2.4 GHz, channel 1");
    EXPECT_EQ(link(2472, 0), "2.4 GHz, channel 13");
    EXPECT_EQ(link(2484, 0), "2.4 GHz, channel 14"); // Japan's, off the 5 MHz grid
    EXPECT_EQ(link(5180, 0), "5 GHz, channel 36");
    EXPECT_EQ(link(5825, 0), "5 GHz, channel 165");
    EXPECT_EQ(link(5935, 0), "6 GHz, channel 2"); // off the 6 GHz grid
    EXPECT_EQ(link(5955, 0), "6 GHz, channel 1");
    EXPECT_EQ(link(7115, 0), "6 GHz, channel 233");
    EXPECT_EQ(link(58320, 0), "58320 MHz"); // 60 GHz: shown as it is
    EXPECT_EQ(link(5180, 866'700), "5 GHz, channel 36, 866.7 Mbit/s");
    EXPECT_EQ(link(0, 54'000), "54.0 Mbit/s");
    EXPECT_EQ(link(0, 0), "");
}

TEST(SystemInfoSectionsTest, WifiNetworkAndLinkRows)
{
    Platform::NetworkAdaptersInfo network;
    network.available = true;
    network.listed = true;
    Platform::NetworkAdapter wifi;
    wifi.name = "wlp3s0";
    wifi.up = true;
    wifi.wireless = true;
    wifi.wifiLink = Platform::WifiLink{
        .ssid = "Cafe \\xFF",
        .frequencyMhz = 5180,
        .bitrateKbps = 866'700,
        .signalQuality = std::nullopt,
        .phy = "",
        .channel = 0,
        .rxBitrateKbps = 0,
    };
    Platform::NetworkAdapter idle;
    idle.name = "wlan1";
    idle.wireless = true;
    idle.wifiLinkUnavailable = "Not connected";
    network.adapters = {idle, wifi};

    const Section section = SystemInfo::buildNetworkAdaptersSection(network);
    const auto* name = findRow(section, "wlp3s0 Wi-Fi network");
    ASSERT_NE(name, nullptr);
    EXPECT_EQ(name->value, "Cafe \\xFF");
    EXPECT_TRUE(name->isIdentifier); // a network's name can place the machine
    EXPECT_EQ(findRow(section, "wlp3s0 Wi-Fi link")->value, "5 GHz, channel 36, 866.7 Mbit/s");
    EXPECT_FALSE(findRow(section, "wlp3s0 Wi-Fi link")->isIdentifier);

    const auto* idleName = findRow(section, "wlan1 Wi-Fi network");
    ASSERT_NE(idleName, nullptr);
    EXPECT_FALSE(idleName->available());
    EXPECT_EQ(idleName->unavailableReason, "Not connected");
    EXPECT_EQ(findRow(section, "wlan1 Wi-Fi link"), nullptr);
}

// #1782: where the platform gates the Wi-Fi details behind location access, a wireless adapter whose
// details weren't read gets a "Wi-Fi details" row with the "Show Wi-Fi details" button in place of its
// network name; a wired adapter, a disconnected one, and every adapter where they aren't gated don't.
TEST(SystemInfoSectionsTest, ShowWifiDetailsRowOnlyForWirelessAdaptersOnAGatedPlatform)
{
    Platform::NetworkAdaptersInfo network;
    network.available = true;
    network.family = Platform::OsFamily::Windows;
    network.listed = true;
    Platform::NetworkAdapter wifi;
    wifi.name = "Wi-Fi";
    wifi.up = true;
    wifi.wireless = true;
    wifi.wifiSignalDbm = -55;
    wifi.wifiDetailsWithheld = true;
    const auto channelOnly = [](std::string ssid)
    {
        return Platform::WifiLink{
            .ssid = std::move(ssid),
            .frequencyMhz = 0,
            .bitrateKbps = 0,
            .signalQuality = std::nullopt,
            .phy = "",
            .channel = 36,
            .rxBitrateKbps = 0,
        };
    };
    wifi.wifiLink = channelOnly("");
    Platform::NetworkAdapter idle;
    idle.name = "Wi-Fi 2";
    idle.wireless = true;
    idle.wifiLinkUnavailable = "Not connected";
    Platform::NetworkAdapter ethernet;
    ethernet.name = "Ethernet";
    ethernet.up = true;
    network.adapters = {ethernet, wifi, idle};

    const Section gated = SystemInfo::buildNetworkAdaptersSection(network, /*wifiDetailsGated=*/true);
    const Row* details = findRow(gated, "Wi-Fi Wi-Fi details");
    ASSERT_NE(details, nullptr);
    EXPECT_EQ(details->action, SystemInfo::RowAction::ShowWifiDetails);
    EXPECT_FALSE(details->available());
    EXPECT_FALSE(details->isIdentifier) << "the button stays visible with identifiers hidden";
    EXPECT_TRUE(details->unavailableReason.contains("Show Wi-Fi details")) << details->unavailableReason;
    EXPECT_TRUE(details->unavailableReason.contains(SystemInfo::WIFI_DETAILS_NOTE)) << details->unavailableReason;
    EXPECT_EQ(findRow(gated, "Wi-Fi Wi-Fi network"), nullptr);
    EXPECT_EQ(findRow(gated, "Wi-Fi Wi-Fi signal")->value, "-55 dBm") << "the RSSI needs no location access";
    EXPECT_EQ(findRow(gated, "Wi-Fi Wi-Fi link")->value, "channel 36");
    EXPECT_EQ(findRow(gated, "Wi-Fi 2 Wi-Fi details"), nullptr) << "not connected: nothing to show";
    EXPECT_EQ(findRow(gated, "Wi-Fi 2 Wi-Fi network")->unavailableReason, "Not connected");
    EXPECT_EQ(findRow(gated, "Ethernet Wi-Fi details"), nullptr);
    const auto withButton = std::ranges::count(gated.rows, SystemInfo::RowAction::ShowWifiDetails, &Row::action);
    EXPECT_EQ(withButton, 1);
    EXPECT_TRUE(SystemInfo::sectionText(gated, false).contains("Wi-Fi Wi-Fi details: unavailable")) << "Copy says why";

    // Not gated (Linux): no button, whatever the adapter says; the network row as before.
    const Section ungated = SystemInfo::buildNetworkAdaptersSection(network, /*wifiDetailsGated=*/false);
    EXPECT_EQ(std::ranges::count(ungated.rows, SystemInfo::RowAction::ShowWifiDetails, &Row::action), 0);
    EXPECT_NE(findRow(ungated, "Wi-Fi Wi-Fi network"), nullptr);

    // Once read with the details, the network row is back and there is no button.
    wifi.wifiDetailsWithheld = false;
    wifi.wifiLink = channelOnly("HomeNet");
    network.adapters = {ethernet, wifi, idle};
    const Section read = SystemInfo::buildNetworkAdaptersSection(network, /*wifiDetailsGated=*/true);
    EXPECT_EQ(findRow(read, "Wi-Fi Wi-Fi details"), nullptr);
    EXPECT_EQ(findRow(read, "Wi-Fi Wi-Fi network")->value, "HomeNet");

    // Through the page's sections, the capability decides.
    Domain::SystemInfoSnapshot snap = snapshot();
    network.adapters = {ethernet, wifi, idle};
    network.adapters[1].wifiDetailsWithheld = true;
    snap.adapters = network;
    const Platform::SystemInfoCapabilities windows{.hasOs = true, .unavailableReason = {}, .wifiDetailsGated = true};
    const auto sections = SystemInfo::buildSystemInfoSections(snap, {}, windows);
    const auto adapters = std::ranges::find(sections, std::string("Network adapters"), &Section::title);
    ASSERT_NE(adapters, sections.end());
    EXPECT_NE(findRow(*adapters, "Wi-Fi Wi-Fi details"), nullptr);
}

// #1518: the public IP rows: none without a lookup; the "Show public IP" button until asked (not an
// identifier, so it stays visible); then a Public IPv4 and IPv6 row, identifiers both, with the source as
// the tooltip and each missing address's own reason.
TEST(SystemInfoSectionsTest, PublicIpRowsWaitForTheButtonThenAreIdentifiers)
{
    EXPECT_TRUE(SystemInfo::buildPublicIpRows(/*hasPublicIp=*/false, /*included=*/true, {}).empty());

    const std::vector<Row> ask = SystemInfo::buildPublicIpRows(true, false, {});
    ASSERT_EQ(ask.size(), 1U);
    EXPECT_EQ(ask[0].label, "Public IP");
    EXPECT_EQ(ask[0].action, SystemInfo::RowAction::ShowPublicIp);
    EXPECT_FALSE(ask[0].available());
    EXPECT_FALSE(ask[0].isIdentifier) << "the button stays visible with identifiers hidden";
    EXPECT_TRUE(ask[0].unavailableReason.contains(SystemInfo::PUBLIC_IP_NOTE)) << ask[0].unavailableReason;

    const Platform::PublicIpInfo found{.read = true,
                                       .v4 = "203.0.113.7",
                                       .v6 = {},
                                       .v4Unavailable = {},
                                       .v6Unavailable = "No IPv6 route to the internet",
                                       .source = "OpenDNS (myip.opendns.com)"};
    const std::vector<Row> rows = SystemInfo::buildPublicIpRows(true, true, found);
    ASSERT_EQ(rows.size(), 2U);
    EXPECT_EQ(rows[0].label, "Public IPv4");
    EXPECT_EQ(rows[0].value, "203.0.113.7");
    EXPECT_TRUE(rows[0].isIdentifier);
    EXPECT_EQ(rows[0].valueTooltip, "As seen by OpenDNS (myip.opendns.com)");
    EXPECT_EQ(rows[0].action, SystemInfo::RowAction::None);
    EXPECT_EQ(rows[1].label, "Public IPv6");
    EXPECT_FALSE(rows[1].available());
    EXPECT_TRUE(rows[1].isIdentifier);
    EXPECT_EQ(rows[1].unavailableReason, "No IPv6 route to the internet");

    // Through the page's sections: first in Network adapters, where the capability allows.
    Domain::SystemInfoSnapshot snap = snapshot();
    snap.adapters.available = true;
    const Platform::SystemInfoCapabilities withLookup{
        .hasOs = true, .unavailableReason = {}, .wifiDetailsGated = false, .hasPublicIp = true};
    auto sections = SystemInfo::buildSystemInfoSections(snap, {}, withLookup);
    auto adapters = std::ranges::find(sections, std::string("Network adapters"), &Section::title);
    ASSERT_NE(adapters, sections.end());
    ASSERT_FALSE(adapters->rows.empty());
    EXPECT_EQ(adapters->rows[0].action, SystemInfo::RowAction::ShowPublicIp);
    snap.publicIpIncluded = true;
    snap.publicIp = found;
    sections = SystemInfo::buildSystemInfoSections(snap, {}, withLookup);
    adapters = std::ranges::find(sections, std::string("Network adapters"), &Section::title);
    ASSERT_NE(adapters, sections.end());
    EXPECT_EQ(findRow(*adapters, "Public IPv4")->value, "203.0.113.7");
    EXPECT_FALSE(SystemInfo::sectionText(*adapters, false).contains("203.0.113.7")) << "Copy leaves identifiers out";
    const Platform::SystemInfoCapabilities without{.hasOs = true, .unavailableReason = {}, .wifiDetailsGated = false, .hasPublicIp = false};
    sections = SystemInfo::buildSystemInfoSections(snap, {}, without);
    adapters = std::ranges::find(sections, std::string("Network adapters"), &Section::title);
    ASSERT_NE(adapters, sections.end());
    EXPECT_EQ(findRow(*adapters, "Public IPv4"), nullptr);
}

TEST(SystemInfoSectionsTest, NetworkAdaptersUnknownsAndTheStub)
{
    Platform::NetworkAdaptersInfo network;
    network.available = true;
    const Section unread = SystemInfo::buildNetworkAdaptersSection(network);
    EXPECT_FALSE(findRow(unread, "Default gateway")->available());
    EXPECT_FALSE(findRow(unread, "DNS servers")->available());
    EXPECT_EQ(findRow(unread, "IPv6 gateway"), nullptr);
    EXPECT_FALSE(findRow(unread, "Adapters")->available());

    network.listed = true;
    network.dnsRead = true;
    network.dnsServers = {"127.0.0.53"};
    network.dnsIsLocalStub = true;
    const Section stub = SystemInfo::buildNetworkAdaptersSection(network);
    EXPECT_EQ(findRow(stub, "DNS servers")->value, "127.0.0.53 (systemd-resolved; its upstream servers couldn't be read)");
    EXPECT_EQ(findRow(stub, "Adapters")->value, "None found");
}

TEST(SystemInfoSectionsTest, NetworkAdapterWindowsRows)
{
    Platform::NetworkAdaptersInfo network;
    network.available = true;
    network.family = Platform::OsFamily::Windows;
    network.listed = true;
    network.dnsRead = true;
    Platform::NetworkAdapter wifi;
    wifi.name = "Wi-Fi";
    wifi.description = "Intel(R) Wi-Fi 6E AX211 160MHz";
    wifi.up = true;
    wifi.wireless = true;
    wifi.mtu = 1500;
    wifi.gateways = {"192.168.0.1", "fe80::1"};
    wifi.linkDnsServers = {"192.168.0.1"};
    wifi.linkSearchDomains = {"home.example"};
    wifi.dhcpEnabled = true;
    wifi.dhcpLease = Platform::DhcpLease{.server = "192.168.0.1", .leaseSeconds = 0, .expiresUnix = 1'760'000'000};
    wifi.wifiSignalDbm = -55;
    wifi.wifiLink = Platform::WifiLink{
        .ssid = "HomeNet",
        .frequencyMhz = 5180,
        .bitrateKbps = 864'500,
        .signalQuality = 82,
        .phy = "802.11ax (Wi-Fi 6)",
        .channel = 0,
        .rxBitrateKbps = 1'201'000,
    };
    Platform::NetworkAdapter ethernet;
    ethernet.name = "Ethernet";
    ethernet.dhcpEnabled = false;
    Platform::NetworkAdapter denied;
    denied.name = "Wi-Fi 2";
    denied.dhcpEnabled = true; // asks DHCP, holds no lease
    denied.wireless = true;
    denied.wifiLinkUnavailable = "Windows refused it: turn on location access";
    // The location refusal still leaves the channel number.
    denied.wifiLink = Platform::WifiLink{
        .ssid = "",
        .frequencyMhz = 0,
        .bitrateKbps = 0,
        .signalQuality = std::nullopt,
        .phy = "",
        .channel = 36,
        .rxBitrateKbps = 0,
    };
    network.adapters = {ethernet, wifi, denied};

    const Section section = SystemInfo::buildNetworkAdaptersSection(network);
    EXPECT_EQ(findRow(section, "Wi-Fi")->value, "Up, Wi-Fi, MTU 1500, Intel(R) Wi-Fi 6E AX211 160MHz");
    EXPECT_EQ(findRow(section, "Wi-Fi gateways")->value, "192.168.0.1, fe80::1");
    EXPECT_EQ(findRow(section, "Wi-Fi DNS servers")->value, "192.168.0.1");
    EXPECT_TRUE(findRow(section, "Wi-Fi search domains")->isIdentifier);
    EXPECT_EQ(findRow(section, "Wi-Fi DNS suffix"), nullptr); // one set of rows for both platforms
    EXPECT_EQ(findRow(section, "Wi-Fi DHCP lease")->value, "From 192.168.0.1, expires " + UI::Format::formatEpochDateTime(1'760'000'000));
    EXPECT_EQ(findRow(section, "Ethernet DHCP lease")->value, "None: a static IPv4 address");
    EXPECT_EQ(findRow(section, "Wi-Fi DHCP"), nullptr); // one DHCP row for both platforms
    EXPECT_EQ(findRow(section, "Wi-Fi Wi-Fi signal")->value, "-55 dBm (82%)");
    EXPECT_EQ(findRow(section, "Wi-Fi Wi-Fi network")->value, "HomeNet");
    EXPECT_TRUE(findRow(section, "Wi-Fi Wi-Fi network")->isIdentifier);
    EXPECT_EQ(findRow(section, "Wi-Fi Wi-Fi link")->value,
              "802.11ax (Wi-Fi 6), 5 GHz, channel 36, " + UI::Format::formatFixedLocalized(864.5, 1, " Mbit/s") + " transmit, " +
                  UI::Format::formatFixedLocalized(1201.0, 1, " Mbit/s") + " receive");
    EXPECT_EQ(findRow(section, "Ethernet gateways"), nullptr);
    EXPECT_EQ(findRow(section, "Ethernet search domains"), nullptr);
    EXPECT_EQ(findRow(section, "Ethernet DNS servers"), nullptr);
    EXPECT_FALSE(findRow(section, "Wi-Fi 2 Wi-Fi network")->available());
    EXPECT_EQ(findRow(section, "Wi-Fi 2 Wi-Fi network")->unavailableReason, "Windows refused it: turn on location access");
    EXPECT_EQ(findRow(section, "Wi-Fi 2 Wi-Fi link")->value, "channel 36");
    EXPECT_FALSE(findRow(section, "Wi-Fi 2 DHCP lease")->available());
    EXPECT_EQ(findRow(section, "Wi-Fi 2 DHCP lease")->unavailableReason, "DHCP is on, but no lease is held (not connected)");
    EXPECT_FALSE(findRow(section, "Wi-Fi 2 Wi-Fi signal")->available());

    network.adapters.clear();
    network.listed = false;
    const Section failed = SystemInfo::buildNetworkAdaptersSection(network);
    EXPECT_EQ(findRow(failed, "Adapters")->unavailableReason, "GetAdaptersAddresses failed");
}

TEST(SystemInfoSectionsTest, BootPerformanceRows)
{
    EXPECT_EQ(SystemInfo::formatBootDuration(850'000), "850 ms");
    EXPECT_EQ(SystemInfo::formatBootDuration(4'210'000), "4.21 s");
    EXPECT_EQ(SystemInfo::formatBootDuration(72'300'000), "1 min 12.3 s");

    Platform::BootPerformanceInfo boot;
    boot.available = true;
    boot.timingsRead = true;
    boot.finished = true;
    boot.kernelUs = 1'500'000;
    boot.userspaceUs = 2'500'000;
    boot.totalUs = 4'000'000;
    const Section section = SystemInfo::buildBootPerformanceSection(boot);
    EXPECT_EQ(section.title, "Boot performance");
    EXPECT_EQ(findRow(section, "Last boot")->value, "4.00 s");
    EXPECT_FALSE(findRow(section, "Firmware")->available());
    EXPECT_TRUE(findRow(section, "Firmware")->unavailableReason.contains("boot loader")) << findRow(section, "Firmware")->unavailableReason;
    EXPECT_FALSE(findRow(section, "Boot loader")->available());
    EXPECT_EQ(findRow(section, "Kernel")->value, "1.50 s");
    EXPECT_EQ(findRow(section, "Initrd"), nullptr); // no initramfs
    EXPECT_EQ(findRow(section, "Userspace")->value, "2.50 s");
    EXPECT_TRUE(std::ranges::none_of(section.rows, [](const Row& item) { return item.isIdentifier; }));
}

TEST(SystemInfoSectionsTest, BootPerformanceUnreadAndStarting)
{
    Platform::BootPerformanceInfo unread;
    unread.available = true;
    unread.unavailableReason = "This build has no systemd support (it was built without libsystemd)";
    const Section none = SystemInfo::buildBootPerformanceSection(unread);
    ASSERT_EQ(none.rows.size(), 1U);
    EXPECT_FALSE(none.rows[0].available());
    EXPECT_EQ(none.rows[0].unavailableReason, unread.unavailableReason);

    Platform::BootPerformanceInfo starting;
    starting.available = true;
    starting.timingsRead = true;
    starting.kernelUs = 1'000'000;
    const Section early = SystemInfo::buildBootPerformanceSection(starting);
    EXPECT_EQ(findRow(early, "Last boot")->value, "Still starting up");
    EXPECT_EQ(findRow(early, "Userspace")->unavailableReason, "Startup hasn't finished yet");
}

TEST(SystemInfoSectionsTest, WindowsSecurityRows)
{
    Platform::PlatformSecurityInfo security;
    security.available = true;
    security.family = Platform::OsFamily::Windows;
    security.secureBoot = Platform::SecurityFeatureState::On;
    security.tpm = Platform::SecurityFeatureState::On;
    security.tpmVersionMajor = 2;
    security.memoryIntegrity = true;
    security.kernelDmaProtection = false;
    security.bitLockerRead = true;
    security.bitLocker = {{.volume = "C:", .state = Platform::BitLockerState::On}};
    security.vulnerabilitiesRead = true;
    security.vulnerabilities = {
        {.name = "l1tf", .status = "Not affected"},
        {.name = "spec_store_bypass", .status = "Vulnerable: Speculative Store Bypass Disable is off"},
        {.name = "spectre_v2", .status = "Mitigation: Enhanced IBRS"},
    };
    const Section section = SystemInfo::buildSecuritySection(security);
    EXPECT_EQ(findRow(section, "Secure Boot")->value, "On");
    EXPECT_EQ(findRow(section, "TPM")->value, "Present (TPM 2.0)");
    EXPECT_EQ(findRow(section, "Memory integrity (HVCI)")->value, "On");
    EXPECT_EQ(findRow(section, "Kernel DMA Protection")->value, "Off");
    EXPECT_EQ(findRow(section, "BitLocker (C:)")->value, "On"); // read without administrator (#2584)
    EXPECT_EQ(findRow(section, "BitLocker"), nullptr);
    EXPECT_EQ(findRow(section, "CPU vulnerabilities")->value, "1 vulnerable, 1 mitigated, 1 not affected");
    EXPECT_EQ(findRow(section, "L1TF")->value, "Not affected");
    EXPECT_EQ(findRow(section, "Spectre v2")->value, "Mitigation: Enhanced IBRS");
    // The Linux-only rows are left out.
    for (const std::string_view label : {"Security modules", "SELinux", "AppArmor", "Kernel lockdown", "Disk encryption"})
    {
        EXPECT_EQ(findRow(section, label), nullptr) << label;
    }
}

TEST(SystemInfoSectionsTest, WindowsSecurityUnknownsSayWhy)
{
    Platform::PlatformSecurityInfo unread;
    unread.available = true;
    unread.family = Platform::OsFamily::Windows;
    const Section section = SystemInfo::buildSecuritySection(unread);
    EXPECT_TRUE(findRow(section, "Secure Boot")->unavailableReason.contains("registry"))
        << findRow(section, "Secure Boot")->unavailableReason;
    EXPECT_TRUE(findRow(section, "TPM")->unavailableReason.contains("tbs.dll")) << findRow(section, "TPM")->unavailableReason;
    for (const std::string_view label : {"Memory integrity (HVCI)", "Kernel DMA Protection", "CPU vulnerabilities"})
    {
        const Row* item = findRow(section, label);
        ASSERT_NE(item, nullptr) << label;
        EXPECT_FALSE(item->available()) << label;
        EXPECT_FALSE(item->unavailableReason.empty()) << label;
    }
    EXPECT_EQ(findRow(section, "CPU vulnerabilities")->unavailableReason, "Windows didn't report its speculation control settings");
    EXPECT_EQ(findRow(section, "BitLocker")->unavailableReason, "Windows couldn't list the drives");

    unread.vulnerabilitiesRead = true;
    unread.bitLockerRead = true;
    const Section none = SystemInfo::buildSecuritySection(unread);
    EXPECT_EQ(findRow(none, "CPU vulnerabilities")->unavailableReason, "Windows reports none");
    EXPECT_EQ(findRow(none, "BitLocker")->unavailableReason, "No fixed drives");
}

// #2585: one "Disk encryption" row per open dm-crypt volume on Linux, the counterpart of the BitLocker
// rows; "None" when there are none, unavailable with the reason when /sys/block couldn't be read.
TEST(SystemInfoSectionsTest, LinuxDiskEncryptionRows)
{
    Platform::PlatformSecurityInfo security;
    security.available = true;
    security.family = Platform::OsFamily::Linux;
    const Section unread = SystemInfo::buildSecuritySection(security);
    const Row* unknown = findRow(unread, "Disk encryption");
    ASSERT_NE(unknown, nullptr);
    EXPECT_FALSE(unknown->available()); // unknown, not "None"
    EXPECT_EQ(unknown->unavailableReason, "/sys/block couldn't be read");

    security.encryptedVolumesRead = true;
    const Section none = SystemInfo::buildSecuritySection(security);
    ASSERT_NE(findRow(none, "Disk encryption"), nullptr);
    EXPECT_EQ(findRow(none, "Disk encryption")->value, "None");
    EXPECT_FALSE(findRow(none, "Disk encryption")->valueTooltip.empty());

    security.encryptedVolumes = {
        {.name = "dm_crypt-0", .type = "LUKS2", .device = "nvme0n1p3", .mountPoints = {"/", "/home"}},
        {.name = "cryptswap", .type = "Plain", .device = {}, .mountPoints = {}},
    };
    const Section section = SystemInfo::buildSecuritySection(security);
    EXPECT_EQ(findRow(section, "Disk encryption"), nullptr);
    const Row* root = findRow(section, "Disk encryption (nvme0n1p3)");
    ASSERT_NE(root, nullptr);
    EXPECT_EQ(root->value, "LUKS2: /, /home");
    EXPECT_EQ(root->valueTooltip, "Unlocked as /dev/mapper/dm_crypt-0");
    const Row* swap = findRow(section, "Disk encryption (cryptswap)"); // no device: named by its mapper name
    ASSERT_NE(swap, nullptr);
    EXPECT_EQ(swap->value, "Plain: not mounted");
    EXPECT_EQ(findRow(section, "BitLocker"), nullptr); // Windows-only
}

// #2584: every BitLocker state Windows reports, one row per fixed drive in the order read; Unknown says why.
TEST(SystemInfoSectionsTest, WindowsBitLockerRowPerDriveInEveryState)
{
    using Platform::BitLockerState;
    Platform::PlatformSecurityInfo security;
    security.available = true;
    security.family = Platform::OsFamily::Windows;
    security.bitLockerRead = true;
    const std::vector<std::pair<BitLockerState, std::string_view>> states = {
        {BitLockerState::On, "On"},
        {BitLockerState::Off, "Off"},
        {BitLockerState::Encrypting, "Encrypting"},
        {BitLockerState::Decrypting, "Decrypting"},
        {BitLockerState::Suspended, "Suspended"},
        {BitLockerState::Locked, "Locked"},
        {BitLockerState::NotEncryptable, "Not encryptable"},
        {BitLockerState::WaitingForActivation, "Waiting for activation"},
        {BitLockerState::Unknown, "Unknown"},
    };
    char letter = 'C';
    for (const auto& [state, text] : states)
    {
        EXPECT_EQ(SystemInfo::bitLockerStateText(state), text);
        security.bitLocker.push_back({.volume = std::string{letter, ':'}, .state = state});
        ++letter;
    }
    const Section section = SystemInfo::buildSecuritySection(security);
    letter = 'C';
    for (const auto& [state, text] : states)
    {
        const std::string label = std::string("BitLocker (") + letter + ":)";
        const Row* item = findRow(section, label);
        ASSERT_NE(item, nullptr) << label;
        EXPECT_EQ(item->value, text) << label;
        EXPECT_EQ(item->valueTooltip.empty(), state != BitLockerState::Unknown) << label;
        ++letter;
    }
    EXPECT_EQ(findRow(section, "BitLocker (K:)")->valueTooltip, "Windows doesn't report BitLocker for this drive");
    // Never asks for administrator any more.
    for (const Row& item : section.rows)
    {
        EXPECT_FALSE(item.unavailableReason.contains("administrator")) << item.label;
    }
}

// #2584: Linux has no BitLocker rows.
TEST(SystemInfoSectionsTest, LinuxSecurityHasNoBitLockerRows)
{
    Platform::PlatformSecurityInfo security;
    security.available = true;
    security.family = Platform::OsFamily::Linux;
    const Section section = SystemInfo::buildSecuritySection(security);
    for (const Row& item : section.rows)
    {
        EXPECT_FALSE(item.label.starts_with("BitLocker")) << item.label;
    }
}

TEST(SystemInfoSectionsTest, WindowsSensorsRowsAndVendorDriverNote)
{
    Platform::SensorsInfo sensors;
    sensors.available = true;
    sensors.family = Platform::OsFamily::Windows;
    sensors.listed = true;
    sensors.devices = {
        {
            .name = "Thermal zones",
            .id = {},
            .readings =
                {
                    {.kind = Platform::SensorKind::Temperature, .label = "TZ00", .id = {}, .value = 45.0, .high = {}, .critical = {}},
                },
        },
    };
    const Section section = SystemInfo::buildSensorsSection(sensors);
    ASSERT_EQ(section.rows.size(), 2U);
    EXPECT_EQ(section.rows[0].label, "Thermal zones: TZ00");
    EXPECT_EQ(section.rows[0].value, "45.0 \u00B0C");
    EXPECT_EQ(section.rows[1].label, "Other sensors");
    EXPECT_TRUE(section.rows[1].value.contains("need a vendor driver")) << section.rows[1].value;

    sensors.devices.clear();
    const Section none = SystemInfo::buildSensorsSection(sensors);
    ASSERT_EQ(none.rows.size(), 2U);
    EXPECT_FALSE(none.rows[0].available());
    EXPECT_TRUE(none.rows[0].unavailableReason.contains("No ACPI thermal zones")) << none.rows[0].unavailableReason;
    EXPECT_EQ(none.rows[1].label, "Other sensors");

    sensors.listed = false;
    const Section unread = SystemInfo::buildSensorsSection(sensors);
    EXPECT_TRUE(unread.rows[0].unavailableReason.contains("Thermal Zone Information")) << unread.rows[0].unavailableReason;
}

TEST(SystemInfoSectionsTest, WindowsBootPerformanceRows)
{
    Platform::BootPerformanceInfo boot;
    boot.available = true;
    boot.family = Platform::OsFamily::Windows;
    boot.timingsRead = true;
    boot.finished = true;
    boot.totalUs = 34'567'000;
    boot.mainPathUs = 14'200'000;
    boot.postBootUs = 20'367'000;
    boot.recordedUnixSeconds = 1'791'529'361;
    const Section section = SystemInfo::buildBootPerformanceSection(boot);
    ASSERT_EQ(section.rows.size(), 4U);
    EXPECT_EQ(findRow(section, "Last boot")->value, "34.57 s");
    EXPECT_EQ(findRow(section, "Recorded")->value, UI::Format::formatEpochDateTime(1'791'529'361));
    EXPECT_EQ(findRow(section, "Main path (to the desktop)")->value, "14.20 s");
    EXPECT_EQ(findRow(section, "Post-boot (desktop to idle)")->value, "20.37 s");
    EXPECT_EQ(findRow(section, "Firmware"), nullptr); // the systemd phases are Linux only
    EXPECT_EQ(findRow(section, "Userspace"), nullptr);

    Platform::BootPerformanceInfo denied;
    denied.available = true;
    denied.family = Platform::OsFamily::Windows;
    denied.unavailableReason = "Requires administrator (the Diagnostics-Performance event log is readable by administrators only)";
    const Section admin = SystemInfo::buildBootPerformanceSection(denied);
    ASSERT_EQ(admin.rows.size(), 1U);
    EXPECT_FALSE(admin.rows[0].available());
    EXPECT_EQ(admin.rows[0].unavailableReason, denied.unavailableReason);
}
TEST(SystemInfoSectionsTest, CommitPagingUnreadableLinuxFilesAreMuted)
{
    Platform::CommitPagingInfo unread;
    unread.available = true;
    unread.family = Platform::OsFamily::Linux;
    const Section section = SystemInfo::buildCommitPagingSection(unread);
    for (const std::string_view label :
         {"Commit charge", "Overcommit mode", "Swap", "zram", "zswap", "Huge pages", "Transparent huge pages"})
    {
        const Row* item = findRow(section, label);
        ASSERT_NE(item, nullptr) << label;
        EXPECT_FALSE(item->available()) << label;
        EXPECT_FALSE(item->unavailableReason.empty()) << label;
    }

    // Read, but nothing configured, says so.
    unread.pageFilesRead = true;
    unread.zramRead = true;
    unread.hugePagesRead = true;
    unread.hugePageSizeBytes = 2 * MIB;
    const Section empty = SystemInfo::buildCommitPagingSection(unread);
    EXPECT_EQ(findRow(empty, "Swap")->value, "None configured");
    EXPECT_EQ(findRow(empty, "zram")->value, "None");
    EXPECT_EQ(findRow(empty, "Huge pages")->value, "None reserved (2 MiB pages)");
}

TEST(SystemInfoSectionsTest, CommitPagingSectionFollowsMemoryModules)
{
    Domain::SystemInfoSnapshot all = snapshot();
    all.firmware = firmware();
    all.memory = memory();
    all.paging = windowsPaging();
    const auto sections = SystemInfo::buildSystemInfoSections(all);
    ASSERT_EQ(sections.size(), 4U);
    EXPECT_EQ(sections[2].title, "Memory modules");
    EXPECT_EQ(sections[3].title, "Commit & paging");
}

[[nodiscard]] Platform::StorageInfo windowsStorage()
{
    Platform::StorageInfo info;
    info.available = true;
    info.family = Platform::OsFamily::Windows;
    info.disksRead = true;
    Platform::PhysicalDisk nvme;
    nvme.name = "Disk 0";
    nvme.model = "Samsung SSD 980 PRO 1TB";
    nvme.bus = "NVMe";
    nvme.media = Platform::DiskMedia::Ssd;
    nvme.sizeBytes = 931 * GIB;
    nvme.firmware = "5B2QGXA7";
    nvme.serial = "S5GXNX0R123456";
    nvme.temperatureCelsius = 41;
    nvme.health = Platform::NvmeHealth{.criticalWarning = 0, .availableSparePercent = 100, .percentageUsed = 3, .mediaErrors = 0};
    info.disks.push_back(nvme);
    Platform::PhysicalDisk usb;
    usb.name = "Disk 1";
    usb.model = "SanDisk Ultra";
    usb.bus = "USB";
    usb.sizeBytes = 64 * GIB;
    info.disks.push_back(usb);
    info.volumesRead = true;
    info.volumes.push_back({
        .mountPoint = "C:",
        .label = "Windows",
        .fileSystem = "NTFS",
        .device = {},
        .network = false,
        .sizeRead = true,
        .sizeBytes = 400 * GIB,
        .freeBytes = 100 * GIB,
    });
    info.volumes.push_back({
        .mountPoint = "Z:",
        .label = {},
        .fileSystem = {},
        .device = {},
        .network = true,
        .sizeRead = false,
        .sizeBytes = 0,
        .freeBytes = 0,
    });
    return info;
}

TEST(SystemInfoSectionsTest, FormatsStorageValues)
{
    const Platform::StorageInfo storage = windowsStorage();
    EXPECT_EQ(SystemInfo::formatDisk(storage.disks[0]),
              "Samsung SSD 980 PRO 1TB, NVMe SSD, 931 GiB, firmware 5B2QGXA7, 41 \xC2\xB0"
              "C");
    EXPECT_EQ(SystemInfo::formatDisk(storage.disks[1]), "SanDisk Ultra, USB, 64 GiB"); // media unknown
    EXPECT_EQ(SystemInfo::formatDisk(Platform::PhysicalDisk{}), "");
    EXPECT_EQ(SystemInfo::formatNvmeHealth({.criticalWarning = 0, .availableSparePercent = 100, .percentageUsed = 3, .mediaErrors = 0}),
              "3% used, 100% spare left, 0 media errors");
    EXPECT_EQ(SystemInfo::formatNvmeHealth({.criticalWarning = 4, .availableSparePercent = 9, .percentageUsed = 101, .mediaErrors = 2}),
              "Critical warning (0x04), 101% used, 9% spare left, 2 media errors");
    EXPECT_EQ(SystemInfo::formatAtaHealth({.failing = false, .badSectors = 0, .powerOnHours = 12345}),
              "SMART OK, 0 bad sectors, 12345 power-on hours");
    EXPECT_EQ(SystemInfo::formatAtaHealth({.failing = true, .badSectors = 1, .powerOnHours = std::nullopt}),
              "Failing (the drive's SMART assessment predicts failure), 1 bad sector");
    EXPECT_EQ(SystemInfo::formatAtaHealth({}), "SMART OK");
    EXPECT_EQ(SystemInfo::formatVolume(storage.volumes[0]), "Windows, NTFS, 100 GiB free of 400 GiB (75% used)");
    EXPECT_EQ(SystemInfo::formatVolume(storage.volumes[1]), "network, size not read");
    EXPECT_EQ(SystemInfo::formatVolume({.mountPoint = "/home",
                                        .label = {},
                                        .fileSystem = "ext4",
                                        .device = "/dev/sda2",
                                        .network = false,
                                        .sizeRead = true,
                                        .sizeBytes = 8 * GIB,
                                        .freeBytes = 8 * GIB}),
              "ext4 on /dev/sda2, 8 GiB free of 8 GiB (0% used)");
}

TEST(SystemInfoSectionsTest, StorageRowsWindows)
{
    const Section section = SystemInfo::buildStorageSection(windowsStorage());
    EXPECT_EQ(section.title, "Storage");
    ASSERT_NE(findRow(section, "Disk 0"), nullptr);
    EXPECT_EQ(findRow(section, "Disk 0 health")->value, "3% used, 100% spare left, 0 media errors");
    EXPECT_EQ(findRow(section, "Disk 1 health"), nullptr); // not read for a USB drive: no row
    ASSERT_NE(findRow(section, "Disk 0 serial number"), nullptr);
    EXPECT_TRUE(findRow(section, "Disk 0 serial number")->isIdentifier);
    EXPECT_FALSE(findRow(section, "Disk 1 serial number")->available());
    EXPECT_EQ(findRow(section, "C: drive")->value, "Windows, NTFS, 100 GiB free of 400 GiB (75% used)");
    EXPECT_EQ(findRow(section, "Z: drive")->value, "network, size not read");
    for (const Row& item : section.rows)
    {
        EXPECT_EQ(item.isIdentifier, item.label.ends_with("serial number")) << item.label;
    }
    // Serials stay out of Copy until identifiers are shown.
    EXPECT_EQ(SystemInfo::sectionText(section, false).find("S5GXNX0R123456"), std::string::npos);
    EXPECT_NE(SystemInfo::sectionText(section, true).find("S5GXNX0R123456"), std::string::npos);

    // A cloud drive labelled with the account's e-mail: the label moves to its own identifier row.
    Platform::StorageInfo cloud = windowsStorage();
    cloud.volumes[0].label = "someone@example.com - Google Drive";
    const Section withCloud = SystemInfo::buildStorageSection(cloud);
    EXPECT_EQ(findRow(withCloud, "C: drive")->value, "NTFS, 100 GiB free of 400 GiB (75% used)");
    ASSERT_NE(findRow(withCloud, "C: drive label"), nullptr);
    EXPECT_TRUE(findRow(withCloud, "C: drive label")->isIdentifier);
    EXPECT_EQ(SystemInfo::sectionText(withCloud, false).find("someone@"), std::string::npos);
}

TEST(SystemInfoSectionsTest, StorageRowsLinuxAndUnreadable)
{
    Platform::StorageInfo linuxStorage;
    linuxStorage.available = true;
    linuxStorage.family = Platform::OsFamily::Linux;
    linuxStorage.disksRead = true;
    Platform::PhysicalDisk sda;
    sda.name = "sda";
    sda.healthUnavailableReason = "SMART status needs udisks2";
    linuxStorage.disks.push_back(sda);
    const Section section = SystemInfo::buildStorageSection(linuxStorage);
    ASSERT_NE(findRow(section, "sda health"), nullptr);
    EXPECT_FALSE(findRow(section, "sda health")->available());
    EXPECT_EQ(findRow(section, "sda health")->unavailableReason, "SMART status needs udisks2");

    // An ATA drive's SMART status through udisks2 (#1631).
    Platform::StorageInfo smartStorage = linuxStorage;
    smartStorage.disks[0].healthUnavailableReason.clear();
    smartStorage.disks[0].ataHealth = Platform::AtaHealth{.failing = false, .badSectors = 0, .powerOnHours = 100};
    const Section smart = SystemInfo::buildStorageSection(smartStorage);
    ASSERT_NE(findRow(smart, "sda health"), nullptr);
    EXPECT_EQ(findRow(smart, "sda health")->value, "SMART OK, 0 bad sectors, 100 power-on hours");
    EXPECT_FALSE(findRow(section, "sda")->available()); // nothing known about it
    EXPECT_FALSE(findRow(section, "Volumes")->available());
    EXPECT_EQ(findRow(section, "Volumes")->unavailableReason, "/proc/self/mountinfo couldn't be read");

    Platform::StorageInfo none;
    none.available = true;
    none.family = Platform::OsFamily::Linux;
    none.volumesRead = true;
    const Section empty = SystemInfo::buildStorageSection(none);
    EXPECT_FALSE(findRow(empty, "Disks")->available());
    EXPECT_EQ(findRow(empty, "Volumes")->value, "None mounted");
}

TEST(SystemInfoSectionsTest, StoragePartitionRows)
{
    // #1632: Disk 0 is GPT with C: on its third partition; Disk 1's layout was refused.
    Platform::StorageInfo storage = windowsStorage();
    Platform::PhysicalDisk& nvme = storage.disks[0];
    nvme.partitionsRead = true;
    nvme.partitionStyle = Platform::PartitionStyle::Gpt;
    const auto partition =
        [](std::uint32_t number, std::string_view typeName, std::uint64_t offset, std::uint64_t size, std::string_view mount)
    {
        Platform::Partition result;
        result.number = number;
        result.typeName = std::string(typeName);
        result.offsetBytes = offset;
        result.sizeBytes = size;
        result.mountPoint = std::string(mount);
        return result;
    };
    nvme.partitions = {
        partition(1, "EFI System", MIB, 260 * MIB, ""),
        partition(2, "Microsoft reserved", 261 * MIB, 16 * MIB, ""),
        partition(3, "Basic data", 277 * MIB, 930 * GIB, "C:"),
    };
    storage.disks[1].partitionsUnavailableReason = "Requires administrator";

    const Section section = SystemInfo::buildStorageSection(storage);
    ASSERT_NE(findRow(section, "Disk 0 partitions"), nullptr);
    EXPECT_EQ(findRow(section, "Disk 0 partitions")->value, "GPT, 3 partitions");
    EXPECT_EQ(findRow(section, "Disk 0 partition 1")->value, "EFI System, 260 MiB, offset 1 MiB");
    EXPECT_EQ(findRow(section, "Disk 0 partition 3")->value, "Basic data, 930 GiB, offset 277 MiB \xE2\x86\x92 C:");
    ASSERT_NE(findRow(section, "Disk 1 partitions"), nullptr);
    EXPECT_FALSE(findRow(section, "Disk 1 partitions")->available());
    EXPECT_EQ(findRow(section, "Disk 1 partitions")->unavailableReason, "Requires administrator");
    EXPECT_EQ(findRow(section, "Disk 1 partition 1"), nullptr);
    // The partition rows follow their disk's rows, before the volumes.
    const auto at = [&section](std::string_view label)
    {
        return std::ranges::find(section.rows, label, &Row::label) - section.rows.begin();
    };
    EXPECT_LT(at("Disk 0 health"), at("Disk 0 partitions"));
    EXPECT_LT(at("Disk 0 partition 3"), at("Disk 1"));
    EXPECT_LT(at("Disk 1 partitions"), at("C: drive"));
    // No layout facts at all (an older snapshot): no partition rows.
    EXPECT_EQ(findRow(SystemInfo::buildStorageSection(windowsStorage()), "Disk 0 partitions"), nullptr);

    // The summary's other forms.
    Platform::PhysicalDisk disk;
    EXPECT_EQ(SystemInfo::formatPartitionLayout(disk), "");
    disk.partitionsRead = true;
    EXPECT_EQ(SystemInfo::formatPartitionLayout(disk), "No partitions");
    disk.partitionStyle = Platform::PartitionStyle::Raw;
    EXPECT_EQ(SystemInfo::formatPartitionLayout(disk), "Not partitioned");
    disk.partitionStyle = Platform::PartitionStyle::Mbr;
    disk.partitions.push_back(partition(1, "NTFS/exFAT", MIB, GIB, "E:"));
    EXPECT_EQ(SystemInfo::formatPartitionLayout(disk), "MBR, 1 partition");
    disk.partitionStyle = Platform::PartitionStyle::Unknown;
    EXPECT_EQ(SystemInfo::formatPartitionLayout(disk), "1 partition");

    // Linux: the kernel name first; an unnamed type shows its id, an unread one a dash.
    Platform::Partition linuxPartition = partition(2, "", 513 * MIB, 8 * GIB, "/home");
    linuxPartition.device = "sda2";
    EXPECT_EQ(SystemInfo::formatPartition(linuxPartition), "sda2, \xE2\x80\x94, 8 GiB, offset 513 MiB \xE2\x86\x92 /home");
    linuxPartition.typeId = "0x99";
    EXPECT_EQ(SystemInfo::formatPartition(linuxPartition), "sda2, 0x99, 8 GiB, offset 513 MiB \xE2\x86\x92 /home");
    EXPECT_EQ(SystemInfo::formatPartition(partition(1, "EFI System", 0, 100 * MIB, "")), "EFI System, 100 MiB, offset 0 B");

    // User-locale numbers: a partition count past a thousand is grouped.
    const UI::Format::ScopedDisplayPunctuation deDe({.decimalPoint = ',', .thousandsSep = ".", .grouping = "\3"});
    Platform::PhysicalDisk many;
    many.partitionsRead = true;
    many.partitionStyle = Platform::PartitionStyle::Gpt;
    many.partitions.resize(1200);
    EXPECT_EQ(SystemInfo::formatPartitionLayout(many), "GPT, 1.200 partitions");
    EXPECT_EQ(SystemInfo::formatPartition(partition(1, "Basic data", MIB, (3 * GIB) / 2, "")), "Basic data, 1,5 GiB, offset 1 MiB");
}

TEST(SystemInfoSectionsTest, StorageSectionFollowsCommitPaging)
{
    Domain::SystemInfoSnapshot all = snapshot();
    all.paging = windowsPaging();
    all.storage = windowsStorage();
    const auto sections = SystemInfo::buildSystemInfoSections(all);
    ASSERT_EQ(sections.size(), 3U);
    EXPECT_EQ(sections[1].title, "Commit & paging");
    EXPECT_EQ(sections[2].title, "Storage");
}

[[nodiscard]] Platform::GraphicsInfo windowsGraphics()
{
    Platform::GraphicsInfo info;
    info.available = true;
    info.family = Platform::OsFamily::Windows;
    info.adaptersRead = true;
    info.adapters.push_back({
        .name = "NVIDIA GeForce RTX 4070",
        .vendorId = 0x10DE,
        .deviceId = 0x2786,
        .dedicatedBytes = 12ULL << 30U,
        .sharedBytes = 16ULL << 30U,
        .location = "01:00.0",
        .driver = "",
        .driverVersion = "32.0.15.6094",
        .driverDate = "2024-09-05",
        .d3dFeatureLevel = "12_2",
        .vulkanApiVersion = "1.4.303",
        .vulkanDriverVersion = "560.94.0.0",
    });
    info.monitorsRead = true;
    info.vulkanRead = true;
    info.vulkanVersion = "1.4.304";
    Platform::Monitor monitor;
    monitor.name = "DELL U2720Q";
    monitor.serial = "ABC1234";
    monitor.widthMm = 597;
    monitor.heightMm = 336;
    monitor.hasDesktopRect = true;
    monitor.desktopWidth = 3840;
    monitor.desktopHeight = 2160;
    monitor.colorSpace = "BT.2020 PQ";
    monitor.hdr = true;
    monitor.bitsPerColor = 10;
    info.monitors.push_back(monitor);
    return info;
}

[[nodiscard]] Core::GraphicsHostInfo windowsHost()
{
    Core::GraphicsHostInfo host;
    host.glVendor = "NVIDIA Corporation";
    host.glRenderer = "NVIDIA GeForce RTX 4070/PCIe/SSE2";
    host.glVersion = "4.6.0 NVIDIA 560.94";
    host.videoDriver = "windows";
    host.displays.push_back({
        .name = "DELL U2720Q",
        .x = 0,
        .y = 0,
        .width = 3840,
        .height = 2160,
        .pixelWidth = 3840,
        .pixelHeight = 2160,
        .refreshHz = 59.94,
        .contentScale = 1.5F,
        .primary = true,
        .hdrEnabled = true,
    });
    host.displays.push_back({
        .name = "Generic PnP Monitor",
        .x = 3840,
        .y = 0,
        .width = 1920,
        .height = 1080,
        .pixelWidth = 1920,
        .pixelHeight = 1080,
        .refreshHz = 60.0,
        .contentScale = 1.0F,
        .primary = false,
        .hdrEnabled = false,
    });
    return host;
}

/// A Linux adapter: a name, a kernel driver and its module version, a PCI slot.
[[nodiscard]] Platform::GraphicsAdapter
linuxAdapter(std::string name, std::string driver, std::string version = {}, std::string location = {})
{
    Platform::GraphicsAdapter adapter;
    adapter.name = std::move(name);
    adapter.driver = std::move(driver);
    adapter.driverVersion = std::move(version);
    adapter.location = std::move(location);
    return adapter;
}

[[nodiscard]] Platform::Monitor linuxMonitor(std::string name, std::string connector)
{
    Platform::Monitor monitor;
    monitor.name = std::move(name);
    monitor.connector = std::move(connector);
    return monitor;
}

TEST(SystemInfoSectionsTest, FormatsGraphicsValues)
{
    const Platform::GraphicsInfo graphics = windowsGraphics();
    EXPECT_EQ(SystemInfo::formatAdapter(graphics.adapters[0]), "NVIDIA GeForce RTX 4070, 12 GiB dedicated, 16 GiB shared, PCI 01:00.0");
    EXPECT_EQ(SystemInfo::formatAdapterDriver(graphics.adapters[0]), "32.0.15.6094 (2024-09-05)");
    EXPECT_EQ(SystemInfo::formatAdapterDriver(linuxAdapter("", "nvidia", "560.35.03")), "nvidia 560.35.03");
    EXPECT_EQ(SystemInfo::formatAdapterDriver(linuxAdapter("", "amdgpu")), "amdgpu");
    EXPECT_EQ(SystemInfo::formatAdapterDriver({}), "");

    const Core::GraphicsHostInfo host = windowsHost();
    EXPECT_EQ(SystemInfo::formatDisplay(host.displays[0], graphics.monitors.data()),
              "DELL U2720Q, 3840 \xC3\x97 2160 at 59.94 Hz, 150% scale, 27.0\" (597 \xC3\x97 336 mm), HDR (BT.2020 PQ, 10-bit), primary");
    EXPECT_EQ(SystemInfo::formatDisplay(host.displays[1], nullptr), "Generic PnP Monitor, 1920 \xC3\x97 1080 at 60 Hz, 100% scale");
    Core::DisplayInfo hdrOnly = host.displays[1];
    hdrOnly.hdrEnabled = true;
    EXPECT_TRUE(SystemInfo::formatDisplay(hdrOnly, nullptr).ends_with(", HDR"));
}

TEST(SystemInfoSectionsTest, MatchesMonitorsToDisplays)
{
    const Core::GraphicsHostInfo host = windowsHost();
    std::vector<Platform::Monitor> monitors(3);
    monitors[0].hasDesktopRect = true; // the second display's rectangle
    monitors[0].desktopX = 3840;
    monitors[0].desktopWidth = 1920;
    monitors[0].desktopHeight = 1080;
    monitors[1].name = "DELL U2720Q"; // the first display's name
    monitors[2].name = "Elsewhere";
    const auto matches = SystemInfo::matchMonitors(host.displays, monitors);
    ASSERT_EQ(matches.size(), 2U);
    EXPECT_EQ(matches[0], std::optional<std::size_t>{1});
    EXPECT_EQ(matches[1], std::optional<std::size_t>{0});

    // A lone display and a lone monitor without a rectangle (Linux) pair up; one with a rectangle doesn't.
    const std::span<const Core::DisplayInfo> one(host.displays.data(), 1);
    std::vector<Platform::Monitor> lone(1);
    lone[0].connector = "eDP-1";
    EXPECT_EQ(SystemInfo::matchMonitors(one, lone)[0], std::optional<std::size_t>{0});
    lone[0].hasDesktopRect = true;
    lone[0].desktopX = 99;
    EXPECT_FALSE(SystemInfo::matchMonitors(one, lone)[0].has_value());
}

TEST(SystemInfoSectionsTest, GraphicsRowsWindows)
{
    const Section section = SystemInfo::buildGraphicsSection(windowsGraphics(), windowsHost());
    EXPECT_EQ(section.title, "Graphics & displays");
    EXPECT_EQ(findRow(section, "GPU")->value, "NVIDIA GeForce RTX 4070, 12 GiB dedicated, 16 GiB shared, PCI 01:00.0");
    EXPECT_EQ(findRow(section, "GPU driver")->value, "32.0.15.6094 (2024-09-05)");
    EXPECT_EQ(findRow(section, "Direct3D on GPU")->value, "Feature level 12_2");
    EXPECT_EQ(findRow(section, "Vulkan on GPU")->value, "1.4.303 (driver 560.94.0.0)");
    EXPECT_EQ(findRow(section, "Vulkan")->value, "1.4.304");
    EXPECT_EQ(findRow(section, "OpenGL")->value, "4.6.0 NVIDIA 560.94");
    EXPECT_EQ(findRow(section, "OpenGL renderer")->value, "NVIDIA GeForce RTX 4070/PCIe/SSE2 (NVIDIA Corporation)");
    EXPECT_EQ(findRow(section, "Display server"), nullptr);
    ASSERT_NE(findRow(section, "Display 1"), nullptr);
    const Row* serial = findRow(section, "Display 1 serial number");
    ASSERT_NE(serial, nullptr);
    EXPECT_TRUE(serial->isIdentifier);
    EXPECT_EQ(serial->value, "ABC1234");
    EXPECT_EQ(findRow(section, "Display 2")->value, "Generic PnP Monitor, 1920 \xC3\x97 1080 at 60 Hz, 100% scale");
    EXPECT_EQ(findRow(section, "Display 2 serial number"), nullptr); // no monitor facts to give one

    // Without Core's facts (no context, no displays) the rows say why.
    const Section bare = SystemInfo::buildGraphicsSection(windowsGraphics(), {});
    EXPECT_FALSE(findRow(bare, "OpenGL")->available());
    EXPECT_FALSE(findRow(bare, "Displays")->available());
    EXPECT_EQ(findRow(bare, "Monitor 1")->value, "DELL U2720Q, 27.0\" (597 \xC3\x97 336 mm), HDR (BT.2020 PQ, 10-bit)");

    // A second adapter without Direct3D 12 gets a muted dash with the reason (#1636).
    Platform::GraphicsInfo two = windowsGraphics();
    two.adapters.push_back(two.adapters[0]);
    two.adapters[1].d3dFeatureLevel.clear();
    const Section both = SystemInfo::buildGraphicsSection(two, {});
    EXPECT_EQ(findRow(both, "Direct3D on GPU 1")->value, "Feature level 12_2");
    const Row* noD3d = findRow(both, "Direct3D on GPU 2");
    ASSERT_NE(noD3d, nullptr);
    EXPECT_FALSE(noD3d->available());
    EXPECT_TRUE(noD3d->unavailableReason.contains("d3d12.dll"));

    // A second adapter the Vulkan loader doesn't list, then no loader at all: muted dashes and why (#1635).
    two.adapters[1].vulkanApiVersion.clear();
    two.adapters[1].vulkanDriverVersion.clear();
    const Section noDevice = SystemInfo::buildGraphicsSection(two, {});
    EXPECT_EQ(findRow(noDevice, "Vulkan on GPU 1")->value, "1.4.303 (driver 560.94.0.0)");
    const Row* unlisted = findRow(noDevice, "Vulkan on GPU 2");
    ASSERT_NE(unlisted, nullptr);
    EXPECT_FALSE(unlisted->available());
    EXPECT_EQ(unlisted->unavailableReason, "The Vulkan loader lists no device for this GPU");

    Platform::GraphicsInfo noLoader = windowsGraphics();
    noLoader.vulkanVersion.clear();
    noLoader.adapters[0].vulkanApiVersion.clear();
    noLoader.adapters[0].vulkanDriverVersion.clear();
    const Section withoutLoader = SystemInfo::buildGraphicsSection(noLoader, {});
    for (const std::string_view label : std::array<std::string_view, 2>{"Vulkan", "Vulkan on GPU"})
    {
        const Row* vulkan = findRow(withoutLoader, label);
        ASSERT_NE(vulkan, nullptr) << label;
        EXPECT_FALSE(vulkan->available());
        EXPECT_EQ(vulkan->unavailableReason, "No Vulkan loader (vulkan-1.dll / libvulkan.so.1) is installed");
    }
}

TEST(SystemInfoSectionsTest, GraphicsRowsLinuxAndUnreadable)
{
    Platform::GraphicsInfo linuxGraphics;
    linuxGraphics.available = true;
    linuxGraphics.family = Platform::OsFamily::Linux;
    linuxGraphics.adaptersRead = true;
    linuxGraphics.adapters.push_back(linuxAdapter("AMD Radeon RX 6800 XT", "amdgpu", "", "0000:03:00.0"));
    linuxGraphics.adapters.push_back(linuxAdapter("Intel GPU (8086:A7A0)", "i915"));
    linuxGraphics.monitorsRead = true;
    linuxGraphics.monitors.push_back(linuxMonitor("DELL U2720Q", "DP-1"));
    linuxGraphics.monitors.push_back(linuxMonitor("", "eDP-1"));
    linuxGraphics.displayServer = "Wayland";
    Core::GraphicsHostInfo host = windowsHost();
    host.videoDriver = "x11";
    host.displays.resize(1);

    const Section section = SystemInfo::buildGraphicsSection(linuxGraphics, host);
    EXPECT_EQ(findRow(section, "GPU 1")->value, "AMD Radeon RX 6800 XT, PCI 0000:03:00.0");
    EXPECT_EQ(findRow(section, "GPU 2 driver")->value, "i915");
    EXPECT_EQ(findRow(section, "Direct3D on GPU 1"), nullptr); // Windows only
    EXPECT_EQ(findRow(section, "Vulkan"), nullptr);            // not read on Linux yet (#1635)
    EXPECT_EQ(findRow(section, "Vulkan on GPU 1"), nullptr);
    EXPECT_EQ(findRow(section, "Display server")->value, "Wayland (TaskSmack runs through XWayland)");
    EXPECT_TRUE(findRow(section, "Display 1")->value.starts_with("DELL U2720Q, 3840")); // matched by name
    EXPECT_FALSE(findRow(section, "Display 1 serial number")->available());
    EXPECT_FALSE(findRow(section, "Monitor eDP-1")->available()); // no EDID: nothing to say

    Platform::GraphicsInfo unread;
    unread.available = true;
    unread.family = Platform::OsFamily::Linux;
    const Section empty = SystemInfo::buildGraphicsSection(unread, {});
    EXPECT_EQ(findRow(empty, "GPU")->unavailableReason, "/sys/class/drm couldn't be listed");
    EXPECT_FALSE(findRow(empty, "Display server")->available());
}

TEST(SystemInfoSectionsTest, GraphicsSectionFollowsStorage)
{
    Domain::SystemInfoSnapshot all = snapshot();
    all.storage = windowsStorage();
    all.graphics = windowsGraphics();
    const auto sections = SystemInfo::buildSystemInfoSections(all, windowsHost());
    ASSERT_EQ(sections.size(), 3U);
    EXPECT_EQ(sections[1].title, "Storage");
    EXPECT_EQ(sections[2].title, "Graphics & displays");
}

[[nodiscard]] Platform::Device device(std::string vendor, std::string name, std::uint16_t vendorId, std::uint16_t productId)
{
    Platform::Device made;
    made.vendor = std::move(vendor);
    made.name = std::move(name);
    made.vendorId = vendorId;
    made.productId = productId;
    return made;
}

[[nodiscard]] Platform::DevicesInfo windowsDevices()
{
    Platform::DevicesInfo info;
    info.available = true;
    info.family = Platform::OsFamily::Windows;
    info.pciRead = true;
    info.usbRead = true;
    info.audioRead = true;
    info.problemsRead = true;
    Platform::Device gpu = device("NVIDIA", "NVIDIA GeForce RTX 4070", 0x10DE, 0x2786);
    gpu.className = "Display adapters";
    gpu.location = "01:00.0";
    gpu.driver = "nvlddmkm";
    Platform::Device wifi = device("Intel Corporation", "Intel(R) Wi-Fi 6E AX211", 0x8086, 0x51F0);
    wifi.className = "Network adapters";
    wifi.problem = "This device cannot start (Code 10)";
    info.pci = {wifi, gpu}; // by location, as the probe gives them
    info.problems = {wifi};
    Platform::Device hub = device("", "Generic USB Hub", 0x05E3, 0x0610);
    hub.depth = 1;
    hub.speedMbps = 20000.0; // a SuperSpeedPlus Gen 2x2 link read from its hub (#1642)
    Platform::Device stick = device("SanDisk", "Ultra", 0x0781, 0x5583);
    stick.depth = 2;
    stick.parent = 0;
    stick.speedMbps = 5000.0;
    stick.serial = "4C530001";
    info.usb = {hub, stick};
    info.audio = {
        {.name = "Speakers (Realtek(R) Audio)", .flow = Platform::AudioFlow::Output},
        {.name = "Microphone (USB Audio)", .flow = Platform::AudioFlow::Input},
    };
    return info;
}

TEST(SystemInfoSectionsTest, FormatsDeviceValues)
{
    EXPECT_EQ(SystemInfo::formatDeviceName(device("Intel Corporation", "Alder Lake-P GT2", 0x8086, 0x46A6)),
              "Intel Corporation Alder Lake-P GT2 (8086:46A6)");
    EXPECT_EQ(SystemInfo::formatDeviceName(device("Intel Corporation", "Intel(R) Wi-Fi 6E AX211", 0x8086, 0x51F0)),
              "Intel(R) Wi-Fi 6E AX211 (8086:51F0)"); // the vendor isn't repeated
    EXPECT_EQ(SystemInfo::formatDeviceName(device("Intel Corporation", "", 0x8086, 0x51F0)), "Intel Corporation device (8086:51F0)");
    EXPECT_EQ(SystemInfo::formatDeviceName(device("", "", 0x1B21, 0x2142)), "Unknown device (1B21:2142)");
    EXPECT_EQ(SystemInfo::formatDeviceName(device("", "Disabled thing", 0, 0)), "Disabled thing");
    EXPECT_EQ(SystemInfo::formatUsbSpeed(1.5), "1.5 Mbps");
    EXPECT_EQ(SystemInfo::formatUsbSpeed(480.0), "480 Mbps");
    EXPECT_EQ(SystemInfo::formatUsbSpeed(5000.0), "5 Gbps");
    EXPECT_EQ(SystemInfo::formatUsbSpeed(12.0), "12 Mbps");
    EXPECT_EQ(SystemInfo::formatUsbSpeed(10000.0), "10 Gbps");
    EXPECT_EQ(SystemInfo::formatUsbSpeed(20000.0), "20 Gbps");
    EXPECT_EQ(SystemInfo::formatUsbSpeed(0.0), "");
}

TEST(SystemInfoSectionsTest, DevicesRowsWindows)
{
    const Section section = SystemInfo::buildDevicesSection(windowsDevices());
    EXPECT_EQ(section.title, "Devices");
    EXPECT_EQ(findRow(section, "Problem devices")->value, "1");
    EXPECT_EQ(findRow(section, "Problem device")->value, "Intel(R) Wi-Fi 6E AX211 (8086:51F0): This device cannot start (Code 10)");
    EXPECT_EQ(findRow(section, "Display adapters")->value, "NVIDIA GeForce RTX 4070 (10DE:2786), PCI 01:00.0, driver nvlddmkm");
    EXPECT_EQ(findRow(section, "Network adapters")->value, "Intel(R) Wi-Fi 6E AX211 (8086:51F0), This device cannot start (Code 10)");
    EXPECT_EQ(findRow(section, "USB 1")->value, "Generic USB Hub (05E3:0610), 20 Gbps");
    EXPECT_EQ(findRow(section, "USB 2")->value, "SanDisk Ultra (0781:5583), 5 Gbps, via Generic USB Hub (05E3:0610)");
    const Row* serial = findRow(section, "USB 2 serial number");
    ASSERT_NE(serial, nullptr);
    EXPECT_TRUE(serial->isIdentifier);
    EXPECT_EQ(findRow(section, "USB 1 serial number"), nullptr);
    EXPECT_EQ(findRow(section, "Audio output")->value, "Speakers (Realtek(R) Audio)");
    EXPECT_EQ(findRow(section, "Audio input")->value, "Microphone (USB Audio)");
    // PCI rows are grouped by class: Display adapters before Network adapters.
    const auto display = std::ranges::find(section.rows, "Display adapters", &Row::label);
    const auto network = std::ranges::find(section.rows, "Network adapters", &Row::label);
    EXPECT_LT(display, network);
}

TEST(SystemInfoSectionsTest, DevicesRowsEmptyAndUnreadable)
{
    Platform::DevicesInfo empty;
    empty.available = true;
    empty.family = Platform::OsFamily::Linux;
    empty.pciRead = true;
    empty.problemsRead = true;
    empty.usbRead = true;
    empty.audioRead = true;
    const Section none = SystemInfo::buildDevicesSection(empty);
    EXPECT_EQ(findRow(none, "Problem devices")->value, "None");
    EXPECT_EQ(findRow(none, "PCI devices")->value, "None found");
    EXPECT_EQ(findRow(none, "USB devices")->value, "None found");
    EXPECT_EQ(findRow(none, "Audio")->value, "No active devices");

    Platform::DevicesInfo unread;
    unread.available = true;
    unread.family = Platform::OsFamily::Linux;
    const Section muted = SystemInfo::buildDevicesSection(unread);
    EXPECT_EQ(findRow(muted, "Problem devices")->unavailableReason, "/sys/bus/pci/devices couldn't be listed");
    EXPECT_EQ(findRow(muted, "USB devices")->unavailableReason, "/sys/bus/usb/devices couldn't be listed");
    EXPECT_FALSE(findRow(muted, "Audio")->available());
    unread.family = Platform::OsFamily::Windows;
    EXPECT_EQ(findRow(SystemInfo::buildDevicesSection(unread), "PCI devices")->unavailableReason, "SetupAPI couldn't list the devices");
}

TEST(SystemInfoSectionsTest, DevicesSectionComesLast)
{
    Domain::SystemInfoSnapshot all = snapshot();
    all.graphics = windowsGraphics();
    all.devices = windowsDevices();
    const auto sections = SystemInfo::buildSystemInfoSections(all, windowsHost());
    ASSERT_EQ(sections.size(), 3U);
    EXPECT_EQ(sections[1].title, "Graphics & displays");
    EXPECT_EQ(sections[2].title, "Devices");
}

[[nodiscard]] Platform::DriversInfo windowsDrivers()
{
    Platform::DriversInfo info;
    info.available = true;
    info.family = Platform::OsFamily::Windows;
    info.listed = true;
    Platform::KernelDriver acpi;
    acpi.name = "ACPI";
    acpi.displayName = "Microsoft ACPI Driver";
    acpi.state = Platform::ServiceState::Running;
    acpi.startType = Platform::ServiceStartType::Boot;
    acpi.version = "10.0.26100.1";
    acpi.company = "Microsoft Corporation";
    acpi.path = R"(C:\Windows\System32\drivers\ACPI.sys)";
    Platform::KernelDriver ntfs;
    ntfs.name = "Ntfs";
    ntfs.displayName = "NTFS";
    ntfs.fileSystem = true;
    ntfs.state = Platform::ServiceState::StopPending;
    ntfs.startType = Platform::ServiceStartType::Boot;
    Platform::KernelDriver beep;
    beep.name = "beep";
    beep.displayName = "Beep";
    beep.state = Platform::ServiceState::Running;
    beep.startType = Platform::ServiceStartType::System;
    info.drivers = {ntfs, beep, acpi};
    return info;
}

[[nodiscard]] Platform::DriversInfo linuxModules()
{
    Platform::DriversInfo info;
    info.available = true;
    info.family = Platform::OsFamily::Linux;
    info.listed = true;
    Platform::KernelDriver nvidia;
    nvidia.name = "nvidia";
    nvidia.moduleState = "Live";
    nvidia.sizeBytes = 2048;
    nvidia.useCount = 49;
    nvidia.usedBy = {"nvidia_uvm", "nvidia_modeset"};
    nvidia.version = "550.54.14";
    nvidia.taints = "POE";
    Platform::KernelDriver vfio;
    vfio.name = "vfio";
    vfio.moduleState = "Loading";
    vfio.useCount = 2;
    vfio.permanent = true;
    Platform::KernelDriver kvm;
    kvm.name = "kvm";
    kvm.moduleState = "Live";
    kvm.useCount = 0;
    info.drivers = {vfio, nvidia, kvm};
    return info;
}

TEST(SystemInfoSectionsTest, DriversRowsWindows)
{
    const Section section = SystemInfo::buildDriversSection(windowsDrivers());
    EXPECT_EQ(section.title, "Drivers");
    EXPECT_EQ(findRow(section, "Loaded drivers")->value, "3");
    EXPECT_EQ(findRow(section, "ACPI")->value,
              "Microsoft ACPI Driver, Boot start, 10.0.26100.1, Microsoft Corporation, C:\\Windows\\System32\\drivers\\ACPI.sys");
    EXPECT_EQ(findRow(section, "Ntfs")->value, "file system driver, Boot start, Stopping"); // same name in another case: not repeated
    EXPECT_EQ(findRow(section, "beep")->value, "System start");
    EXPECT_EQ(findRow(section, "Built-in modules"), nullptr);
    EXPECT_EQ(findRow(section, "Problem drivers")->value, "None");
    // By name, ignoring case, after the counts.
    ASSERT_EQ(section.rows.size(), 5U);
    EXPECT_EQ(section.rows[1].label, "Problem drivers");
    EXPECT_EQ(section.rows[2].label, "ACPI");
    EXPECT_EQ(section.rows[3].label, "beep");
    EXPECT_EQ(section.rows[4].label, "Ntfs");
}

TEST(SystemInfoSectionsTest, DriversProblemRowsWindows)
{
    Platform::DriversInfo info = windowsDrivers();
    info.drivers[0].signature = Platform::DriverSignature::Catalog;  // Ntfs
    info.drivers[1].signature = Platform::DriverSignature::Unsigned; // beep
    info.drivers[1].path = R"(C:\Windows\System32\drivers\beep.sys)";
    info.drivers[2].signature = Platform::DriverSignature::Embedded; // ACPI
    Platform::KernelDriver failed;
    failed.name = "hwbad";
    failed.displayName = "hwbad";
    failed.state = Platform::ServiceState::Stopped;
    failed.startType = Platform::ServiceStartType::Boot;
    failed.startError = "A device attached to the system is not functioning";
    failed.signature = Platform::DriverSignature::Untrusted;
    failed.signatureNote = "A required certificate is not within its validity period";
    Platform::KernelDriver locked;
    locked.name = "locked";
    locked.state = Platform::ServiceState::Running;
    locked.signature = Platform::DriverSignature::Unknown;
    locked.signatureNote = "Access is denied";
    info.drivers.push_back(failed);
    info.drivers.push_back(locked);

    const Section section = SystemInfo::buildDriversSection(info);
    EXPECT_EQ(findRow(section, "Loaded drivers")->value, "4");  // the driver that failed to start isn't loaded
    EXPECT_EQ(findRow(section, "Problem drivers")->value, "2"); // hwbad (twice over) and beep; not the unchecked one
    ASSERT_GE(section.rows.size(), 5U);
    EXPECT_EQ(section.rows[2].label, "Problem driver");
    EXPECT_EQ(section.rows[2].value, "Failed to start: hwbad (A device attached to the system is not functioning)");
    EXPECT_EQ(section.rows[3].value, "Unsigned: beep (C:\\Windows\\System32\\drivers\\beep.sys)");
    EXPECT_EQ(section.rows[4].value, "Untrusted signature: hwbad (A required certificate is not within its validity period)");
    EXPECT_EQ(findRow(section, "beep")->value, "System start, Unsigned, C:\\Windows\\System32\\drivers\\beep.sys");
    EXPECT_EQ(findRow(section, "hwbad")->value,
              "Boot start, Stopped, failed to start (A device attached to the system is not functioning), Untrusted signature");
    EXPECT_EQ(findRow(section, "ACPI")->value,
              "Microsoft ACPI Driver, Boot start, 10.0.26100.1, Microsoft Corporation, C:\\Windows\\System32\\drivers\\ACPI.sys");

    // A signature that couldn't be checked: an em dash whose tooltip says why, right after the driver.
    const Row* unknown = findRow(section, "locked signature");
    ASSERT_NE(unknown, nullptr);
    EXPECT_FALSE(unknown->available());
    EXPECT_EQ(unknown->unavailableReason, "The signature couldn't be checked: Access is denied");
    const auto lockedRow = std::ranges::find(section.rows, std::string("locked"), &Row::label);
    ASSERT_NE(lockedRow, section.rows.end());
    ASSERT_NE(std::next(lockedRow), section.rows.end());
    EXPECT_EQ(std::next(lockedRow)->label, "locked signature");
    EXPECT_EQ(findRow(section, "Ntfs signature"), nullptr);

    // The SCM couldn't list them: no problem count either.
    info.listed = false;
    EXPECT_FALSE(findRow(SystemInfo::buildDriversSection(info), "Problem drivers")->available());
}

TEST(SystemInfoSectionsTest, DriversRowsLinux)
{
    const Section section = SystemInfo::buildDriversSection(linuxModules());
    EXPECT_EQ(section.title, "Kernel modules");
    EXPECT_EQ(findRow(section, "Loaded modules")->value, "3");
    EXPECT_EQ(findRow(section, "Tainted modules")->value, "1");
    EXPECT_TRUE(findRow(section, "Built-in modules")->available());
    EXPECT_EQ(findRow(section, "nvidia")->value, "2.0 KiB, used by nvidia_uvm, nvidia_modeset, version 550.54.14, tainted (POE)");
    EXPECT_EQ(findRow(section, "vfio")->value, "2 references, Loading, permanent");
    EXPECT_EQ(findRow(section, "kvm")->value, "not in use");
}

TEST(SystemInfoSectionsTest, DriversRowsEmptyAndUnreadable)
{
    Platform::DriversInfo none;
    none.available = true;
    none.family = Platform::OsFamily::Windows;
    none.listed = true;
    EXPECT_EQ(findRow(SystemInfo::buildDriversSection(none), "Loaded drivers")->value, "None found");
    none.listed = false;
    EXPECT_EQ(findRow(SystemInfo::buildDriversSection(none), "Loaded drivers")->unavailableReason,
              "The Service Control Manager couldn't list the drivers");
    none.family = Platform::OsFamily::Linux;
    const Section linux = SystemInfo::buildDriversSection(none);
    EXPECT_FALSE(findRow(linux, "Loaded modules")->available());
    EXPECT_EQ(findRow(linux, "Tainted modules"), nullptr);
}

TEST(SystemInfoSectionsTest, DriversSectionComesLast)
{
    Domain::SystemInfoSnapshot all = snapshot();
    all.devices = windowsDevices();
    all.drivers = windowsDrivers();
    const auto sections = SystemInfo::buildSystemInfoSections(all, windowsHost());
    ASSERT_EQ(sections.size(), 3U);
    EXPECT_EQ(sections[1].title, "Devices");
    EXPECT_EQ(sections[2].title, "Drivers");
    EXPECT_NE(SystemInfo::sectionText(sections[2], false).find("ACPI: Microsoft ACPI Driver, Boot start"), std::string::npos);
}

[[nodiscard]] Platform::CrashesInfo windowsCrashes()
{
    Platform::CrashesInfo info;
    info.available = true;
    info.family = Platform::OsFamily::Windows;
    info.listed = true;
    Platform::CrashEvent hang;
    hang.unixSeconds = 1'791'015'300;
    hang.hang = true;
    hang.application = "notepad.exe";
    hang.appVersion = "11.2408.12.0";
    hang.hangType = "Quiesce";
    hang.pid = 16224;
    Platform::CrashEvent crash;
    crash.unixSeconds = 1'790'863'402;
    crash.application = "contoso.exe";
    crash.appVersion = "2.4.0.17";
    crash.module = "ntdll.dll";
    crash.moduleVersion = "10.0.26100.2033";
    crash.exceptionCode = "0xC0000005";
    crash.pid = 6700;
    Platform::CrashEvent bare;
    bare.unixSeconds = 1'790'000'000;
    bare.exceptionCode = "0x12345678";
    info.events = {hang, crash, bare};
    return info;
}

TEST(SystemInfoSectionsTest, CrashesRowsWindows)
{
    const Section section = SystemInfo::buildCrashesSection(windowsCrashes());
    EXPECT_EQ(section.title, "Recent crashes & hangs");
    EXPECT_EQ(findRow(section, "Last 14 days")->value, "2 crashes, 1 hang");
    EXPECT_EQ(findRow(section, "Older"), nullptr);
    ASSERT_EQ(section.rows.size(), 4U);
    EXPECT_EQ(section.rows[1].label, UI::Format::formatEpochDateTime(1'791'015'300)); // newest first, by local time
    EXPECT_EQ(section.rows[1].value, "notepad.exe 11.2408.12.0 hung (Quiesce), pid 16224");
    EXPECT_EQ(section.rows[2].value,
              "contoso.exe 2.4.0.17 crashed in ntdll.dll 10.0.26100.2033, exception 0xC0000005 (access violation), pid 6700");
    EXPECT_EQ(section.rows[3].value, "Unknown application crashed, exception 0x12345678");
}

TEST(SystemInfoSectionsTest, HiddenCrashesOfferAuthenticateToRead)
{
    Platform::CrashesInfo crashes;
    crashes.available = true;
    crashes.family = Platform::OsFamily::Linux;
    crashes.listed = true;
    crashes.journalRead = true;
    crashes.otherUsersHidden = true;
    EXPECT_EQ(findRow(SystemInfo::buildCrashesSection(crashes), "Other users' crashes"), nullptr) << "not without the helper";
    const Section offered = SystemInfo::buildCrashesSection(crashes, /*canAuthenticate=*/true);
    ASSERT_FALSE(offered.rows.empty());
    EXPECT_EQ(offered.rows.front().label, "Other users' crashes");
    EXPECT_EQ(offered.rows.front().action, SystemInfo::RowAction::AuthenticateToRead);

    crashes.otherUsersHidden = false; // root, or in systemd-journal: nothing to offer
    EXPECT_EQ(findRow(SystemInfo::buildCrashesSection(crashes, true), "Other users' crashes"), nullptr);
}

TEST(SystemInfoSectionsTest, CrashesRowsLinuxCappedAndUnreadable)
{
    Platform::CrashesInfo cores;
    cores.available = true;
    cores.family = Platform::OsFamily::Linux;
    cores.listed = true;
    cores.capped = true;
    Platform::CrashEvent core;
    core.unixSeconds = 1'790'863'402;
    core.application = "python3.12";
    core.pid = 4242;
    core.uid = 1000;
    core.coreBytes = 2048;
    cores.events = {core};
    const Section section = SystemInfo::buildCrashesSection(cores);
    EXPECT_EQ(section.title, "Recent crashes");
    EXPECT_EQ(findRow(section, "Last 14 days")->value, "1 core dump");
    EXPECT_EQ(findRow(section, "Older")->unavailableReason, "Not listed: only the 200 newest are shown"); // a muted note
    EXPECT_TRUE(findRow(section, "Source")->available());
    EXPECT_EQ(section.rows.back().value, "python3.12 dumped core, pid 4242, uid 1000, 2.0 KiB");

    // The journal's details (#1674): the signal and the executable, and a crash whose core wasn't kept.
    Platform::CrashEvent detailed = core;
    detailed.coreKept = true;
    detailed.signal = "SIGSEGV";
    detailed.executable = "/usr/bin/python3.12";
    EXPECT_EQ(SystemInfo::formatCrashValue(detailed), "python3.12 dumped core (SIGSEGV), /usr/bin/python3.12, pid 4242, uid 1000, 2.0 KiB");
    Platform::CrashEvent notKept;
    notKept.application = "app";
    notKept.pid = 7;
    notKept.signal = "SIGABRT";
    EXPECT_EQ(SystemInfo::formatCrashValue(notKept), "app crashed (SIGABRT), pid 7, core not kept");

    // #1697: the Source row and the count say what was read.
    EXPECT_EQ(findRow(section, "Source")->value, "systemd-coredump's core files; the journal's details (signal, executable) weren't read");
    Platform::CrashesInfo withJournal = cores;
    withJournal.journalRead = true;
    withJournal.events = {detailed, notKept};
    const Section journal = SystemInfo::buildCrashesSection(withJournal);
    EXPECT_EQ(findRow(journal, "Source")->value, "systemd-coredump's journal entries and core files");
    EXPECT_EQ(findRow(journal, "Last 14 days")->value, "2 crashes"); // one of them kept no core
    withJournal.events.clear();
    EXPECT_EQ(findRow(SystemInfo::buildCrashesSection(withJournal), "Last 14 days")->value, "No crashes");
    Platform::CrashesInfo noJournal = cores;
    noJournal.journalUnavailableReason = "The journal couldn't be opened: Permission denied";
    EXPECT_EQ(findRow(SystemInfo::buildCrashesSection(noJournal), "Source")->value,
              "systemd-coredump's core files; the journal's details weren't read: The journal couldn't be opened: Permission denied");

    Platform::CrashesInfo denied;
    denied.available = true;
    denied.family = Platform::OsFamily::Linux;
    denied.accessDenied = true;
    denied.unavailableReason = "Listing /var/lib/systemd/coredump requires permission";
    const Section unreadable = SystemInfo::buildCrashesSection(denied);
    ASSERT_EQ(unreadable.rows.size(), 1U);
    EXPECT_FALSE(unreadable.rows[0].available());
    EXPECT_EQ(unreadable.rows[0].unavailableReason, "Listing /var/lib/systemd/coredump requires permission");

    Platform::CrashesInfo none;
    none.available = true;
    none.family = Platform::OsFamily::Windows;
    none.listed = true;
    EXPECT_EQ(findRow(SystemInfo::buildCrashesSection(none), "Last 14 days")->value, "No crashes or hangs");
}

TEST(SystemInfoSectionsTest, CrashesSectionComesLast)
{
    Domain::SystemInfoSnapshot all = snapshot();
    all.drivers = windowsDrivers();
    all.crashes = windowsCrashes();
    const auto sections = SystemInfo::buildSystemInfoSections(all, windowsHost());
    ASSERT_EQ(sections.size(), 3U);
    EXPECT_EQ(sections[1].title, "Drivers");
    EXPECT_EQ(sections[2].title, "Recent crashes & hangs");
    EXPECT_NE(SystemInfo::sectionText(sections[2], false).find("crashed in ntdll.dll"), std::string::npos);
}

TEST(SystemInfoSectionsTest, NoSectionsBeforeTheFirstRead)
{
    EXPECT_TRUE(SystemInfo::buildSystemInfoSections(Domain::SystemInfoSnapshot{}).empty());
}

// #1922: the toolbar's read time is never cut off. The box narrows to its hint first, then the time
// drops its "Read at", and only then does it wrap to its own line.
[[nodiscard]] SystemInfo::ToolbarWidths toolbarAt(float rowPx)
{
    return {.rowPx = rowPx,
            .filterWantedPx = 300.0F,
            .filterHintPx = 200.0F,
            .controlsPx = 450.0F,
            .readAtFullPx = 120.0F,
            .readAtShortPx = 60.0F};
}

TEST(SystemInfoToolbarLayoutTest, WideRowKeepsTheBoxAndTheFullTime)
{
    const auto layout = SystemInfo::computeToolbarLayout(toolbarAt(1000.0F));
    EXPECT_EQ(layout.readAt, SystemInfo::ReadAtForm::Full);
    EXPECT_FLOAT_EQ(layout.filterPx, 300.0F);
}

TEST(SystemInfoToolbarLayoutTest, BoxNarrowsBeforeTheTimeShortens)
{
    // 300 + 450 + 120 = 870: 30 px over, taken from the box.
    const auto layout = SystemInfo::computeToolbarLayout(toolbarAt(840.0F));
    EXPECT_EQ(layout.readAt, SystemInfo::ReadAtForm::Full);
    EXPECT_FLOAT_EQ(layout.filterPx, 270.0F);
}

TEST(SystemInfoToolbarLayoutTest, TimeOnlyOnceTheBoxIsDownToItsHint)
{
    // The box at its hint leaves 750 - 200 - 450 = 100 px: not the full 120, but the 60 px time, and
    // the box keeps what the time leaves.
    const auto layout = SystemInfo::computeToolbarLayout(toolbarAt(750.0F));
    EXPECT_EQ(layout.readAt, SystemInfo::ReadAtForm::TimeOnly);
    EXPECT_FLOAT_EQ(layout.filterPx, 240.0F);
}

TEST(SystemInfoToolbarLayoutTest, WrapsWhenNotEvenTheTimeFits)
{
    const auto layout = SystemInfo::computeToolbarLayout(toolbarAt(690.0F));
    EXPECT_EQ(layout.readAt, SystemInfo::ReadAtForm::NextLine);
    EXPECT_FLOAT_EQ(layout.filterPx, 240.0F); // the time is off the row: the box takes what the controls leave
}

TEST(SystemInfoToolbarLayoutTest, ExactFitsEmptyTimesAndBadWidths)
{
    // A row the full form fills to within the cell-clip tolerance keeps it.
    EXPECT_EQ(SystemInfo::computeToolbarLayout(toolbarAt(769.6F)).readAt, SystemInfo::ReadAtForm::Full);

    // No read time (an empty text) fits wherever the box at its hint and the controls do.
    SystemInfo::ToolbarWidths empty = toolbarAt(650.0F);
    empty.readAtFullPx = 0.0F;
    empty.readAtShortPx = 0.0F;
    EXPECT_EQ(SystemInfo::computeToolbarLayout(empty).readAt, SystemInfo::ReadAtForm::Full);

    // NaN widths count as zero, and the box never drops below its hint.
    const float nan = std::numeric_limits<float>::quiet_NaN();
    SystemInfo::ToolbarWidths bad = toolbarAt(nan);
    bad.filterWantedPx = nan;
    bad.controlsPx = nan;
    bad.readAtShortPx = nan;
    const auto layout = SystemInfo::computeToolbarLayout(bad);
    EXPECT_EQ(layout.readAt, SystemInfo::ReadAtForm::NextLine);
    EXPECT_FLOAT_EQ(layout.filterPx, 200.0F);
}

class SystemInfoViewRenderTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Context = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.DisplaySize = ImVec2(1000.0F, 700.0F);
        io.DeltaTime = 1.0F / 60.0F;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        io.Fonts->AddFontDefault();
    }

    void TearDown() override
    {
        ImGui::DestroyContext(m_Context);
    }

    static SystemInfoViewResult runFrame(const std::function<SystemInfoViewResult()>& body, float windowWidth = 1000.0F)
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
        ImGui::SetNextWindowSize(ImVec2(windowWidth, 700.0F));
        ImGui::Begin("System", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
        const SystemInfoViewResult result = body();
        ImGui::End();
        ImGui::Render();
        return result;
    }

  private:
    ImGuiContext* m_Context = nullptr;
};

TEST_F(SystemInfoViewRenderTest, UnsupportedAndLoading)
{
    SystemInfoViewState state;
    const auto unsupported = Mocks::UnsupportedSystemInfoProbe{}.capabilities();
    EXPECT_EQ(runFrame([&] { return renderSystemInfoView(nullptr, unsupported, false, state); }).content,
              SystemInfoViewContent::Unsupported);
    const Platform::SystemInfoCapabilities supported{.hasOs = true, .unavailableReason = {}};
    const Domain::SystemInfoSnapshot empty;
    EXPECT_EQ(runFrame([&] { return renderSystemInfoView(&empty, supported, true, state); }).content, SystemInfoViewContent::Loading);
}

TEST_F(SystemInfoViewRenderTest, SectionsRenderFilterAndToggleIdentifiers)
{
    const Platform::SystemInfoCapabilities supported{.hasOs = true, .unavailableReason = {}};
    const auto snap = snapshot();
    SystemInfoViewState state;
    const auto result = runFrame([&] { return renderSystemInfoView(&snap, supported, false, state); });
    EXPECT_EQ(result.content, SystemInfoViewContent::Sections);
    EXPECT_FALSE(result.refreshRequested);
    EXPECT_GT(ImGui::GetDrawData()->TotalVtxCount, 0);
    ASSERT_EQ(state.visible.size(), 1U);
    const std::size_t hiddenCount = state.visible[0].rows.size();
    EXPECT_FALSE(state.readAtText.empty());

    state.showIdentifiers = true;
    static_cast<void>(runFrame([&] { return renderSystemInfoView(&snap, supported, false, state); }));
    EXPECT_EQ(state.visible[0].rows.size(), hiddenCount + 2);

    state.filter = "directory";
    static_cast<void>(runFrame([&] { return renderSystemInfoView(&snap, supported, false, state); }));
    ASSERT_EQ(state.visible.size(), 1U);
    EXPECT_EQ(state.visible[0].rows.size(), 2U); // System and Windows directory

    state.filter = "nothing matches this";
    static_cast<void>(runFrame([&] { return renderSystemInfoView(&snap, supported, false, state); }));
    EXPECT_TRUE(state.visible.empty());
}

// #1922: as the window narrows the read time shortens, then wraps, rather than running off the edge.
TEST_F(SystemInfoViewRenderTest, ToolbarReadTimeShortensThenWrapsAsTheWindowNarrows)
{
    const Platform::SystemInfoCapabilities supported{.hasOs = true, .unavailableReason = {}};
    const auto snap = snapshot();
    SystemInfoViewState state;
    const auto render = [&](float width, bool reading)
    {
        static_cast<void>(runFrame([&] { return renderSystemInfoView(&snap, supported, reading, state); }, width));
        return state.readAtForm;
    };
    EXPECT_EQ(render(1000.0F, false), SystemInfo::ReadAtForm::Full);
    ASSERT_TRUE(state.readAtText.starts_with("Read at "));
    const std::string fullReadAt = state.readAtText;

    // Narrow a pixel at a time, so the test does not hard-code the default font's glyph widths.
    float timeOnlyWidth = 0.0F;
    float nextLineWidth = 0.0F;
    for (int widthPx = 1000; widthPx >= 300; --widthPx)
    {
        const auto width = static_cast<float>(widthPx);
        const SystemInfo::ReadAtForm form = render(width, false);
        if (form == SystemInfo::ReadAtForm::TimeOnly && timeOnlyWidth == 0.0F)
        {
            timeOnlyWidth = width;
        }
        if (form == SystemInfo::ReadAtForm::NextLine)
        {
            nextLineWidth = width;
            break;
        }
    }
    ASSERT_GT(timeOnlyWidth, 0.0F);
    ASSERT_GT(nextLineWidth, 0.0F);
    // Dropping "Read at " buys about its width before the time has to wrap.
    EXPECT_GT(timeOnlyWidth - nextLineWidth, ImGui::CalcTextSize("Read at ").x - 2.0F);

    // Shortened, the time's tooltip gives the full "Read at ..." (#1891): sweep the toolbar row for it.
    ImGuiStyle& style = ImGui::GetStyle();
    style.HoverStationaryDelay = 0.0F;
    style.HoverDelayShort = 0.0F;
    style.HoverDelayNormal = 0.0F;
    bool shown = false;
    const int rowHeight = static_cast<int>(ImGui::GetFrameHeightWithSpacing());
    for (int y = 2; y <= rowHeight && !shown; y += 3)
    {
        for (int x = 2; x < static_cast<int>(timeOnlyWidth) && !shown; x += 6)
        {
            ImGui::GetIO().AddMousePosEvent(static_cast<float>(x), static_cast<float>(y));
            for (int settle = 0; settle < 2 && !shown; ++settle)
            {
                std::string text;
                static_cast<void>(runFrame(
                    [&]
                    {
                        ImGui::LogToBuffer();
                        const SystemInfoViewResult result = renderSystemInfoView(&snap, supported, false, state);
                        text = GImGui->LogBuffer.c_str();
                        ImGui::LogFinish();
                        return result;
                    },
                    timeOnlyWidth));
                const ImGuiWindow* tooltip = ImGui::FindWindowByName("##Tooltip_00");
                shown = state.readAtForm == SystemInfo::ReadAtForm::TimeOnly && tooltip != nullptr && tooltip->Active &&
                        text.contains(fullReadAt);
            }
        }
    }
    EXPECT_TRUE(shown) << "the shortened time's tooltip: " << fullReadAt;
    ImGui::GetIO().AddMousePosEvent(-FLT_MAX, -FLT_MAX);

    // While a read is in flight "Reading..." has no shorter form: it keeps the row or wraps.
    EXPECT_NE(render(timeOnlyWidth, true), SystemInfo::ReadAtForm::TimeOnly);
    EXPECT_EQ(render(nextLineWidth, true), SystemInfo::ReadAtForm::NextLine);
}

TEST_F(SystemInfoViewRenderTest, CommitPagingSectionRendersAndFilters)
{
    const Platform::SystemInfoCapabilities supported{.hasOs = true, .unavailableReason = {}};
    Domain::SystemInfoSnapshot snap = snapshot();
    snap.paging = windowsPaging();
    snap.paging.compressedBytes.reset(); // an unavailable value draws muted
    SystemInfoViewState state;
    EXPECT_EQ(runFrame([&] { return renderSystemInfoView(&snap, supported, false, state); }).content, SystemInfoViewContent::Sections);
    EXPECT_GT(ImGui::GetDrawData()->TotalVtxCount, 0);
    ASSERT_EQ(state.visible.size(), 2U);

    state.filter = "pagefile";
    static_cast<void>(runFrame([&] { return renderSystemInfoView(&snap, supported, false, state); }));
    ASSERT_EQ(state.visible.size(), 1U);
    EXPECT_EQ(state.visible[0].rows.size(), 1U);
}

TEST_F(SystemInfoViewRenderTest, StorageSectionRendersAndHidesSerials)
{
    const Platform::SystemInfoCapabilities supported{.hasOs = true, .unavailableReason = {}};
    Domain::SystemInfoSnapshot snap = snapshot();
    snap.storage = windowsStorage();
    SystemInfoViewState state;
    EXPECT_EQ(runFrame([&] { return renderSystemInfoView(&snap, supported, false, state); }).content, SystemInfoViewContent::Sections);
    EXPECT_GT(ImGui::GetDrawData()->TotalVtxCount, 0);
    ASSERT_EQ(state.visible.size(), 2U);

    state.filter = "disk 0";
    static_cast<void>(runFrame([&] { return renderSystemInfoView(&snap, supported, false, state); }));
    ASSERT_EQ(state.visible.size(), 1U);
    EXPECT_EQ(state.visible[0].rows.size(), 2U); // the disk and its health; the serial is hidden

    state.showIdentifiers = true;
    static_cast<void>(runFrame([&] { return renderSystemInfoView(&snap, supported, false, state); }));
    ASSERT_EQ(state.visible.size(), 1U);
    EXPECT_EQ(state.visible[0].rows.size(), 3U);
}

TEST_F(SystemInfoViewRenderTest, GraphicsSectionRendersAndHidesMonitorSerials)
{
    const Platform::SystemInfoCapabilities supported{.hasOs = true, .unavailableReason = {}};
    Domain::SystemInfoSnapshot snap = snapshot();
    snap.graphics = windowsGraphics();
    SystemInfoViewState state;
    state.host = windowsHost();
    EXPECT_EQ(runFrame([&] { return renderSystemInfoView(&snap, supported, false, state); }).content, SystemInfoViewContent::Sections);
    EXPECT_GT(ImGui::GetDrawData()->TotalVtxCount, 0);
    ASSERT_EQ(state.visible.size(), 2U);

    state.filter = "display 1";
    static_cast<void>(runFrame([&] { return renderSystemInfoView(&snap, supported, false, state); }));
    ASSERT_EQ(state.visible.size(), 1U);
    EXPECT_EQ(state.visible[0].rows.size(), 1U); // the serial is hidden

    state.showIdentifiers = true;
    static_cast<void>(runFrame([&] { return renderSystemInfoView(&snap, supported, false, state); }));
    ASSERT_EQ(state.visible.size(), 1U);
    EXPECT_EQ(state.visible[0].rows.size(), 2U);
}

TEST_F(SystemInfoViewRenderTest, DevicesSectionRendersAndHidesUsbSerials)
{
    const Platform::SystemInfoCapabilities supported{.hasOs = true, .unavailableReason = {}};
    Domain::SystemInfoSnapshot snap = snapshot();
    snap.devices = windowsDevices();
    SystemInfoViewState state;
    EXPECT_EQ(runFrame([&] { return renderSystemInfoView(&snap, supported, false, state); }).content, SystemInfoViewContent::Sections);
    EXPECT_GT(ImGui::GetDrawData()->TotalVtxCount, 0);
    ASSERT_EQ(state.visible.size(), 2U);

    state.filter = "usb 2";
    static_cast<void>(runFrame([&] { return renderSystemInfoView(&snap, supported, false, state); }));
    ASSERT_EQ(state.visible.size(), 1U);
    EXPECT_EQ(state.visible[0].rows.size(), 1U); // the serial is hidden

    state.showIdentifiers = true;
    static_cast<void>(runFrame([&] { return renderSystemInfoView(&snap, supported, false, state); }));
    ASSERT_EQ(state.visible.size(), 1U);
    EXPECT_EQ(state.visible[0].rows.size(), 2U);
}

TEST_F(SystemInfoViewRenderTest, DriversSectionRendersAndFilters)
{
    const Platform::SystemInfoCapabilities supported{.hasOs = true, .unavailableReason = {}};
    Domain::SystemInfoSnapshot snap = snapshot();
    snap.drivers = linuxModules();
    SystemInfoViewState state;
    EXPECT_EQ(runFrame([&] { return renderSystemInfoView(&snap, supported, false, state); }).content, SystemInfoViewContent::Sections);
    EXPECT_GT(ImGui::GetDrawData()->TotalVtxCount, 0);
    ASSERT_EQ(state.visible.size(), 2U);
    EXPECT_EQ(state.visible[1].rows.size(), 6U);

    state.filter = "tainted";
    static_cast<void>(runFrame([&] { return renderSystemInfoView(&snap, supported, false, state); }));
    ASSERT_EQ(state.visible.size(), 1U);
    EXPECT_EQ(state.visible[0].rows.size(), 2U); // the count and nvidia
}

TEST_F(SystemInfoViewRenderTest, CrashesSectionRendersAndFilters)
{
    const Platform::SystemInfoCapabilities supported{.hasOs = true, .unavailableReason = {}};
    Domain::SystemInfoSnapshot snap = snapshot();
    snap.crashes = windowsCrashes();
    SystemInfoViewState state;
    EXPECT_EQ(runFrame([&] { return renderSystemInfoView(&snap, supported, false, state); }).content, SystemInfoViewContent::Sections);
    EXPECT_GT(ImGui::GetDrawData()->TotalVtxCount, 0);
    ASSERT_EQ(state.visible.size(), 2U);
    EXPECT_EQ(state.visible[1].rows.size(), 4U);

    state.filter = "ntdll";
    static_cast<void>(runFrame([&] { return renderSystemInfoView(&snap, supported, false, state); }));
    ASSERT_EQ(state.visible.size(), 1U);
    EXPECT_EQ(state.visible[0].rows.size(), 1U); // the contoso.exe crash
}

// ========== Sensor histories (#1522) ==========

TEST(SystemInfoSectionsTest, SparklineRangeFitsTheFiniteValues)
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const std::pair<double, double> none{0.0, 0.0};
    const std::array<float, 4> values{40.0F, nan, 60.0F, 50.0F};
    const auto [low, high] = UI::Widgets::sparklineYRange(values).value_or(none);
    EXPECT_DOUBLE_EQ(low, 38.0); // 10% headroom either side of 40..60
    EXPECT_DOUBLE_EQ(high, 62.0);

    const std::array<float, 2> flat{1200.0F, 1200.0F};
    const auto [flatLow, flatHigh] = UI::Widgets::sparklineYRange(flat).value_or(none);
    EXPECT_DOUBLE_EQ(flatLow, 1199.0); // a band of 1 around a flat line
    EXPECT_DOUBLE_EQ(flatHigh, 1201.0);

    const std::array<float, 2> unread{nan, nan};
    EXPECT_FALSE(UI::Widgets::sparklineYRange(unread).has_value());
    EXPECT_FALSE(UI::Widgets::sparklineYRange({}).has_value());
}

/// The sensor sampler's read (#2737) of a package temperature and a fan, with a few seconds of history
/// for the first (Domain::sensorKey()).
[[nodiscard]] Domain::SensorPublication sensorsWithHistory()
{
    Domain::SensorPublication snap;
    snap.version = 1;
    snap.sensors.available = true;
    snap.sensors.listed = true;
    snap.sensors.devices.push_back({.name = "coretemp", .id = {}, .readings = {}});
    snap.sensors.devices[0].readings.push_back(
        {.kind = Platform::SensorKind::Temperature, .label = "Package id 0", .id = {}, .value = 55.0, .high = {}, .critical = {}});
    snap.sensors.devices[0].readings.push_back(
        {.kind = Platform::SensorKind::Fan, .label = "fan1", .id = {}, .value = 1200.0, .high = {}, .critical = {}});
    const std::string key = Domain::sensorKey(snap.sensors.devices[0], snap.sensors.devices[0].readings[0]);
    Domain::SensorRange& range = snap.ranges[key];
    const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    int sample = 0;
    while (sample < 5)
    {
        const double value = 50.0 + sample;
        range.min = sample == 0 ? value : std::min(range.min, value);
        range.max = std::max(range.max, value);
        ++range.samples;
        Domain::appendSensorHistory(snap.histories[key], value, now - 10.0 + (2.0 * sample));
        ++sample;
    }
    return snap;
}

TEST(SystemInfoSectionsTest, SensorRowsNameTheirKindForTheirChart)
{
    const Domain::SensorPublication snap = sensorsWithHistory();
    const Section section = SystemInfo::buildSensorsSection(snap.sensors, snap.ranges);
    const Row* temperature = findRow(section, "coretemp: Package id 0");
    ASSERT_NE(temperature, nullptr);
    EXPECT_EQ(temperature->sensorKind, std::optional(Platform::SensorKind::Temperature));
    EXPECT_EQ(findRow(section, "coretemp: fan1")->sensorKind, std::optional(Platform::SensorKind::Fan));
    // Other sections' rows have no chart.
    for (const Row& item : SystemInfo::buildOsSection(windowsOs(), 0).rows)
    {
        EXPECT_FALSE(item.sensorKind.has_value()) << item.label;
    }
}

// #2340: two thermal zones of the same type are two rows with the same label, each showing its own range
// and keyed to its own history.
TEST(SystemInfoSectionsTest, SensorRowsSharingALabelShowTheirOwnRange)
{
    Platform::SensorsInfo sensors;
    sensors.available = true;
    sensors.listed = true;
    sensors.devices.push_back({.name = "Thermal zones", .id = "thermal", .readings = {}});
    sensors.devices[0].readings.push_back(
        {.kind = Platform::SensorKind::Temperature, .label = "acpitz", .id = "thermal_zone0", .value = 40.0, .high = {}, .critical = {}});
    sensors.devices[0].readings.push_back(
        {.kind = Platform::SensorKind::Temperature, .label = "acpitz", .id = "thermal_zone1", .value = 70.0, .high = {}, .critical = {}});
    const std::map<std::string, Domain::SensorRange> ranges{
        {Domain::sensorKey(sensors.devices[0], sensors.devices[0].readings[0]), {.min = 38.0, .max = 42.0, .samples = 3}},
        {Domain::sensorKey(sensors.devices[0], sensors.devices[0].readings[1]), {.min = 65.0, .max = 75.0, .samples = 3}},
    };
    const Section section = SystemInfo::buildSensorsSection(sensors, ranges);
    ASSERT_GE(section.rows.size(), 2U);
    const Row& first = section.rows[0];
    const Row& second = section.rows[1];
    EXPECT_EQ(first.label, "Thermal zones: acpitz");
    EXPECT_EQ(second.label, "Thermal zones: acpitz");
    EXPECT_NE(first.sensorKey, second.sensorKey);
    EXPECT_TRUE(ranges.contains(first.sensorKey));
    EXPECT_TRUE(ranges.contains(second.sensorKey));
    EXPECT_TRUE(first.value.contains(UI::Format::formatFixedLocalized(42.0, 1, " °C"))) << first.value;
    EXPECT_FALSE(first.value.contains(UI::Format::formatFixedLocalized(75.0, 1, " °C"))) << first.value;
    EXPECT_TRUE(second.value.contains(UI::Format::formatFixedLocalized(75.0, 1, " °C"))) << second.value;
    EXPECT_FALSE(second.value.contains(UI::Format::formatFixedLocalized(42.0, 1, " °C"))) << second.value;
}

TEST_F(SystemInfoViewRenderTest, SensorsSectionDrawsEachSensorsHistory)
{
    ImPlotContext* plot = ImPlot::CreateContext();
    const Platform::SystemInfoCapabilities supported{.hasOs = true, .unavailableReason = {}};
    const Domain::SystemInfoSnapshot page = snapshot();
    const Domain::SensorPublication withHistory = sensorsWithHistory();
    SystemInfoViewState state;
    static_cast<void>(runFrame([&] { return renderSystemInfoView(&page, supported, false, state, &withHistory); }));
    const int withCharts = ImGui::GetDrawData()->TotalVtxCount;

    // The same sensors with no history (a first read): no chart column, so less geometry.
    Domain::SensorPublication withoutHistory = withHistory;
    withoutHistory.version += 1;
    withoutHistory.histories.clear();
    static_cast<void>(runFrame([&] { return renderSystemInfoView(&page, supported, false, state, &withoutHistory); }));
    const int withoutCharts = ImGui::GetDrawData()->TotalVtxCount;
    EXPECT_GT(withCharts, withoutCharts);
    ImPlot::DestroyContext(plot);
}

// #2737: a new sensor read, at the Settings interval, rebuilds only the Sensors section: its row shows the
// new reading, and the rest of the page (built from the page read) is left as it was.
TEST_F(SystemInfoViewRenderTest, ANewSensorReadRebuildsOnlyTheSensorsSection)
{
    ImPlotContext* plot = ImPlot::CreateContext();
    const Platform::SystemInfoCapabilities supported{.hasOs = true, .unavailableReason = {}};
    const Domain::SystemInfoSnapshot page = snapshot();
    Domain::SensorPublication sensors = sensorsWithHistory();
    SystemInfoViewState state;
    static_cast<void>(runFrame([&] { return renderSystemInfoView(&page, supported, false, state, &sensors); }));
    const auto sensorsSection = [&state]
    {
        return std::ranges::find(state.sections, SystemInfo::SENSORS_SECTION_TITLE, &Section::title);
    };
    ASSERT_NE(sensorsSection(), state.sections.end());
    const std::size_t sectionCount = state.sections.size();
    state.readAtText = "untouched"; // set again only by a full rebuild for a new page read

    sensors.version += 1;
    sensors.sensors.devices[0].readings[0].value = 77.0;
    static_cast<void>(runFrame([&] { return renderSystemInfoView(&page, supported, false, state, &sensors); }));
    ASSERT_NE(sensorsSection(), state.sections.end());
    EXPECT_TRUE(findRow(*sensorsSection(), "coretemp: Package id 0")->value.contains(UI::Format::formatFixedLocalized(77.0, 1, " °C")));
    EXPECT_EQ(state.sections.size(), sectionCount);
    EXPECT_EQ(state.readAtText, "untouched") << "the page read wasn't rebuilt";
    EXPECT_EQ(state.sectionsSensorsVersion, sensors.version);
    ImPlot::DestroyContext(plot);
}

// #2120: the page draws a mount point that names the user redacted until identifiers are shown.
TEST_F(SystemInfoViewRenderTest, MountPointNamingTheUserDrawsRedactedUntilIdentifiersAreShown)
{
    const Platform::SystemInfoCapabilities supported{.hasOs = true, .unavailableReason = {}};
    Domain::SystemInfoSnapshot snap;
    snap.version = 1;
    snap.os.family = Platform::OsFamily::Linux;
    snap.os.userName = "alice";
    snap.storage.available = true;
    snap.storage.family = Platform::OsFamily::Linux;
    snap.storage.disksRead = true;
    snap.storage.volumesRead = true;
    snap.storage.volumes = {{.mountPoint = "/media/alice/USB",
                             .label = "USB",
                             .fileSystem = "vfat",
                             .device = "/dev/sdb1",
                             .network = false,
                             .sizeRead = false,
                             .sizeBytes = 0,
                             .freeBytes = 0}};
    SystemInfoViewState state;
    const auto drawnText = [&]
    {
        std::string text;
        static_cast<void>(runFrame(
            [&]
            {
                ImGui::LogToBuffer();
                const SystemInfoViewResult result = renderSystemInfoView(&snap, supported, false, state);
                text = GImGui->LogBuffer.c_str();
                ImGui::LogFinish();
                return result;
            }));
        return text;
    };
    const std::string hidden = drawnText();
    EXPECT_TRUE(hidden.contains("/media/<user>/USB")) << hidden;
    EXPECT_FALSE(hidden.contains("alice")) << hidden;
    state.showIdentifiers = true;
    const std::string shown = drawnText();
    EXPECT_TRUE(shown.contains("/media/alice/USB")) << shown;
}

} // namespace
} // namespace App
