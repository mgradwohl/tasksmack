/// @file test_SystemInfoView.cpp
/// @brief The System Information page (#1399): the Operating system section's rows (#1512), the
/// Firmware & board section's rows (#1513), the filter, identifier hiding, the Copy text and unavailable values; then the view headless:
/// the unsupported and loading states, sections drawn, the filter narrowing and the identifier toggle.

#include "App/Panels/SystemInfoSections.h"
#include "App/Panels/SystemInfoView.h"
#include "Domain/SystemInfoModel.h"
#include "Platform/ISystemInfoProbe.h"

#include <gtest/gtest.h>
#include <imgui.h>

#include <algorithm>
#include <cstddef>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>

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
    return {.version = 3, .readAtUnixSeconds = 1'700'000'000 + 93'784, .os = windowsOs(), .firmware = {}};
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

TEST(SystemInfoSectionsTest, NoSectionsBeforeTheFirstRead)
{
    EXPECT_TRUE(SystemInfo::buildSystemInfoSections(Domain::SystemInfoSnapshot{}).empty());
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

    static SystemInfoViewResult runFrame(const std::function<SystemInfoViewResult()>& body)
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
        ImGui::SetNextWindowSize(ImVec2(1000.0F, 700.0F));
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
    const auto unsupported = Platform::UnsupportedSystemInfoProbe{}.capabilities();
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

} // namespace
} // namespace App
