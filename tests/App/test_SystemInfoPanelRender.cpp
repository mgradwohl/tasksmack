/// @file test_SystemInfoPanelRender.cpp
/// @brief The System tab's panel (#1399), headless with a fake probe through its test seam (#1395): no
/// read until the tab first shows, then one on a worker thread, whose facts the page draws and whose
/// graphics facts are asked for once; showing the tab again doesn't read again, Refresh does; a probe
/// with nothing to read is never asked; detaching waits for a read still in flight; and the live
/// sensors (#1522) are sampled from attach and shown with their minimum and maximum.

#include "App/Panels/SystemInfoPanel.h"
#include "Core/ApplicationEvents.h"
#include "Core/GraphicsHostInfo.h"
#include "Domain/SamplingConfig.h"
#include "Mocks/MockProbes.h"
#include "Platform/ISystemInfoProbe.h"
#include "UI/IconsFontAwesome6.h"

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h> // FindWindowByName(), ImHashStr(), ActivateItemByID(), the log buffer
#include <implot.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <utility>

namespace App
{
namespace
{

/// How the fake's reads went, shared with the test (the reads run on the panel's worker thread).
struct ProbeLog
{
    std::atomic<int> reads{0};
    std::atomic<bool> hold{false}; ///< While set, a read waits: a read "in flight".
};

/// A probe with an OS to report and nothing else, counting its reads.
class FakeSystemInfoProbe final : public Platform::ISystemInfoProbe
{
  public:
    FakeSystemInfoProbe(ProbeLog& log, bool hasOs) : m_Log(&log), m_HasOs(hasOs)
    {}

    [[nodiscard]] Platform::SystemInfoCapabilities capabilities() const override
    {
        return m_HasOs ? Platform::SystemInfoCapabilities{.hasOs = true, .unavailableReason = {}} : m_Default.capabilities();
    }
    [[nodiscard]] Platform::OsInfo readOs() override
    {
        while (m_Log->hold.load())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const int read = ++m_Log->reads;
        Platform::OsInfo info;
        info.family = Platform::OsFamily::Linux;
        info.name = "Fixture Linux " + std::to_string(read);
        return info;
    }
    [[nodiscard]] Platform::FirmwareInfo readFirmware() override
    {
        return m_Default.readFirmware();
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
        return m_Default.readPlatformSecurity();
    }
    [[nodiscard]] Platform::SensorsInfo readSensors() override
    {
        // Listed but none found, so the page has a Sensors section for the live one to replace (#1522).
        return {.available = true, .family = Platform::OsFamily::Linux, .listed = true, .devices = {}};
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
    [[nodiscard]] Platform::NetworkAdaptersInfo readNetworkAdapters() override
    {
        return m_Default.readNetworkAdapters();
    }
    [[nodiscard]] Platform::BootPerformanceInfo readBootPerformance() override
    {
        return m_Default.readBootPerformance();
    }

  private:
    ProbeLog* m_Log; // the test's, which outlives the panel and its reads
    bool m_HasOs;
    Platform::UnsupportedSystemInfoProbe m_Default;
};

class SystemInfoPanelRenderTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Context = ImGui::CreateContext();
        m_PlotContext = ImPlot::CreateContext(); // the live Sensors section's sparklines (#1522)
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

    /// A panel with live sensors (#1522): one package temperature, found at 52 °C by a mock probe the
    /// test keeps a pointer to (the panel's sampler owns it).
    void makePanelWithSensors()
    {
        m_Panel = std::make_unique<SystemInfoPanel>(
            [this] { return std::make_unique<FakeSystemInfoProbe>(m_Log, true); },
            [] { return Core::GraphicsHostInfo{}; },
            [this]
            {
                Platform::SensorsInfo sensors;
                sensors.available = true;
                sensors.listed = true;
                sensors.devices.push_back({.name = "coretemp", .readings = {}});
                sensors.devices[0].readings.push_back(
                    {.kind = Platform::SensorKind::Temperature, .label = "Package id 0", .value = 52.0, .high = {}, .critical = {}});
                auto probe = std::make_unique<TestMocks::MockSensorProbe>(std::move(sensors));
                probe->setValues({60.0});
                m_SensorProbe = probe.get();
                return probe;
            });
        m_Panel->onAttach();
    }

    TestMocks::MockSensorProbe* m_SensorProbe = nullptr; // owned by the panel's sensor model

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

TEST_F(SystemInfoPanelRenderTest, SensorsAreSampledFromAttachAndShownLive)
{
    makePanelWithSensors();
    ASSERT_NE(m_SensorProbe, nullptr);
    // Sampled from onAttach(), before the tab ever shows: minimum and maximum are since start.
    EXPECT_TRUE(m_SensorProbe->discoveries.waitFor(1));
    EXPECT_EQ(m_Log.reads.load(), 0);

    // The faster interval once the tab shows, so the second sample (the probe's 60 °C) comes promptly.
    Core::RefreshRateChangedEvent refresh(Domain::Sampling::REFRESH_INTERVAL_MIN_MS);
    m_Panel->onEvent(refresh);
    Core::HistoryDurationChangedEvent history(60);
    m_Panel->onEvent(history);
    showTab();
    ASSERT_TRUE(frameUntil("Fixture Linux 1"));
    EXPECT_TRUE(m_SensorProbe->reads.waitFor(1));
    ASSERT_TRUE(frameUntil("max 60.0")) << frame();
    EXPECT_TRUE(frame().contains("min 52.0"));
    EXPECT_EQ(m_SensorProbe->discoveries.count(), 1); // found once; every later sample only reads
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

TEST_F(SystemInfoPanelRenderTest, NothingIsDrawnBeforeAttach)
{
    m_Panel = std::make_unique<SystemInfoPanel>([this] { return std::make_unique<FakeSystemInfoProbe>(m_Log, true); },
                                                [] { return Core::GraphicsHostInfo{}; });
    EXPECT_TRUE(frame().empty());
    m_Panel.reset();
}

} // namespace
} // namespace App
