/// @file test_ServicesStartupPanelsRender.cpp
/// @brief The Services and Startup panels as a whole (#1721), run headless with fake probes and actions
/// through their onAttach() seam: the tab gate starts the sampler on first show and refreshes it on a
/// re-show, an unsupported probe never starts one, the loading, table, empty and failed-read states,
/// the default sorts (#1653), a row selected and acted on reaching the fake actions (never a real
/// service or startup entry), a finished service action re-reading the configuration, and onDetach()
/// cancelling a running service action (#1591, #1705) or waiting for a startup write.
///
/// Deterministic: the fakes signal each call through a condition variable (TestMocks::CallSignal), and
/// a frame loop only yields while the sampler thread commits what the fake already returned. No sleeps.

#include "App/Panel.h"
#include "App/Panels/ServicesPanel.h"
#include "App/Panels/StartupPanel.h"
#include "Core/ApplicationEvents.h"
#include "Mocks/MockProbes.h"
#include "Platform/IServiceActions.h"
#include "Platform/IServiceProbe.h"
#include "Platform/IStartupActions.h"
#include "Platform/IStartupProbe.h"
#include "UI/IconsFontAwesome6.h"

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h> // GImGui->LogBuffer, TableFindByID(), ImHashStr(), ActivateItemByID()

#include <cfloat>
#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace App
{
namespace
{

using Platform::ServiceState;
using TestMocks::MOCK_WAIT_LIMIT;
using TestMocks::MockServiceActions;
using TestMocks::MockServiceProbe;
using TestMocks::MockStartupActions;
using TestMocks::MockStartupProbe;

constexpr float DISPLAY_WIDTH = 1400.0F;
constexpr float DISPLAY_HEIGHT = 900.0F;

constexpr Platform::ServiceActionCapabilities SERVICE_ACTIONS{
    .canStart = true,
    .canStop = true,
    .canRestart = true,
    .canSetStartType = true,
    .elevated = true,
};
constexpr Platform::StartupActionCapabilities STARTUP_ACTIONS{.canSetEnabled = true, .elevated = false};

[[nodiscard]] Platform::ServiceCapabilities enumerableServices()
{
    Platform::ServiceCapabilities capabilities;
    capabilities.canEnumerate = true;
    capabilities.hasDisplayName = true;
    capabilities.hasStartType = true;
    return capabilities;
}

[[nodiscard]] Platform::StartupCapabilities enumerableStartup()
{
    Platform::StartupCapabilities capabilities;
    capabilities.canEnumerate = true;
    capabilities.hasEnabledState = true;
    return capabilities;
}

[[nodiscard]] Platform::ServiceInfo service(const char* name, ServiceState state)
{
    Platform::ServiceInfo info;
    info.name = name;
    info.displayName = std::string(name) + " service";
    info.state = state;
    info.startType = Platform::ServiceStartType::Manual;
    return info;
}

[[nodiscard]] Platform::StartupEntry startupEntry(const char* name, bool enabled)
{
    Platform::StartupEntry entry;
    entry.name = name;
    entry.command = std::string(R"(C:\Apps\)") + name + ".exe";
    entry.location = Platform::StartupLocation::RunUser;
    entry.scope = Platform::StartupScope::User;
    entry.enabled = enabled;
    return entry;
}

/// Where each panel's fakes go once the panel owns them: the test keeps observing pointers (the probe
/// lives as long as the panel's model, the actions as long as the test's shared_ptr).
struct ServiceFakes
{
    MockServiceProbe* probe = nullptr;
    std::shared_ptr<MockServiceActions> actions;
};

struct StartupFakes
{
    MockStartupProbe* probe = nullptr;
    std::shared_ptr<MockStartupActions> actions;
};

class ServicesStartupPanelsRenderTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Context = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.DisplaySize = ImVec2(DISPLAY_WIDTH, DISPLAY_HEIGHT);
        io.DeltaTime = 1.0F / 60.0F;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        io.Fonts->AddFontDefault();
    }

    void TearDown() override
    {
        ImGui::DestroyContext(m_Context);
    }

    /// A Services panel attached to a fake probe (capabilities @p capabilities, answering @p enumeration)
    /// and fake actions, whose observers land in @p fakes.
    [[nodiscard]] static std::unique_ptr<ServicesPanel> attachedServices(ServiceFakes& fakes,
                                                                         Platform::ServiceEnumeration enumeration,
                                                                         Platform::ServiceCapabilities capabilities = enumerableServices(),
                                                                         Platform::ServiceActionCapabilities actionCaps = SERVICE_ACTIONS)
    {
        fakes.actions = std::make_shared<MockServiceActions>(actionCaps);
        auto panel = std::make_unique<ServicesPanel>(
            [&fakes, enumeration = std::move(enumeration), capabilities = std::move(capabilities)]
            {
                auto probe = std::make_unique<MockServiceProbe>(capabilities);
                probe->setEnumeration(enumeration);
                fakes.probe = probe.get();
                return ServicesPanelPlatform{.probe = std::move(probe), .actions = fakes.actions};
            });
        panel->onAttach();
        return panel;
    }

    /// A Startup panel attached to a fake probe listing @p entries and fake actions.
    [[nodiscard]] static std::unique_ptr<StartupPanel> attachedStartup(StartupFakes& fakes,
                                                                       std::vector<Platform::StartupEntry> entries,
                                                                       Platform::StartupCapabilities capabilities = enumerableStartup(),
                                                                       Platform::StartupActionCapabilities actionCaps = STARTUP_ACTIONS)
    {
        fakes.actions = std::make_shared<MockStartupActions>(actionCaps);
        auto panel = std::make_unique<StartupPanel>(
            [&fakes, entries = std::move(entries), capabilities = std::move(capabilities)]
            {
                auto probe = std::make_unique<MockStartupProbe>(capabilities);
                probe->setEntries(entries);
                fakes.probe = probe.get();
                return StartupPanelPlatform{.probe = std::move(probe), .actions = fakes.actions};
            });
        panel->onAttach();
        return panel;
    }

    /// Tells @p panel the main tab changed to @p tabName, as ShellLayer does.
    static void showTab(Panel& panel, std::string tabName)
    {
        Core::ActiveTabChangedEvent event(std::move(tabName));
        panel.onEvent(event);
    }

    /// One frame of a window filling the display, drawing @p panel's content; returns its text. When
    /// @p tableId names a table, @p firstRow gets the centre of its first row's first cell.
    static std::string renderFrame(Panel& panel, const char* tableId = nullptr, ImVec2* firstRow = nullptr)
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
        ImGui::SetNextWindowSize(ImVec2(DISPLAY_WIDTH, DISPLAY_HEIGHT));
        ImGui::Begin("Shell", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
        ImGui::LogToBuffer();
        panel.renderContent();
        std::string captured = GImGui->LogBuffer.c_str();
        ImGui::LogFinish();
        if (tableId != nullptr && firstRow != nullptr)
        {
            if (const ImGuiTable* table = ImGui::TableFindByID(ImGui::GetID(tableId)); table != nullptr)
            {
                const float rowHeight = ImGui::GetTextLineHeight() + (ImGui::GetStyle().CellPadding.y * 2.0F);
                *firstRow = ImVec2((table->Columns[0].MinX + table->Columns[0].MaxX) * 0.5F, table->OuterRect.Min.y + (rowHeight * 1.5F));
            }
        }
        ImGui::End();
        ImGui::Render();
        return captured;
    }

    /// Draws frames until one's text contains @p needle (the sampler thread is committing what the fake
    /// already returned), yielding between them. Returns the last frame's text.
    static std::string renderUntil(Panel& panel, std::string_view needle)
    {
        const auto giveUpAt = std::chrono::steady_clock::now() + MOCK_WAIT_LIMIT;
        std::string text = renderFrame(panel);
        while (!text.contains(needle) && std::chrono::steady_clock::now() < giveUpAt)
        {
            std::this_thread::yield();
            text = renderFrame(panel);
        }
        return text;
    }

    /// Left-clicks the first row of @p panel's table @p tableId, selecting it.
    static void clickFirstRow(Panel& panel, const char* tableId)
    {
        ImVec2 row(-1.0F, -1.0F);
        static_cast<void>(renderFrame(panel, tableId, &row));
        ASSERT_GT(row.y, 0.0F) << "no table drawn";
        ImGuiIO& io = ImGui::GetIO();
        io.AddMousePosEvent(row.x, row.y);
        static_cast<void>(renderFrame(panel));
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        static_cast<void>(renderFrame(panel));
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        static_cast<void>(renderFrame(panel));
        io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
        static_cast<void>(renderFrame(panel));
    }

    /// Clicks the action bar's button labelled @p label (a disabled one does nothing).
    static void clickBarButton(Panel& panel, const char* label)
    {
        static_cast<void>(renderFrame(panel));
        const ImGuiWindow* window = ImGui::FindWindowByName("Shell");
        ASSERT_NE(window, nullptr);
        ImGui::ActivateItemByID(ImHashStr(label, 0, window->ID));
        static_cast<void>(renderFrame(panel));
        static_cast<void>(renderFrame(panel));
    }

  private:
    ImGuiContext* m_Context = nullptr;
};

// ---- Services --------------------------------------------------------------------------------------

TEST_F(ServicesStartupPanelsRenderTest, ServicesPanelDoesNothingBeforeAttachOrAfterDetach)
{
    ServicesPanel panel([] { return ServicesPanelPlatform{}; }); // never called: not attached
    showTab(panel, "Services");                                  // no gate yet: ignored
    EXPECT_TRUE(renderFrame(panel).empty());
    panel.onDetach(); // nothing to stop
    EXPECT_TRUE(renderFrame(panel).empty());
}

TEST_F(ServicesStartupPanelsRenderTest, ServicesSamplingStartsOnFirstShowAndRefreshesOnReShow)
{
    ServiceFakes fakes;
    const auto panel = attachedServices(fakes, Platform::ServiceEnumeration::succeeded({service("Spooler", ServiceState::Running)}));
    ASSERT_NE(fakes.probe, nullptr);

    // Attached but not shown: nothing is read, and the view says it is reading.
    EXPECT_TRUE(renderFrame(*panel).contains("Reading services..."));
    EXPECT_EQ(fakes.probe->enumerations.count(), 0);

    showTab(*panel, "Services");
    ASSERT_TRUE(fakes.probe->enumerations.waitFor(1));
    const std::string table = renderUntil(*panel, "Spooler service");
    EXPECT_TRUE(table.contains("1 of 1 services"));
    EXPECT_TRUE(table.contains("Running"));

    // Another tab closes the gate at once, without joining the sampler; showing it again asks for a
    // fresh read straight away rather than at the next interval.
    showTab(*panel, "Processes");
    const int before = fakes.probe->enumerations.count();
    showTab(*panel, "Services");
    EXPECT_TRUE(fakes.probe->enumerations.waitFor(before + 1));

    panel->onDetach();
    EXPECT_TRUE(renderFrame(*panel).empty()); // the model is gone
}

TEST_F(ServicesStartupPanelsRenderTest, ServicesUnsupportedProbeShowsItsReasonAndNeverSamples)
{
    Platform::ServiceCapabilities unsupported = Platform::UnsupportedServiceProbe{}.capabilities();
    ServiceFakes fakes;
    const auto panel = attachedServices(
        fakes, Platform::ServiceEnumeration::failed("unsupported"), std::move(unsupported), Platform::ServiceActionCapabilities{});
    showTab(*panel, "Services");
    const std::string text = renderFrame(*panel);
    EXPECT_TRUE(text.contains("Services aren't available on this platform yet"));
    EXPECT_FALSE(text.contains("Select a service")); // no action bar either
    showTab(*panel, "Processes");
    showTab(*panel, "Services");
    EXPECT_EQ(fakes.probe->enumerations.count(), 0) << "no sampler is started for a probe that cannot list";
}

TEST_F(ServicesStartupPanelsRenderTest, ServicesEmptyListShowsAnEmptyTable)
{
    ServiceFakes fakes;
    const auto panel = attachedServices(fakes, Platform::ServiceEnumeration::succeeded({}));
    showTab(*panel, "Services");
    ASSERT_TRUE(fakes.probe->enumerations.waitFor(1));
    EXPECT_TRUE(renderUntil(*panel, "0 of 0 services").contains("0 of 0 services"));
}

TEST_F(ServicesStartupPanelsRenderTest, ServicesFailedReadsShowTheReason)
{
    ServiceFakes fakes;
    const auto panel = attachedServices(fakes, Platform::ServiceEnumeration::failed("Access to the Service Control Manager was denied"));
    showTab(*panel, "Services");
    ASSERT_TRUE(fakes.probe->enumerations.waitFor(1));
    // Never read: the reason instead of an empty table.
    const std::string never = renderUntil(*panel, "Couldn't read the services");
    EXPECT_TRUE(never.contains("Couldn't read the services"));
    EXPECT_TRUE(never.contains("was denied"));

    // Read once, then failed: the last rows stay, marked out of date.
    fakes.probe->setEnumeration(Platform::ServiceEnumeration::succeeded({service("Spooler", ServiceState::Running)}));
    showTab(*panel, "Services"); // a re-show refreshes
    EXPECT_TRUE(renderUntil(*panel, "Spooler service").contains("Spooler service"));
    fakes.probe->setEnumeration(Platform::ServiceEnumeration::failed("The RPC server is unavailable"));
    showTab(*panel, "Services");
    const std::string stale = renderUntil(*panel, "Out of date");
    EXPECT_TRUE(stale.contains("Out of date: The RPC server is unavailable"));
    EXPECT_TRUE(stale.contains("Spooler service"));
}

TEST_F(ServicesStartupPanelsRenderTest, ServicesDefaultSortPutsRunningFirst)
{
    // The probe lists them A to Z; the tab opens sorted by State (#1599, #1653): running first.
    ServiceFakes fakes;
    const auto panel = attachedServices(fakes,
                                        Platform::ServiceEnumeration::succeeded({
                                            service("svcAlpha", ServiceState::Stopped),
                                            service("svcBravo", ServiceState::Running),
                                            service("svcCharlie", ServiceState::Stopped),
                                        }));
    showTab(*panel, "Services");
    ASSERT_TRUE(fakes.probe->enumerations.waitFor(1));
    static_cast<void>(renderUntil(*panel, "svcCharlie"));
    const std::string text = renderFrame(*panel); // the sort specs settled
    const auto alpha = text.find("svcAlpha");
    const auto bravo = text.find("svcBravo");
    const auto charlie = text.find("svcCharlie");
    ASSERT_NE(bravo, std::string::npos);
    EXPECT_LT(bravo, alpha);
    EXPECT_LT(alpha, charlie);
}

TEST_F(ServicesStartupPanelsRenderTest, ServicesSelectedRowStartsThroughTheFakeAndRereadsTheConfig)
{
    ServiceFakes fakes;
    const auto panel = attachedServices(fakes, Platform::ServiceEnumeration::succeeded({service("Spooler", ServiceState::Stopped)}));
    showTab(*panel, "Services");
    ASSERT_TRUE(fakes.probe->enumerations.waitFor(1));
    ASSERT_TRUE(renderUntil(*panel, "Spooler service").contains("Select a service"));

    clickFirstRow(*panel, "##ServicesTable");
    EXPECT_FALSE(renderFrame(*panel).contains("Select a service")) << "the click selected the row";

    clickBarButton(*panel, ICON_FA_PLAY " Start"); // a start needs no confirm
    ASSERT_TRUE(fakes.actions->starts.waitFor(1));
    EXPECT_EQ(fakes.actions->lastName(), "Spooler");

    // The frame that takes the finished action shows its result and asks for a re-read of the services,
    // configuration included, without waiting the interval.
    EXPECT_TRUE(renderUntil(*panel, "Started Spooler").contains("Started Spooler"));
    EXPECT_TRUE(fakes.probe->configForgets.waitFor(1));
    EXPECT_EQ(fakes.actions->stops.count(), 0);
}

TEST_F(ServicesStartupPanelsRenderTest, ServicesFailedActionShowsItsReason)
{
    ServiceFakes fakes;
    const auto panel = attachedServices(fakes, Platform::ServiceEnumeration::succeeded({service("Spooler", ServiceState::Stopped)}));
    fakes.actions->setResult(Platform::ServiceActionResult::failed("Requires administrator"));
    showTab(*panel, "Services");
    ASSERT_TRUE(fakes.probe->enumerations.waitFor(1));
    static_cast<void>(renderUntil(*panel, "Spooler service"));
    clickFirstRow(*panel, "##ServicesTable");
    clickBarButton(*panel, ICON_FA_PLAY " Start");
    ASSERT_TRUE(fakes.actions->starts.waitFor(1));
    EXPECT_TRUE(renderUntil(*panel, "Could not start").contains("Could not start Spooler: Requires administrator"));
}

TEST_F(ServicesStartupPanelsRenderTest, ServicesDetachCancelsARunningActionPromptly)
{
    ServiceFakes fakes;
    const auto panel = attachedServices(fakes, Platform::ServiceEnumeration::succeeded({service("Spooler", ServiceState::Stopped)}));
    fakes.actions->blockUntilCancelled();
    showTab(*panel, "Services");
    ASSERT_TRUE(fakes.probe->enumerations.waitFor(1));
    static_cast<void>(renderUntil(*panel, "Spooler service"));
    clickFirstRow(*panel, "##ServicesTable");
    clickBarButton(*panel, ICON_FA_PLAY " Start");
    ASSERT_TRUE(fakes.actions->starts.waitFor(1)); // running on the worker, waiting for its stop token
    EXPECT_TRUE(renderFrame(*panel).contains("Starting Spooler..."));

    const auto before = std::chrono::steady_clock::now();
    panel->onDetach();
    const auto took = std::chrono::steady_clock::now() - before;
    EXPECT_TRUE(fakes.actions->sawStop.load()) << "onDetach() cancelled the action rather than waiting it out";
    EXPECT_LT(took, MOCK_WAIT_LIMIT / 2);
}

// ---- Startup ---------------------------------------------------------------------------------------

TEST_F(ServicesStartupPanelsRenderTest, StartupPanelDoesNothingBeforeAttachOrAfterDetach)
{
    StartupPanel panel([] { return StartupPanelPlatform{}; }); // never called: not attached
    showTab(panel, "Startup");
    EXPECT_TRUE(renderFrame(panel).empty());
    panel.onDetach();
    EXPECT_TRUE(renderFrame(panel).empty());
}

TEST_F(ServicesStartupPanelsRenderTest, StartupSamplingStartsOnFirstShowAndRefreshesOnReShow)
{
    StartupFakes fakes;
    const auto panel = attachedStartup(fakes, {startupEntry("OneDrive", true)});
    ASSERT_NE(fakes.probe, nullptr);
    EXPECT_TRUE(renderFrame(*panel).contains("Reading startup apps..."));
    EXPECT_EQ(fakes.probe->enumerations.count(), 0);

    showTab(*panel, "Startup");
    ASSERT_TRUE(fakes.probe->enumerations.waitFor(1));
    EXPECT_TRUE(renderUntil(*panel, "OneDrive").contains(R"(C:\Apps\OneDrive.exe)"));

    showTab(*panel, "Services");
    const int before = fakes.probe->enumerations.count();
    showTab(*panel, "Startup");
    EXPECT_TRUE(fakes.probe->enumerations.waitFor(before + 1));

    panel->onDetach();
    EXPECT_TRUE(renderFrame(*panel).empty());
}

TEST_F(ServicesStartupPanelsRenderTest, StartupUnsupportedProbeShowsItsReasonAndNeverSamples)
{
    StartupFakes fakes;
    const auto panel = attachedStartup(
        fakes, {}, Platform::UnsupportedStartupProbe{}.capabilities(), Platform::UnsupportedStartupActions{}.capabilities());
    showTab(*panel, "Startup");
    const std::string text = renderFrame(*panel);
    EXPECT_TRUE(text.contains("Startup apps aren't available on this platform yet"));
    EXPECT_FALSE(text.contains("Select a startup app"));
    EXPECT_EQ(fakes.probe->enumerations.count(), 0);
}

TEST_F(ServicesStartupPanelsRenderTest, StartupEmptyListShowsAnEmptyTable)
{
    StartupFakes fakes;
    const auto panel = attachedStartup(fakes, {});
    showTab(*panel, "Startup");
    ASSERT_TRUE(fakes.probe->enumerations.waitFor(1));
    EXPECT_TRUE(renderUntil(*panel, "0 of 0").contains("0 of 0"));
}

TEST_F(ServicesStartupPanelsRenderTest, StartupDefaultSortPutsEnabledFirst)
{
    // The probe lists them A to Z; the tab opens sorted by Enabled (#1597, #1653): enabled first.
    StartupFakes fakes;
    const auto panel =
        attachedStartup(fakes, {startupEntry("appAlpha", false), startupEntry("appBravo", true), startupEntry("appCharlie", false)});
    showTab(*panel, "Startup");
    ASSERT_TRUE(fakes.probe->enumerations.waitFor(1));
    static_cast<void>(renderUntil(*panel, "appCharlie"));
    const std::string text = renderFrame(*panel);
    const auto alpha = text.find("appAlpha");
    const auto bravo = text.find("appBravo");
    const auto charlie = text.find("appCharlie");
    ASSERT_NE(bravo, std::string::npos);
    EXPECT_LT(bravo, alpha);
    EXPECT_LT(alpha, charlie);
}

TEST_F(ServicesStartupPanelsRenderTest, StartupSelectedRowEnablesThroughTheFakeAndRefreshes)
{
    StartupFakes fakes;
    const auto panel = attachedStartup(fakes, {startupEntry("OneDrive", false)});
    showTab(*panel, "Startup");
    ASSERT_TRUE(fakes.probe->enumerations.waitFor(1));
    ASSERT_TRUE(renderUntil(*panel, "OneDrive").contains("Select a startup app"));

    clickFirstRow(*panel, "##StartupTable");
    EXPECT_FALSE(renderFrame(*panel).contains("Select a startup app"));

    const int before = fakes.probe->enumerations.count();
    clickBarButton(*panel, ICON_FA_CIRCLE_CHECK " Enable"); // enabling needs no confirm
    ASSERT_TRUE(fakes.actions->enables.waitFor(1));
    EXPECT_EQ(fakes.actions->lastName(), "OneDrive");
    EXPECT_EQ(fakes.actions->disables.count(), 0);

    // The finished action asks the sampler for a fresh read, so the Enabled column updates.
    EXPECT_TRUE(renderUntil(*panel, "Enabled OneDrive").contains("Enabled OneDrive"));
    EXPECT_TRUE(fakes.probe->enumerations.waitFor(before + 1));
}

TEST_F(ServicesStartupPanelsRenderTest, StartupFailedActionShowsItsReason)
{
    StartupFakes fakes;
    const auto panel = attachedStartup(fakes, {startupEntry("OneDrive", false)});
    fakes.actions->setResult(Platform::StartupActionResult::failed("Access is denied"));
    showTab(*panel, "Startup");
    ASSERT_TRUE(fakes.probe->enumerations.waitFor(1));
    static_cast<void>(renderUntil(*panel, "OneDrive"));
    clickFirstRow(*panel, "##StartupTable");
    clickBarButton(*panel, ICON_FA_CIRCLE_CHECK " Enable");
    ASSERT_TRUE(fakes.actions->enables.waitFor(1));
    EXPECT_TRUE(renderUntil(*panel, "Could not enable").contains("Could not enable OneDrive: Access is denied"));
}

TEST_F(ServicesStartupPanelsRenderTest, StartupDetachWaitsForAWriteInFlight)
{
    StartupFakes fakes;
    const auto panel = attachedStartup(fakes, {startupEntry("OneDrive", false)});
    fakes.actions->holdUntilReleased();
    showTab(*panel, "Startup");
    ASSERT_TRUE(fakes.probe->enumerations.waitFor(1));
    static_cast<void>(renderUntil(*panel, "OneDrive"));
    clickFirstRow(*panel, "##StartupTable");
    clickBarButton(*panel, ICON_FA_CIRCLE_CHECK " Enable");
    ASSERT_TRUE(fakes.actions->enables.waitFor(1)); // the write is in flight on the worker
    EXPECT_FALSE(fakes.actions->returned.load());

    // A registry write is one short call with no stop token: onDetach() lets it finish rather than
    // abandon it half done.
    fakes.actions->release();
    panel->onDetach();
    EXPECT_TRUE(fakes.actions->returned.load());
}

} // namespace
} // namespace App
