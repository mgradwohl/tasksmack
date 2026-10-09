/// @file test_ServiceActionsView.cpp
/// @brief The Services tab's actions (#1577): the critical-service list, which actions apply in which
/// state, the confirm and result texts, and, headless, the row menu enabling its items per state, the
/// centred confirm with the critical warning, and the result line after the worker finishes. The
/// actions are a fake: no real service is touched.

#include "App/Panels/ServiceActionsView.h"
#include "Platform/IServiceActions.h"
#include "Platform/IServiceProbe.h"
#include "UI/IconsFontAwesome6.h"

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h> // OpenPopupStack, GetTopMostPopupModal(), ImHashStr(), ActivateItemByID()

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

namespace App
{
namespace
{

using Platform::ServiceState;
namespace SvcDetail = ServiceActionsDetail;

constexpr Platform::ServiceActionCapabilities ALL{
    .canStart = true, .canStop = true, .canRestart = true, .canSetStartType = true, .elevated = false};

/// Answers every action with `result`, counting the calls (made on the action worker).
class FakeServiceActions final : public Platform::IServiceActions
{
  public:
    Platform::ServiceActionResult result = Platform::ServiceActionResult::succeeded();
    std::atomic<int> starts{0};
    std::atomic<int> stops{0};
    std::atomic<int> startTypes{0};

    [[nodiscard]] Platform::ServiceActionCapabilities capabilities() const override
    {
        return ALL;
    }
    [[nodiscard]] Platform::ServiceActionResult start(std::string_view /*name*/) override
    {
        ++starts;
        return result;
    }
    [[nodiscard]] Platform::ServiceActionResult stop(std::string_view /*name*/) override
    {
        ++stops;
        return result;
    }
    [[nodiscard]] Platform::ServiceActionResult restart(std::string_view /*name*/) override
    {
        return result;
    }
    [[nodiscard]] Platform::ServiceActionResult setStartType(std::string_view /*name*/, Platform::ServiceStartType /*type*/) override
    {
        ++startTypes;
        return result;
    }
};

[[nodiscard]] Platform::ServiceInfo service(const char* name, ServiceState state)
{
    Platform::ServiceInfo info;
    info.name = name;
    info.displayName = std::string(name) + " service";
    info.state = state;
    info.startType = Platform::ServiceStartType::Manual;
    return info;
}

/// Waits (briefly: the fake answers at once) for the worker, as the panel's per-frame poll does.
[[nodiscard]] bool waitFinished(ServiceActionsView& view)
{
    for (int i = 0; i < 300; ++i)
    {
        if (view.takeFinished())
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

TEST(ServiceActionsDetailTest, CriticalServicesAreKnown)
{
    EXPECT_TRUE(SvcDetail::isCriticalService("RpcSs", "Shared process"));
    EXPECT_TRUE(SvcDetail::isCriticalService("eventlog", "Shared process"));
    EXPECT_TRUE(SvcDetail::isCriticalService("Winmgmt", ""));
    EXPECT_TRUE(SvcDetail::isCriticalService("Dnscache", ""));
    EXPECT_TRUE(SvcDetail::isCriticalService("SomeFilter", "Driver"));
    EXPECT_FALSE(SvcDetail::isCriticalService("Spooler", "Own process"));
    EXPECT_FALSE(SvcDetail::isCriticalService("RpcSsX", ""));
}

TEST(ServiceActionsDetailTest, ActionsApplyPerState)
{
    using enum ServiceActionKind;
    EXPECT_TRUE(SvcDetail::isApplicable(Start, ServiceState::Stopped, ALL));
    EXPECT_FALSE(SvcDetail::isApplicable(Start, ServiceState::Running, ALL));
    EXPECT_TRUE(SvcDetail::isApplicable(Stop, ServiceState::Running, ALL));
    EXPECT_TRUE(SvcDetail::isApplicable(Stop, ServiceState::Paused, ALL));
    EXPECT_FALSE(SvcDetail::isApplicable(Stop, ServiceState::StopPending, ALL));
    EXPECT_TRUE(SvcDetail::isApplicable(Restart, ServiceState::Running, ALL));
    EXPECT_FALSE(SvcDetail::isApplicable(Restart, ServiceState::Stopped, ALL));
    EXPECT_TRUE(SvcDetail::isApplicable(SetStartType, ServiceState::Unknown, ALL));
    EXPECT_FALSE(SvcDetail::isApplicable(Stop, ServiceState::Running, Platform::ServiceActionCapabilities{}));
}

TEST(ServiceActionsDetailTest, DestructiveActionsConfirm)
{
    const auto spooler = service("Spooler", ServiceState::Running);
    using enum ServiceActionKind;
    EXPECT_FALSE(SvcDetail::needsConfirm(SvcDetail::makeRequest(Start, spooler)));
    EXPECT_TRUE(SvcDetail::needsConfirm(SvcDetail::makeRequest(Stop, spooler)));
    EXPECT_TRUE(SvcDetail::needsConfirm(SvcDetail::makeRequest(Restart, spooler)));
    EXPECT_TRUE(SvcDetail::needsConfirm(SvcDetail::makeRequest(SetStartType, spooler, Platform::ServiceStartType::Disabled)));
    EXPECT_FALSE(SvcDetail::needsConfirm(SvcDetail::makeRequest(SetStartType, spooler, Platform::ServiceStartType::Manual)));
}

TEST(ServiceActionsDetailTest, TextsNameTheServiceAndTheOutcome)
{
    const auto stop = SvcDetail::makeRequest(ServiceActionKind::Stop, service("Spooler", ServiceState::Running));
    EXPECT_EQ(SvcDetail::confirmTitle(stop), "Stop Spooler?");
    EXPECT_FALSE(SvcDetail::confirmQuestion(stop).contains("critical"));
    EXPECT_EQ(SvcDetail::progressText(stop), "Stopping Spooler...");
    EXPECT_EQ(SvcDetail::resultMessage(stop, Platform::ServiceActionResult::succeeded()).text, "Stopped Spooler");
    const auto denied = SvcDetail::resultMessage(stop, Platform::ServiceActionResult::failed("Requires administrator"));
    EXPECT_FALSE(denied.ok);
    EXPECT_EQ(denied.text, "Could not stop Spooler: Requires administrator");

    const auto delayed = SvcDetail::makeRequest(
        ServiceActionKind::SetStartType, service("Spooler", ServiceState::Running), Platform::ServiceStartType::AutomaticDelayed);
    EXPECT_EQ(SvcDetail::resultMessage(delayed, Platform::ServiceActionResult::succeeded()).text, "Set Spooler to Automatic (delayed)");

    const auto critical = SvcDetail::makeRequest(ServiceActionKind::Stop, service("RpcSs", ServiceState::Running));
    EXPECT_TRUE(critical.critical);
    EXPECT_TRUE(SvcDetail::confirmQuestion(critical).contains("RpcSs is critical to Windows"));
}

TEST(ServiceActionsViewTest, UnsupportedPlatformHasNoActions)
{
    const ServiceActionsView view(std::make_shared<Platform::UnsupportedServiceActions>());
    EXPECT_FALSE(view.supported());
}

class ServiceActionsViewRenderTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Context = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.DisplaySize = ImVec2(1200.0F, 800.0F);
        io.DeltaTime = 1.0F / 60.0F;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        io.Fonts->AddFontDefault();
    }

    void TearDown() override
    {
        ImGui::DestroyContext(m_Context);
    }

    /// One frame of the Services window: a row for @p row with its menu (opened when @p openMenu),
    /// the result line and the confirm, as ServicesPanel draws them.
    static void frame(ServiceActionsView& view, const Platform::ServiceInfo& row, bool openMenu = false)
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
        ImGui::SetNextWindowSize(ImVec2(1200.0F, 800.0F));
        ImGui::Begin("Services", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
        ImGui::PushID(row.name.c_str());
        ImGui::Selectable(row.name.c_str());
        if (openMenu)
        {
            ImGui::OpenPopup("##ServiceRowMenu");
        }
        view.renderContextMenu(row);
        ImGui::PopID();
        view.renderResultLine();
        view.renderConfirmation();
        ImGui::End();
        ImGui::Render();
    }

    /// Opens @p row's menu and picks its item labelled @p label: a disabled item does nothing.
    static void pickMenuItem(ServiceActionsView& view, const Platform::ServiceInfo& row, const char* label)
    {
        frame(view, row, true);
        frame(view, row);
        const ImGuiContext& g = *ImGui::GetCurrentContext();
        ASSERT_FALSE(g.OpenPopupStack.empty());
        ImGui::ActivateItemByID(ImHashStr(label, 0, g.OpenPopupStack.back().Window->ID));
        frame(view, row);
        frame(view, row);
    }

  private:
    ImGuiContext* m_Context = nullptr;
};

TEST_F(ServiceActionsViewRenderTest, RowMenuEnablesOnlyTheActionsThatApply)
{
    auto fake = std::make_shared<FakeServiceActions>();
    ServiceActionsView view(fake);
    const auto running = service("Spooler", ServiceState::Running);

    pickMenuItem(view, running, ICON_FA_PLAY "  Start"); // disabled: it is running
    EXPECT_FALSE(view.confirmRequested());
    EXPECT_FALSE(view.busy());

    pickMenuItem(view, running, ICON_FA_STOP "  Stop"); // enabled, and asks first
    EXPECT_TRUE(view.confirmRequested());
    ASSERT_TRUE(view.pendingConfirm().has_value());
    EXPECT_EQ(view.pendingConfirm().value_or(ServiceActionRequest{}).kind, ServiceActionKind::Stop);
    EXPECT_EQ(fake->stops.load(), 0);
}

TEST_F(ServiceActionsViewRenderTest, StartFromTheMenuRunsWithoutAConfirm)
{
    auto fake = std::make_shared<FakeServiceActions>();
    ServiceActionsView view(fake);
    const auto stopped = service("Spooler", ServiceState::Stopped);

    pickMenuItem(view, stopped, ICON_FA_STOP "  Stop"); // disabled: it is stopped
    EXPECT_FALSE(view.confirmRequested());
    pickMenuItem(view, stopped, ICON_FA_PLAY "  Start");
    EXPECT_FALSE(view.confirmRequested());
    ASSERT_TRUE(waitFinished(view));
    EXPECT_EQ(fake->starts.load(), 1);
    EXPECT_EQ(view.lastResult().text, "Started Spooler");
}

TEST_F(ServiceActionsViewRenderTest, CriticalStopConfirmsCentredThenShowsTheResult)
{
    auto fake = std::make_shared<FakeServiceActions>();
    fake->result = Platform::ServiceActionResult::failed("Requires administrator");
    ServiceActionsView view(fake);
    const auto rpcss = service("RpcSs", ServiceState::Running);
    view.request(SvcDetail::makeRequest(ServiceActionKind::Stop, rpcss));
    for (int i = 0; i < 3; ++i)
    {
        frame(view, rpcss);
    }

    const ImGuiWindow* modal = ImGui::GetTopMostPopupModal();
    ASSERT_NE(modal, nullptr);
    EXPECT_TRUE(std::string(modal->Name).starts_with("Stop RpcSs?"));
    EXPECT_TRUE(SvcDetail::confirmQuestion(view.pendingConfirm().value_or(ServiceActionRequest{})).contains("critical to Windows"));
    const ImVec2 centre = ImGui::GetMainViewport()->GetWorkCenter();
    EXPECT_NEAR(modal->Pos.x + (modal->Size.x * 0.5F), centre.x, 1.0F);
    EXPECT_NEAR(modal->Pos.y + (modal->Size.y * 0.5F), centre.y, 1.0F);

    // A danger-filled button: its ID is "##filled" under the label (UI::Widgets::filledButton()).
    ImGui::ActivateItemByID(ImHashStr("##filled", 0, ImHashStr("Stop", 0, modal->ID)));
    frame(view, rpcss);
    frame(view, rpcss);
    ASSERT_TRUE(waitFinished(view));
    EXPECT_EQ(fake->stops.load(), 1);
    EXPECT_FALSE(view.lastResult().ok);
    EXPECT_EQ(view.lastResult().text, "Could not stop RpcSs: Requires administrator");
    frame(view, rpcss); // the result line draws
    EXPECT_GT(ImGui::GetDrawData()->TotalVtxCount, 0);

    view.tick(SvcDetail::RESULT_SECONDS + 1.0F);
    EXPECT_TRUE(view.lastResult().text.empty());
}

} // namespace
} // namespace App
