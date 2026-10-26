/// @file test_SystemInfoPanelRender.cpp
/// @brief The System tab's panel (#1399), headless with a fake probe through its test seam (#1395): no
/// read until the tab first shows, then one on a worker thread, whose facts the page draws and whose
/// graphics facts are asked for once; showing the tab again doesn't read again, Refresh does; a probe
/// with nothing to read is never asked; and detaching waits for a read still in flight. "Show Wi-Fi
/// details" (#1782): only where the platform gates them, the first read leaves the Wi-Fi details out, the
/// click reads them, and every read after (Refresh) keeps reading them.

#include "App/Panels/SystemInfoPanel.h"
#include "App/Panels/SystemInfoSections.h"
#include "Core/Application.h"
#include "Core/ApplicationEvents.h"
#include "Core/GraphicsHostInfo.h"
#include "Core/SaveFileDialog.h"
#include "Domain/SamplingConfig.h"
#include "Domain/SystemInfoModel.h"
#include "Mocks/FakeClipboard.h"
#include "Mocks/UnsupportedSystemInfoProbe.h"
#include "Platform/ISystemInfoProbe.h"
#include "UI/IconsFontAwesome6.h"

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h> // FindWindowByName(), ImHashStr(), ActivateItemByID(), the log buffer
#include <implot.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace App
{
namespace
{

/// How the fake's reads went, shared with the test (the reads run on the panel's worker thread).
struct ProbeLog
{
    std::atomic<int> reads{0};
    std::atomic<bool> hold{false}; ///< While set, a read waits: a read "in flight".
    bool withSensors = false;      ///< The probe reports a temperature sensor.
    bool allSensorKinds = false;   ///< With withSensors: a second device with a fan, power, voltage and current (#1395).
    std::atomic<int> sensorReads{0};
    bool wifiGated = false;               ///< The platform gates the Wi-Fi details behind location access (#1782).
    std::atomic<int> adapterReadsWith{0}; ///< readNetworkAdapters() calls that asked for the Wi-Fi details
    std::atomic<int> adapterReadsWithout{0};
    bool publicIp = false;                     ///< The platform can look the public addresses up (#1518).
    std::atomic<int> publicIpReads{0};         ///< readPublicIp() calls
    std::atomic<bool> osThrows{false};         ///< readOs() throws, so the whole read fails (#1891)
    std::atomic<int> osAttempts{0};            ///< readOs() calls, the throwing ones included
    bool authorizable = false;                 ///< authorize() can ask for a password, and the firmware has root-only facts (#2381)
    std::atomic<bool> authorizeEntered{false}; ///< authorize() was called: the password prompt is "up"
    std::atomic<bool> authorizeSawStop{false}; ///< authorize() gave up because its stop token was stopped
    bool bitLocker = false;                    ///< Windows security with a fixed drive in every BitLocker state (#2584)
};

/// How long the fake's authorize() waits for its stop token before giving up by itself: a password
/// prompt nobody answers, far longer than detaching may take.
constexpr auto AUTHORIZE_BLOCK = std::chrono::seconds(10);

/// The Wi-Fi link row's value, which only a read with the Wi-Fi details has.
constexpr std::string_view WIFI_PHY = "802.11ax (Wi-Fi 6)";

/// A probe with an OS to report and nothing else, counting its reads.
class FakeSystemInfoProbe final : public Platform::ISystemInfoProbe
{
  public:
    FakeSystemInfoProbe(ProbeLog& log, bool hasOs) : m_Log(&log), m_HasOs(hasOs)
    {}

    [[nodiscard]] Platform::SystemInfoCapabilities capabilities() const override
    {
        return m_HasOs ? Platform::SystemInfoCapabilities{.hasOs = true,
                                                          .unavailableReason = {},
                                                          .wifiDetailsGated = m_Log->wifiGated,
                                                          .hasPublicIp = m_Log->publicIp,
                                                          .canReadAuthorized = m_Log->authorizable}
                       : m_Default.capabilities();
    }
    [[nodiscard]] bool canReadAuthorized() const override
    {
        return m_HasOs && m_Log->authorizable;
    }
    /// A password prompt nobody answers: waits until @p stop is stopped (or AUTHORIZE_BLOCK passes),
    /// then reports it wasn't authorized.
    bool authorize(const std::stop_token& stop) override
    {
        m_Log->authorizeEntered = true;
        const auto deadline = std::chrono::steady_clock::now() + AUTHORIZE_BLOCK;
        while (!stop.stop_requested() && std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        m_Log->authorizeSawStop = stop.stop_requested();
        return false;
    }
    [[nodiscard]] Platform::PublicIpInfo readPublicIp() override
    {
        ++m_Log->publicIpReads;
        return {.read = true, .v4 = "203.0.113.7", .v6 = "2001:db8::7", .v4Unavailable = {}, .v6Unavailable = {}, .source = "OpenDNS"};
    }
    [[nodiscard]] Platform::OsInfo readOs() override
    {
        while (m_Log->hold.load())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        ++m_Log->osAttempts;
        if (m_Log->osThrows.load())
        {
            throw std::runtime_error("fixture OS read failed");
        }
        const int read = ++m_Log->reads;
        Platform::OsInfo info;
        info.family = Platform::OsFamily::Linux;
        info.name = "Fixture Linux " + std::to_string(read);
        return info;
    }
    /// Where authorizable, firmware whose identifiers only root can read, so the section leads with
    /// "Authenticate to read" (#2310).
    [[nodiscard]] Platform::FirmwareInfo readFirmware() override
    {
        if (!m_Log->authorizable)
        {
            return m_Default.readFirmware();
        }
        Platform::FirmwareInfo info;
        info.available = true;
        info.systemManufacturer = "Fixture Systems";
        info.identifiersNeedAdmin = true;
        return info;
    }
    [[nodiscard]] Platform::MemoryModulesInfo readMemoryModules() override
    {
        return m_Default.readMemoryModules();
    }
    [[nodiscard]] Platform::CommitPagingInfo readCommitPaging() override
    {
        return m_Default.readCommitPaging();
    }
    [[nodiscard]] Platform::StorageInfo readStorage() override
    {
        return m_Default.readStorage();
    }
    [[nodiscard]] Platform::GraphicsInfo readGraphics() override
    {
        return m_Default.readGraphics();
    }
    [[nodiscard]] Platform::PlatformSecurityInfo readPlatformSecurity() override
    {
        if (!m_Log->bitLocker)
        {
            return m_Default.readPlatformSecurity();
        }
        using Platform::BitLockerState;
        Platform::PlatformSecurityInfo info;
        info.available = true;
        info.family = Platform::OsFamily::Windows;
        info.bitLockerRead = true;
        info.bitLocker = {
            {.volume = "C:", .state = BitLockerState::On},
            {.volume = "D:", .state = BitLockerState::Off},
            {.volume = "E:", .state = BitLockerState::Encrypting},
            {.volume = "F:", .state = BitLockerState::Decrypting},
            {.volume = "G:", .state = BitLockerState::Suspended},
            {.volume = "H:", .state = BitLockerState::Locked},
            {.volume = "I:", .state = BitLockerState::NotEncryptable},
            {.volume = "J:", .state = BitLockerState::WaitingForActivation},
            {.volume = "K:", .state = BitLockerState::Unknown},
        };
        return info;
    }
    [[nodiscard]] Platform::SensorsInfo readSensors() override
    {
        if (!m_Log->withSensors)
        {
            return m_Default.readSensors();
        }
        // One temperature, a degree warmer each read: a range to track (#1522).
        ++m_Log->sensorReads;
        Platform::SensorsInfo info;
        info.available = true;
        info.listed = true;
        info.devices.push_back({.name = "coretemp",
                                .id = "hwmon1",
                                .readings = {{.kind = Platform::SensorKind::Temperature,
                                              .label = "Package id 0",
                                              .id = "temp1",
                                              .value = 40.0 + static_cast<double>(m_Log->sensorReads.load()),
                                              .high = std::nullopt,
                                              .critical = std::nullopt}}});
        if (m_Log->allSensorKinds)
        {
            info.devices.push_back({.name = "nct6775",
                                    .id = "hwmon2",
                                    .readings = {{.kind = Platform::SensorKind::Fan,
                                                  .label = "fan1",
                                                  .id = {},
                                                  .value = 1200.0,
                                                  .high = std::nullopt,
                                                  .critical = std::nullopt},
                                                 {.kind = Platform::SensorKind::Power,
                                                  .label = "power1",
                                                  .id = {},
                                                  .value = 35.0,
                                                  .high = std::nullopt,
                                                  .critical = std::nullopt},
                                                 {.kind = Platform::SensorKind::Voltage,
                                                  .label = "in0",
                                                  .id = {},
                                                  .value = 1.2,
                                                  .high = std::nullopt,
                                                  .critical = std::nullopt},
                                                 {.kind = Platform::SensorKind::Current,
                                                  .label = "curr1",
                                                  .id = {},
                                                  .value = 3.5,
                                                  .high = std::nullopt,
                                                  .critical = std::nullopt}}});
        }
        return info;
    }
    [[nodiscard]] Platform::DevicesInfo readDevices() override
    {
        return m_Default.readDevices();
    }
    [[nodiscard]] Platform::DriversInfo readDrivers() override
    {
        return m_Default.readDrivers();
    }
    [[nodiscard]] Platform::CrashesInfo readCrashes() override
    {
        return m_Default.readCrashes();
    }
    /// One connected Wi-Fi adapter. Where the platform gates them, its details only when asked for, as
    /// Windows' reader does; elsewhere always, as Linux's does.
    [[nodiscard]] Platform::NetworkAdaptersInfo readNetworkAdapters(bool includeWifiDetails) override
    {
        ++(includeWifiDetails ? m_Log->adapterReadsWith : m_Log->adapterReadsWithout);
        Platform::NetworkAdaptersInfo info;
        info.available = true;
        info.listed = true;
        info.family = Platform::OsFamily::Windows;
        Platform::NetworkAdapter wifi;
        wifi.name = "Wi-Fi";
        wifi.up = true;
        wifi.wireless = true;
        wifi.wifiSignalDbm = -55;
        if (m_Log->wifiGated && !includeWifiDetails)
        {
            wifi.wifiDetailsWithheld = true;
        }
        else
        {
            wifi.wifiLink = Platform::WifiLink{
                .ssid = "HomeNet",
                .frequencyMhz = 5180,
                .bitrateKbps = 0,
                .signalQuality = 82,
                .phy = std::string(WIFI_PHY),
                .channel = 0,
                .rxBitrateKbps = 0,
            };
        }
        info.adapters.push_back(std::move(wifi));
        return info;
    }
    [[nodiscard]] Platform::BootPerformanceInfo readBootPerformance() override
    {
        return m_Default.readBootPerformance();
    }

  private:
    ProbeLog* m_Log; // the test's, which outlives the panel and its reads
    bool m_HasOs;
    Mocks::UnsupportedSystemInfoProbe m_Default;
};

class SystemInfoPanelRenderTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Context = ImGui::CreateContext();
        m_PlotContext = ImPlot::CreateContext(); // the Sensors rows' charts (#1522)
        m_Clipboard.install();                   // never the system clipboard (#1890)
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
        if (m_Panel)
        {
            m_Panel->onDetach();
            m_Panel.reset();
        }
        ImPlot::DestroyContext(m_PlotContext);
        ImGui::DestroyContext(m_Context);
    }

    void makePanel(bool hasOs)
    {
        m_Panel = std::make_unique<SystemInfoPanel>([this, hasOs] { return std::make_unique<FakeSystemInfoProbe>(m_Log, hasOs); },
                                                    [this]
                                                    {
                                                        ++m_HostQueries;
                                                        return Core::GraphicsHostInfo{};
                                                    });
        m_Panel->onAttach();
    }

    void showTab(const std::string& name = "SystemInfo")
    {
        Core::ActiveTabChangedEvent event(name);
        m_Panel->onEvent(event);
    }

    /// One frame of the panel in a window standing in for the shell's content area; the text it drew.
    std::string frame()
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        ImGui::Begin("Panel", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
        ImGui::LogToBuffer();
        m_Panel->renderContent();
        std::string text = GImGui->LogBuffer.c_str();
        ImGui::LogFinish();
        ImGui::End();
        ImGui::Render();
        return text;
    }

    /// The ID of the button @p label on the row with @p action in the panel's page: built as SystemInfoView
    /// draws it, in the sections child window, under the section's index, its rows table and the row's
    /// index (#1782, #1518). The indices come from the same fake's sections, configured as m_Log is.
    [[nodiscard]] ImGuiID rowButtonId(SystemInfo::RowAction action, const char* label) const
    {
        ProbeLog log;
        log.wifiGated = m_Log.wifiGated;
        log.publicIp = m_Log.publicIp;
        log.authorizable = m_Log.authorizable;
        Domain::SystemInfoModel model(std::make_unique<FakeSystemInfoProbe>(log, true));
        model.read();
        const std::vector<SystemInfo::Section> sections = SystemInfo::buildSystemInfoSections(*model.snapshot(), {}, model.capabilities());
        const ImGuiWindow* child = nullptr;
        for (const ImGuiWindow* window : GImGui->Windows)
        {
            if (std::string_view(window->Name).starts_with("Panel/##SystemInfoSections"))
            {
                child = window;
            }
        }
        std::size_t sectionIndex = 0;
        while (child != nullptr && sectionIndex < sections.size())
        {
            const std::vector<SystemInfo::Row>& rows = sections[sectionIndex].rows;
            std::size_t rowIndex = 0;
            while (rowIndex < rows.size())
            {
                if (rows[rowIndex].action == action)
                {
                    const int section = static_cast<int>(sectionIndex);
                    const int row = static_cast<int>(rowIndex);
                    const ImGuiID table = ImHashStr("##Rows", 0, ImHashData(&section, sizeof(section), child->ID));
                    return ImHashStr(label, 0, ImHashData(&row, sizeof(row), table));
                }
                ++rowIndex;
            }
            ++sectionIndex;
        }
        return 0;
    }

    /// Frames until the page draws @p text, for up to five seconds (the read runs on a worker thread).
    bool frameUntil(const std::string& text)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (frame().contains(text))
            {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return false;
    }

    ProbeLog m_Log;
    int m_HostQueries = 0;
    std::unique_ptr<SystemInfoPanel> m_Panel;

  private:
    TestMocks::FakeClipboard m_Clipboard;
    ImGuiContext* m_Context = nullptr;
    ImPlotContext* m_PlotContext = nullptr;
};

TEST_F(SystemInfoPanelRenderTest, ReadsOnlyOnceTheTabShowsAndDrawsTheFacts)
{
    makePanel(true);
    static_cast<void>(frame());
    showTab("Processes"); // another tab: still nothing read
    static_cast<void>(frame());
    EXPECT_EQ(m_Log.reads.load(), 0);

    showTab();
    ASSERT_TRUE(frameUntil("Fixture Linux 1"));
    EXPECT_EQ(m_Log.reads.load(), 1);
    EXPECT_EQ(m_HostQueries, 1); // Core's graphics facts, once per read, on the UI thread

    // Static facts: showing the tab again doesn't read them again, and no more host queries.
    showTab();
    static_cast<void>(frame());
    static_cast<void>(frame());
    EXPECT_EQ(m_Log.reads.load(), 1);
    EXPECT_EQ(m_HostQueries, 1);
}

// #1891: a row with no value shows a muted dash, and hovering it says why. Found by sweeping down the
// Value column of each section's table, from the tables ImGui laid out (#2061's approach).
TEST_F(SystemInfoPanelRenderTest, AnUnavailableValueSaysWhyOnHover)
{
    makePanel(true);
    showTab();
    ASSERT_TRUE(frameUntil("Fixture Linux 1")); // the fixture's OS has a name and nothing else
    ImGuiStyle& style = ImGui::GetStyle();
    style.HoverStationaryDelay = 0.0F;
    style.HoverDelayShort = 0.0F;
    style.HoverDelayNormal = 0.0F;

    std::vector<std::pair<float, ImRect>> valueColumns; // the column's middle, the table's rows
    for (int t = 0; t < GImGui->Tables.GetMapSize(); ++t)
    {
        const ImGuiTable* table = GImGui->Tables.TryGetMapData(t);
        for (int c = 0; table != nullptr && c < table->ColumnsCount; ++c)
        {
            if (std::string_view(ImGui::TableGetColumnName(table, c)) == "Value")
            {
                // The dash sits at the column's left edge: aim just inside it.
                valueColumns.emplace_back(table->Columns[c].MinX + 6.0F, table->InnerRect);
            }
        }
    }
    ASSERT_FALSE(valueColumns.empty()) << "no section table drawn";
    bool shown = false;
    ImGuiIO& io = ImGui::GetIO();
    for (const auto& [x, rows] : valueColumns)
    {
        const int steps = static_cast<int>(rows.GetHeight() / 4.0F);
        for (int step = 0; step < steps && !shown; ++step)
        {
            io.AddMousePosEvent(x, rows.Min.y + 2.0F + (static_cast<float>(step) * 4.0F));
            for (int settle = 0; settle < 2 && !shown; ++settle)
            {
                const std::string text = frame();
                const ImGuiWindow* tooltip = ImGui::FindWindowByName("##Tooltip_00");
                shown = tooltip != nullptr && tooltip->Active && text.contains("Not reported by this system");
            }
        }
    }
    EXPECT_TRUE(shown) << "an unavailable row's reason on hover";
}

TEST_F(SystemInfoPanelRenderTest, RefreshReadsAgain)
{
    makePanel(true);
    showTab();
    ASSERT_TRUE(frameUntil("Fixture Linux 1"));

    const ImGuiWindow* window = ImGui::FindWindowByName("Panel");
    ASSERT_NE(window, nullptr);
    ImGui::ActivateItemByID(ImHashStr(ICON_FA_ARROWS_ROTATE "  Refresh", 0, window->ID));
    ASSERT_TRUE(frameUntil("Fixture Linux 2"));
    EXPECT_EQ(m_Log.reads.load(), 2);
    EXPECT_EQ(m_HostQueries, 2);
}

TEST_F(SystemInfoPanelRenderTest, AProbeWithNothingToReadIsNeverAsked)
{
    makePanel(false);
    showTab();
    for (int i = 0; i < 5; ++i)
    {
        static_cast<void>(frame());
    }
    EXPECT_EQ(m_Log.reads.load(), 0);
    EXPECT_EQ(m_HostQueries, 0);
}

TEST_F(SystemInfoPanelRenderTest, DetachingWaitsForAReadInFlight)
{
    makePanel(true);
    m_Log.hold = true;
    showTab();
    static_cast<void>(frame());
    EXPECT_EQ(m_Log.reads.load(), 0); // held on the worker

    // Released from another thread after a moment; onDetach() must not return before the read ends.
    std::thread release(
        [this]
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            m_Log.hold = false;
        });
    m_Panel->onDetach();
    EXPECT_EQ(m_Log.reads.load(), 1);
    release.join();
    m_Panel.reset();
}

// #2381 (#2479): closing TaskSmack while "Authenticate to read"'s password prompt is still up gives the
// prompt up rather than waiting for it to be answered: onDetach() stops the read's stop token, which
// reaches authorize(), before waiting for the read. Without that stop, detaching waits out the fake's
// unanswered prompt (AUTHORIZE_BLOCK).
TEST_F(SystemInfoPanelRenderTest, DetachingGivesUpOnAPasswordPromptStillUp)
{
    m_Log.authorizable = true;
    makePanel(true);
    showTab();
    ASSERT_TRUE(frameUntil("Fixture Linux 1"));
    ASSERT_TRUE(frame().contains("Authenticate to read"));

    const ImGuiID button = rowButtonId(SystemInfo::RowAction::AuthenticateToRead, ICON_FA_LOCK "  Authenticate to read");
    ASSERT_NE(button, 0U);
    // Pressed until the next read is in the prompt, as "Show Wi-Fi details" is (#1845).
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!m_Log.authorizeEntered.load() && std::chrono::steady_clock::now() < deadline)
    {
        ImGui::ActivateItemByID(button);
        static_cast<void>(frame());
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_TRUE(m_Log.authorizeEntered.load()) << "the click never reached authorize()";

    const auto started = std::chrono::steady_clock::now();
    m_Panel->onDetach();
    const auto elapsed = std::chrono::steady_clock::now() - started;
    EXPECT_LT(elapsed, std::chrono::seconds(2)) << "detaching waited for the password prompt to be answered";
    EXPECT_TRUE(m_Log.authorizeSawStop.load());
    m_Panel.reset();
}

TEST_F(SystemInfoPanelRenderTest, NothingIsDrawnBeforeAttach)
{
    m_Panel = std::make_unique<SystemInfoPanel>([this] { return std::make_unique<FakeSystemInfoProbe>(m_Log, true); },
                                                [] { return Core::GraphicsHostInfo{}; });
    EXPECT_TRUE(frame().empty());
    m_Panel.reset();
}

// #1782: where the platform gates them, opening the tab doesn't ask for the Wi-Fi details: a wireless
// adapter shows "Show Wi-Fi details" and its location note instead. The click reads them on the worker,
// and the choice holds for the session: Refresh reads them too.
TEST_F(SystemInfoPanelRenderTest, ShowWifiDetailsReadsThemAndRefreshKeepsReadingThem)
{
    m_Log.wifiGated = true;
    makePanel(true);
    showTab();
    ASSERT_TRUE(frameUntil("Fixture Linux 1"));
    std::string text = frame();
    EXPECT_TRUE(text.contains("Show Wi-Fi details")) << text;
    EXPECT_TRUE(text.contains("Windows may ask for location access")) << text;
    EXPECT_FALSE(text.contains(WIFI_PHY));
    EXPECT_EQ(m_Log.adapterReadsWith.load(), 0);
    EXPECT_EQ(m_Log.adapterReadsWithout.load(), 1);

    const ImGuiID button = rowButtonId(SystemInfo::RowAction::ShowWifiDetails, ICON_FA_WIFI "  Show Wi-Fi details");
    ASSERT_NE(button, 0U);
    // Pressed again each frame until the second read shows: ActivateItemByID() is a one-shot request,
    // dropped on a frame where the button is disabled (a read in flight), which left the test waiting
    // for a read nothing started (#1845). Once pressed the button stays disabled, so pressing it again
    // does nothing.
    bool secondRead = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!secondRead && std::chrono::steady_clock::now() < deadline)
    {
        ImGui::ActivateItemByID(button);
        secondRead = frame().contains("Fixture Linux 2");
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_TRUE(secondRead);
    text = frame();
    EXPECT_TRUE(text.contains(WIFI_PHY)) << text;
    EXPECT_FALSE(text.contains("Show Wi-Fi details")) << text;
    EXPECT_EQ(m_Log.adapterReadsWith.load(), 1);
    EXPECT_EQ(m_Log.adapterReadsWithout.load(), 1);

    const ImGuiWindow* window = ImGui::FindWindowByName("Panel");
    ASSERT_NE(window, nullptr);
    ImGui::ActivateItemByID(ImHashStr(ICON_FA_ARROWS_ROTATE "  Refresh", 0, window->ID));
    ASSERT_TRUE(frameUntil("Fixture Linux 3"));
    EXPECT_EQ(m_Log.adapterReadsWith.load(), 2) << "the choice holds for the session";
    EXPECT_EQ(m_Log.adapterReadsWithout.load(), 1);
    EXPECT_TRUE(frame().contains(WIFI_PHY));
}

// #1518: "Show public IP" looks the addresses up and every Refresh after looks them up again; until it's
// clicked nothing is asked.
TEST_F(SystemInfoPanelRenderTest, ShowPublicIpLooksItUpAndRefreshKeepsLookingItUp)
{
    m_Log.publicIp = true;
    makePanel(true);
    showTab();
    ASSERT_TRUE(frameUntil("Fixture Linux 1"));
    std::string text = frame();
    EXPECT_TRUE(text.contains("Show public IP")) << text;
    EXPECT_TRUE(text.contains("Sends a DNS query to OpenDNS")) << text;
    EXPECT_EQ(m_Log.publicIpReads.load(), 0);

    const ImGuiID button = rowButtonId(SystemInfo::RowAction::ShowPublicIp, ICON_FA_NETWORK_WIRED "  Show public IP");
    ASSERT_NE(button, 0U);
    // Pressed until the second read shows, as "Show Wi-Fi details" is (#1845).
    bool secondRead = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!secondRead && std::chrono::steady_clock::now() < deadline)
    {
        ImGui::ActivateItemByID(button);
        secondRead = frame().contains("Fixture Linux 2");
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_TRUE(secondRead);
    text = frame();
    EXPECT_FALSE(text.contains("Show public IP")) << text;
    EXPECT_EQ(m_Log.publicIpReads.load(), 1);

    const ImGuiWindow* window = ImGui::FindWindowByName("Panel");
    ASSERT_NE(window, nullptr);
    ImGui::ActivateItemByID(ImHashStr(ICON_FA_ARROWS_ROTATE "  Refresh", 0, window->ID));
    ASSERT_TRUE(frameUntil("Fixture Linux 3"));
    EXPECT_EQ(m_Log.publicIpReads.load(), 2) << "the choice holds for the session";
}

// #1866: Save report... asks where to save (Text or JSON, a default name without identifiers), then
// writes the page in the chosen format and says where; a cancel says nothing; a failed write says why.
TEST_F(SystemInfoPanelRenderTest, SaveReportWritesTheChosenFormat)
{
    std::filesystem::path dir =
        std::filesystem::temp_directory_path() / ("tasksmack_save_report_" + std::to_string(std::random_device{}()));
    ASSERT_TRUE(std::filesystem::create_directory(dir));

    struct Ask
    {
        std::vector<Core::FileFilter> filters;
        std::string defaultLocation;
        Core::SaveFileCallback answer;
    };
    std::vector<Ask> asks;
    makePanel(true);
    m_Panel->setSaveFileDialog(
        [&asks](std::vector<Core::FileFilter> filters, std::string defaultLocation, Core::SaveFileCallback answer)
        { asks.push_back({.filters = std::move(filters), .defaultLocation = std::move(defaultLocation), .answer = std::move(answer)}); });
    showTab();
    ASSERT_TRUE(frameUntil("Fixture Linux 1"));
    const ImGuiWindow* window = ImGui::FindWindowByName("Panel");
    ASSERT_NE(window, nullptr);
    const ImGuiID save = ImHashStr(ICON_FA_FLOPPY_DISK "  Save report...", 0, window->ID);
    const auto click = [&]
    {
        ImGui::ActivateItemByID(save);
        static_cast<void>(frame());
        static_cast<void>(frame());
    };
    const auto slurp = [](const std::filesystem::path& path)
    {
        std::ifstream in(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), {});
    };

    click();
    ASSERT_EQ(asks.size(), 1U);
    ASSERT_EQ(asks[0].filters.size(), 2U);
    EXPECT_EQ(asks[0].filters[0].pattern, "txt");
    EXPECT_EQ(asks[0].filters[1].pattern, "json");
    EXPECT_TRUE(asks[0].defaultLocation.starts_with("tasksmack-system-info-machine-")) << asks[0].defaultLocation;
    click();
    EXPECT_EQ(asks.size(), 1U) << "disabled while the dialog is open";

    // JSON through the filter, the name typed without an extension. The answer comes from another thread.
    std::thread([&] { asks[0].answer({.path = (dir / "report").string(), .filter = 1, .error = {}}); }).join();
    const std::filesystem::path json = dir / "report.json";
    ASSERT_TRUE(frameUntil("Saved to " + json.string())) << "written on the report-save worker (#2986)";
    ASSERT_TRUE(std::filesystem::exists(json));
    EXPECT_TRUE(slurp(json).contains("\"title\": \"Operating system\"")) << slurp(json);
    EXPECT_TRUE(slurp(json).contains("Fixture Linux 1"));

    // Cancelled: nothing written, nothing said.
    click();
    ASSERT_EQ(asks.size(), 2U);
    asks[1].answer({});
    std::string text = frame();
    EXPECT_FALSE(text.contains("Saved to")) << text;
    EXPECT_FALSE(text.contains("Couldn't")) << text;

    // Text by its extension; then a folder that doesn't exist.
    click();
    ASSERT_EQ(asks.size(), 3U);
    asks[2].answer({.path = (dir / "report.txt").string(), .filter = -1, .error = {}});
    ASSERT_TRUE(frameUntil("Saved to " + (dir / "report.txt").string()));
    EXPECT_TRUE(slurp(dir / "report.txt").starts_with("Operating system\n")) << slurp(dir / "report.txt");
    click();
    ASSERT_EQ(asks.size(), 4U);
    asks[3].answer({.path = (dir / "missing" / "report.txt").string(), .filter = 0, .error = {}});
    EXPECT_TRUE(frameUntil("Couldn't save " + (dir / "missing" / "report.txt").string()));

    // The dialog itself failed.
    click();
    ASSERT_EQ(asks.size(), 5U);
    asks[4].answer({.path = {}, .filter = -1, .error = "no portal"});
    text = frame();
    EXPECT_TRUE(text.contains("Couldn't show the save dialog: no portal")) << text;

    std::error_code ignored;
    std::filesystem::remove_all(dir, ignored);
}

// #2144: a name typed without an extension gets one after the dialog has closed, so the dialog never
// asked about replacing that file: an existing one is left alone and a numbered name is used instead.
// A name the dialog answered with its extension already on is the user's confirmed choice.
TEST_F(SystemInfoPanelRenderTest, SaveReportNeverReplacesAFileTheDialogDidNotAskAbout)
{
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / ("tasksmack_save_report_" + std::to_string(std::random_device{}()));
    ASSERT_TRUE(std::filesystem::create_directory(dir));
    const auto slurp = [](const std::filesystem::path& path)
    {
        std::ifstream in(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), {});
    };
    const auto write = [](const std::filesystem::path& path, std::string_view text)
    {
        std::ofstream out(path, std::ios::binary);
        out << text;
    };
    write(dir / "report.txt", "yesterday's report");
    write(dir / "report (2).txt", "an older one");

    std::vector<Core::SaveFileCallback> answers;
    makePanel(true);
    m_Panel->setSaveFileDialog(
        [&answers](const std::vector<Core::FileFilter>& /*filters*/, const std::string& /*defaultLocation*/, Core::SaveFileCallback answer)
        { answers.push_back(std::move(answer)); });
    showTab();
    ASSERT_TRUE(frameUntil("Fixture Linux 1"));
    const ImGuiWindow* window = ImGui::FindWindowByName("Panel");
    ASSERT_NE(window, nullptr);
    const ImGuiID save = ImHashStr(ICON_FA_FLOPPY_DISK "  Save report...", 0, window->ID);
    const auto click = [&]
    {
        ImGui::ActivateItemByID(save);
        static_cast<void>(frame());
        static_cast<void>(frame());
    };

    // "report", the Text filter: report.txt exists, so the report goes to the first free numbered name.
    click();
    ASSERT_EQ(answers.size(), 1U);
    answers[0]({.path = (dir / "report").string(), .filter = 0, .error = {}});
    const std::filesystem::path numbered = dir / "report (3).txt";
    ASSERT_TRUE(frameUntil("Saved to " + numbered.string())) << "the free name is found on the report-save worker (#2986)";
    std::string text = frame();
    EXPECT_EQ(slurp(dir / "report.txt"), "yesterday's report") << "not replaced without being asked";
    EXPECT_EQ(slurp(dir / "report (2).txt"), "an older one");
    ASSERT_TRUE(std::filesystem::exists(numbered));
    EXPECT_TRUE(slurp(numbered).starts_with("Operating system\n")) << slurp(numbered);
    EXPECT_TRUE(text.contains("Saved to " + numbered.string())) << text;
    EXPECT_TRUE(text.contains("already exists")) << text;

    // "report.txt" typed in full: the dialog asked, so it is replaced; the typed extension decides the
    // format over the JSON filter.
    click();
    ASSERT_EQ(answers.size(), 2U);
    answers[1]({.path = (dir / "report.txt").string(), .filter = 1, .error = {}});
    ASSERT_TRUE(frameUntil("Saved to " + (dir / "report.txt").string()));
    text = frame();
    EXPECT_TRUE(slurp(dir / "report.txt").starts_with("Operating system\n")) << slurp(dir / "report.txt");
    EXPECT_TRUE(text.contains("Saved to " + (dir / "report.txt").string())) << text;
    EXPECT_FALSE(text.contains("already exists")) << text;

    std::error_code ignored;
    std::filesystem::remove_all(dir, ignored);
}

// #2986: the report is written on a worker, never in renderContent(): while a write blocks, every
// frame returns at once with no note and Save report... refuses a second save; the note shows once the
// write finishes; and a write that fails still says why.
TEST_F(SystemInfoPanelRenderTest, SaveReportWritesOffTheUiThread)
{
    struct Writes
    {
        std::atomic<bool> release{false}; ///< While clear, a write waits: a slow destination.
        std::atomic<int> started{0};
        std::mutex mutex;
        std::thread::id thread;             // guarded by mutex
        std::string contents;               // guarded by mutex
        std::optional<std::string> failure; // guarded by mutex; what the next write reports
    };
    Writes writes;
    std::vector<Core::SaveFileCallback> answers;
    makePanel(true);
    m_Panel->setSaveFileDialog(
        [&answers](const std::vector<Core::FileFilter>& /*filters*/, const std::string& /*defaultLocation*/, Core::SaveFileCallback answer)
        { answers.push_back(std::move(answer)); });
    m_Panel->setReportWriter(
        [&writes](const std::string& /*path*/, const std::string& contents) -> std::optional<std::string>
        {
            std::optional<std::string> failure;
            {
                const std::scoped_lock lock(writes.mutex);
                writes.thread = std::this_thread::get_id();
                writes.contents = contents;
                failure = writes.failure;
            }
            ++writes.started;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (!writes.release.load() && std::chrono::steady_clock::now() < deadline)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            return failure;
        });
    showTab();
    ASSERT_TRUE(frameUntil("Fixture Linux 1"));
    const ImGuiWindow* window = ImGui::FindWindowByName("Panel");
    ASSERT_NE(window, nullptr);
    const ImGuiID save = ImHashStr(ICON_FA_FLOPPY_DISK "  Save report...", 0, window->ID);
    const auto click = [&]
    {
        ImGui::ActivateItemByID(save);
        static_cast<void>(frame());
        static_cast<void>(frame());
    };
    // Typed with its extension, so no free-name check is needed either; the fake writes nothing.
    const std::string target = (std::filesystem::temp_directory_path() / "tasksmack_unwritten_report.txt").string();

    click();
    ASSERT_EQ(answers.size(), 1U);
    answers[0]({.path = target, .filter = -1, .error = {}});
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (writes.started.load() == 0 && std::chrono::steady_clock::now() < deadline)
    {
        static_cast<void>(frame());
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_EQ(writes.started.load(), 1);
    {
        const std::scoped_lock lock(writes.mutex);
        EXPECT_NE(writes.thread, std::this_thread::get_id()) << "written on the report-save worker";
        EXPECT_TRUE(writes.contents.starts_with("Operating system\n")) << writes.contents;
    }
    const auto blockedFrom = std::chrono::steady_clock::now();
    for (int blocked = 0; blocked < 5; ++blocked)
    {
        const std::string text = frame();
        EXPECT_FALSE(text.contains("Saved to")) << text;
        EXPECT_FALSE(text.contains("Couldn't")) << text;
    }
    EXPECT_LT(std::chrono::steady_clock::now() - blockedFrom, std::chrono::seconds(2)) << "the frames never waited for the write";
    click();
    EXPECT_EQ(answers.size(), 1U) << "a second save is refused while the first is written";

    writes.release = true;
    EXPECT_TRUE(frameUntil("Saved to " + target));

    // A write that fails says so, with the writer's reason.
    {
        const std::scoped_lock lock(writes.mutex);
        writes.failure = "disk full";
    }
    click();
    ASSERT_EQ(answers.size(), 2U);
    answers[1]({.path = target, .filter = -1, .error = {}});
    EXPECT_TRUE(frameUntil("Couldn't save " + target + ": disk full"));
    EXPECT_EQ(writes.started.load(), 2);
}

// #1891: a read that throws is reported as failed on the UI thread; no facts are drawn from it.
TEST_F(SystemInfoPanelRenderTest, AFailedReadDrawsNoFacts)
{
    m_Log.osThrows = true;
    makePanel(true);
    showTab();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (m_Log.osAttempts.load() == 0 && std::chrono::steady_clock::now() < deadline)
    {
        static_cast<void>(frame());
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_GE(m_Log.osAttempts.load(), 1);
    std::string text;
    for (int settle = 0; settle < 20; ++settle) // let the worker finish and the panel collect its result
    {
        text = frame();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    EXPECT_FALSE(text.contains("Fixture Linux")) << text;
    EXPECT_EQ(m_Log.reads.load(), 0);
}

// #1891: with no save-dialog override and no window to own one, Save report says why instead.
TEST_F(SystemInfoPanelRenderTest, SaveReportWithoutAWindowSaysWhy)
{
    Core::ApplicationSpecification spec;
    spec.Name = "SaveReportHeadlessTest";
    spec.Headless = true;
    const Core::Application app(spec);
    ASSERT_FALSE(app.hasWindow());
    makePanel(true);
    showTab();
    ASSERT_TRUE(frameUntil("Fixture Linux 1"));
    const ImGuiWindow* window = ImGui::FindWindowByName("Panel");
    ASSERT_NE(window, nullptr);
    ImGui::ActivateItemByID(ImHashStr(ICON_FA_FLOPPY_DISK "  Save report...", 0, window->ID));
    static_cast<void>(frame());
    EXPECT_TRUE(frameUntil("Couldn't save: there is no window to show a save dialog"));
}

// #2135: with no host-query override (the default, as the PGO training shell builds it) in a headless
// Application, a finished read takes no graphics facts rather than throwing from getWindow().
TEST_F(SystemInfoPanelRenderTest, AReadInAHeadlessApplicationTakesNoGraphicsFacts)
{
    Core::ApplicationSpecification spec;
    spec.Name = "SystemInfoHeadlessTest";
    spec.Headless = true;
    const Core::Application app(spec);
    ASSERT_FALSE(app.hasWindow());
    m_Panel = std::make_unique<SystemInfoPanel>([this] { return std::make_unique<FakeSystemInfoProbe>(m_Log, true); }, nullptr);
    m_Panel->onAttach();
    showTab();
    bool drawn = false;
    EXPECT_NO_THROW(drawn = frameUntil("Fixture Linux 1"));
    EXPECT_TRUE(drawn);
}

// #2584: the Security section draws each fixed drive's BitLocker state, read without administrator, so
// nothing on it asks for administrator.
TEST_F(SystemInfoPanelRenderTest, SecurityDrawsEveryBitLockerState)
{
    m_Log.bitLocker = true;
    makePanel(true);
    showTab();
    ASSERT_TRUE(frameUntil("BitLocker (K:)"));
    const std::string text = frame();
    for (const char* drawn : {"BitLocker (C:)",
                              "BitLocker (D:)",
                              "Encrypting",
                              "Decrypting",
                              "Suspended",
                              "Locked",
                              "Not encryptable",
                              "Waiting for activation",
                              "Unknown"})
    {
        EXPECT_TRUE(text.contains(drawn)) << drawn << " in:" << '\n' << text;
    }
    EXPECT_FALSE(text.contains("Requires administrator")) << text;
}

// #1395: Copy all puts every section's text on the clipboard.
TEST_F(SystemInfoPanelRenderTest, CopyAllPutsThePageOnTheClipboard)
{
    makePanel(true);
    showTab();
    ASSERT_TRUE(frameUntil("Fixture Linux 1"));
    ImGui::SetClipboardText("");
    const ImGuiWindow* window = ImGui::FindWindowByName("Panel");
    ASSERT_NE(window, nullptr);
    ImGui::ActivateItemByID(ImHashStr(ICON_FA_COPY "  Copy all", 0, window->ID));
    static_cast<void>(frame());
    static_cast<void>(frame());
    const char* clipboard = ImGui::GetClipboardText();
    ASSERT_NE(clipboard, nullptr);
    EXPECT_TRUE(std::string_view(clipboard).contains("Fixture Linux 1")) << clipboard;
}

// #1395: a sensor of every kind -- fan, power, voltage, current beside the temperature -- is listed.
TEST_F(SystemInfoPanelRenderTest, EverySensorKindIsListed)
{
    m_Log.withSensors = true;
    m_Log.allSensorKinds = true;
    makePanel(true);
    showTab();
    ASSERT_TRUE(frameUntil("nct6775"));
    const std::string text = frame();
    for (const char* label : {"fan1", "power1", "in0", "curr1", "Package id 0"})
    {
        EXPECT_TRUE(text.contains(label)) << label << " in:\n" << text;
    }
}

// #1782: where the platform doesn't gate them (Linux), the Wi-Fi rows are read with the page, no button.
TEST_F(SystemInfoPanelRenderTest, NoWifiDetailsButtonWhereThePlatformDoesntGateThem)
{
    makePanel(true);
    showTab();
    ASSERT_TRUE(frameUntil("Fixture Linux 1"));
    const std::string text = frame();
    EXPECT_TRUE(text.contains(WIFI_PHY)) << text;
    EXPECT_FALSE(text.contains("Show Wi-Fi details")) << text;
    EXPECT_FALSE(text.contains("location access")) << text;
}

// #2737: the sensors are a metric, sampled by their own worker at the Settings interval from attach, with the
// tab never shown and nothing drawn; the page's full read waits for the tab. Once it shows, the Sensors rows
// show each sensor's range over the samples taken before.
TEST_F(SystemInfoPanelRenderTest, TheSensorsAreSampledAtTheSettingsIntervalWithoutTheTab)
{
    m_Log.withSensors = true;
    makePanel(true);
    Core::RefreshRateChangedEvent fast(Domain::Sampling::REFRESH_INTERVAL_MIN_MS);
    m_Panel->onEvent(fast);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline && m_Log.sensorReads.load() < 3)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    ASSERT_GE(m_Log.sensorReads.load(), 3) << "no frame drawn, no tab shown";
    EXPECT_EQ(m_Log.reads.load(), 0) << "the page's own read waits for the tab";

    showTab();
    ASSERT_TRUE(frameUntil("Fixture Linux 1"));
    EXPECT_TRUE(frameUntil("min ")) << "the range over the samples taken while the tab was hidden";
    EXPECT_EQ(m_Log.reads.load(), 1);
}

} // namespace
} // namespace App
