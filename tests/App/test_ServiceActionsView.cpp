/// @file test_ServiceActionsView.cpp
/// @brief The Services tab's actions (#1577): the critical-service list, which actions apply in which
/// state, the confirm and result texts, and, headless, the row menu enabling its items per state, the
/// centred confirm with the critical warning, and the result line after the worker finishes; and a
/// running action cancelled by cancel() or the view's destruction (#1591), through a gated fake that
/// returns only once its stop token is stopped. The actions are fakes: no real service is touched.

#include "App/Panels/ServiceActionsView.h"
#include "Platform/IServiceActions.h"
#include "Platform/IServiceProbe.h"
#include "UI/IconsFontAwesome6.h"

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h> // OpenPopupStack, GetTopMostPopupModal(), ImHashStr(), ActivateItemByID()

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <latch>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <stop_token>
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
    explicit FakeServiceActions(Platform::ServiceActionCapabilities caps = ALL) : m_Caps(caps)
    {}

    Platform::ServiceActionResult result = Platform::ServiceActionResult::succeeded();
    std::atomic<int> starts{0};
    std::atomic<int> stops{0};
    std::atomic<int> startTypes{0};
    std::atomic<Platform::ServiceStartType> lastStartType{Platform::ServiceStartType::Unknown};

    [[nodiscard]] Platform::ServiceActionCapabilities capabilities() const override
    {
        return m_Caps;
    }
    [[nodiscard]] Platform::ServiceActionResult start(std::string_view /*name*/, const std::stop_token& /*stopToken*/) override
    {
        ++starts;
        return result;
    }
    [[nodiscard]] Platform::ServiceActionResult stop(std::string_view /*name*/, const std::stop_token& /*stopToken*/) override
    {
        ++stops;
        return result;
    }
    [[nodiscard]] Platform::ServiceActionResult restart(std::string_view /*name*/, const std::stop_token& /*stopToken*/) override
    {
        return result;
    }
    [[nodiscard]] Platform::ServiceActionResult setStartType(std::string_view /*name*/, Platform::ServiceStartType type) override
    {
        lastStartType = type;
        ++startTypes;
        return result;
    }

  private:
    Platform::ServiceActionCapabilities m_Caps;
};

/// How long the gated fake waits for its stop request before giving up: a bound for a broken build
/// only, never reached when cancelling works.
constexpr auto GATE_LIMIT = std::chrono::seconds(10);

/// Start / Stop / Restart block, as a slow service's wait does, until their stop token is stopped,
/// then answer cancelled, as WindowsServiceActions does. No sleeps: a condition variable woken by the
/// stop request. `entered` opens once an action is running on the worker.
class GatedServiceActions final : public Platform::IServiceActions
{
  public:
    std::latch entered{1};
    std::atomic<bool> sawStop{false};

    [[nodiscard]] Platform::ServiceActionCapabilities capabilities() const override
    {
        return ALL;
    }
    [[nodiscard]] Platform::ServiceActionResult start(std::string_view /*name*/, const std::stop_token& stopToken) override
    {
        return block(stopToken);
    }
    [[nodiscard]] Platform::ServiceActionResult stop(std::string_view /*name*/, const std::stop_token& stopToken) override
    {
        return block(stopToken);
    }
    [[nodiscard]] Platform::ServiceActionResult restart(std::string_view /*name*/, const std::stop_token& stopToken) override
    {
        return block(stopToken);
    }
    [[nodiscard]] Platform::ServiceActionResult setStartType(std::string_view /*name*/, Platform::ServiceStartType /*type*/) override
    {
        return Platform::ServiceActionResult::succeeded();
    }

  private:
    [[nodiscard]] Platform::ServiceActionResult block(const std::stop_token& stopToken)
    {
        entered.count_down();
        std::unique_lock lock(m_Mutex);
        // Woken by the stop request itself; the predicate is the stop, so a spurious wake waits on.
        static_cast<void>(m_Wake.wait_for(lock, stopToken, GATE_LIMIT, [] { return false; }));
        if (!stopToken.stop_requested())
        {
            return Platform::ServiceActionResult::failed("not cancelled");
        }
        sawStop = true;
        return Platform::ServiceActionResult::stopRequested();
    }

    std::mutex m_Mutex;
    std::condition_variable_any m_Wake;
};

/// Every action throws, on the worker.
class ThrowingServiceActions final : public Platform::IServiceActions
{
  public:
    [[nodiscard]] Platform::ServiceActionCapabilities capabilities() const override
    {
        return ALL;
    }
    [[nodiscard]] Platform::ServiceActionResult start(std::string_view /*name*/, const std::stop_token& /*stopToken*/) override
    {
        throw std::runtime_error("boom");
    }
    [[nodiscard]] Platform::ServiceActionResult stop(std::string_view /*name*/, const std::stop_token& /*stopToken*/) override
    {
        throw std::runtime_error("boom");
    }
    [[nodiscard]] Platform::ServiceActionResult restart(std::string_view /*name*/, const std::stop_token& /*stopToken*/) override
    {
        throw std::runtime_error("boom");
    }
    [[nodiscard]] Platform::ServiceActionResult setStartType(std::string_view /*name*/, Platform::ServiceStartType /*type*/) override
    {
        throw std::runtime_error("boom");
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

/// Waits for an action the fake answers at once, without sleeping: the worker's future is ready as
/// soon as the fake returns, so this yields until takeFinished() takes it. GATE_LIMIT bounds a broken
/// build only.
[[nodiscard]] bool finishWithoutSleeping(ServiceActionsView& view)
{
    const auto giveUpAt = std::chrono::steady_clock::now() + GATE_LIMIT;
    while (!view.takeFinished())
    {
        if (std::chrono::steady_clock::now() > giveUpAt)
        {
            return false;
        }
        std::this_thread::yield();
    }
    return true;
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
    // #2118: the systemd units a Linux machine can't run well without; unit names are exact.
    EXPECT_TRUE(SvcDetail::isCriticalService("dbus.service", "simple"));
    EXPECT_TRUE(SvcDetail::isCriticalService("systemd-logind.service", ""));
    EXPECT_TRUE(SvcDetail::isCriticalService("ssh.service", ""));
    EXPECT_TRUE(SvcDetail::isCriticalService("NetworkManager.service", ""));
    EXPECT_FALSE(SvcDetail::isCriticalService("networkmanager.service", ""));
    EXPECT_FALSE(SvcDetail::isCriticalService("cups.service", ""));
    // #2471: a user's manager, whatever the uid, which "Show in Services" leads to from their user services.
    EXPECT_TRUE(SvcDetail::isCriticalService("user@1000.service", ""));
    EXPECT_TRUE(SvcDetail::isCriticalService("user@0.service", ""));
    EXPECT_TRUE(SvcDetail::isCriticalService("user-runtime-dir@1000.service", ""));
    EXPECT_FALSE(SvcDetail::isCriticalService("userdata.service", ""));
    EXPECT_FALSE(SvcDetail::isCriticalService("users.service", ""));
    EXPECT_FALSE(SvcDetail::isCriticalService("user@1000.socket", "")) << "only the service";
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

// #2118: Manual on a critical service that starts at boot asks first -- after the next restart it won't
// be running -- and says so; Manual on anything else, or on one that isn't started at boot, doesn't.
TEST(ServiceActionsDetailTest, ManualOnACriticalBootServiceConfirms)
{
    using enum Platform::ServiceStartType;
    auto ssh = service("ssh.service", ServiceState::Running);
    ssh.startType = Automatic;
    const auto manual = SvcDetail::makeRequest(ServiceActionKind::SetStartType, ssh, Manual);
    EXPECT_EQ(manual.fromStartType, Automatic);
    EXPECT_TRUE(SvcDetail::needsConfirm(manual));
    EXPECT_EQ(SvcDetail::confirmTitle(manual), "Set to Manual ssh.service?");
    EXPECT_TRUE(SvcDetail::confirmQuestion(manual).contains("is critical to")) << SvcDetail::confirmQuestion(manual);
    EXPECT_TRUE(SvcDetail::confirmQuestion(manual).contains("won't start at boot any more")) << SvcDetail::confirmQuestion(manual);

    ssh.startType = AutomaticDelayed;
    EXPECT_TRUE(SvcDetail::needsConfirm(SvcDetail::makeRequest(ServiceActionKind::SetStartType, ssh, Manual)));
    ssh.startType = Disabled; // not started at boot: Manual starts nothing new and stops nothing
    EXPECT_FALSE(SvcDetail::needsConfirm(SvcDetail::makeRequest(ServiceActionKind::SetStartType, ssh, Manual)));
    auto cups = service("cups.service", ServiceState::Running);
    cups.startType = Automatic;
    EXPECT_FALSE(SvcDetail::needsConfirm(SvcDetail::makeRequest(ServiceActionKind::SetStartType, cups, Manual)));
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
#ifdef _WIN32
    EXPECT_TRUE(SvcDetail::confirmQuestion(critical).contains("RpcSs is critical to Windows"));
#else
    EXPECT_TRUE(SvcDetail::confirmQuestion(critical).contains("RpcSs is critical to the system"));
#endif
}

// #1891: every action's progress and outcome in words, failures included: Restart's verb, and a start
// type that couldn't be set naming the type it was going to.
TEST(ServiceActionsDetailTest, EachActionSaysWhatItDoesAndWhatFailed)
{
    const Platform::ServiceInfo running = service("Spooler", ServiceState::Running);
    const auto failed = Platform::ServiceActionResult::failed("Access is denied");

    const auto start = SvcDetail::makeRequest(ServiceActionKind::Start, service("Spooler", ServiceState::Stopped));
    EXPECT_EQ(SvcDetail::progressText(start), "Starting Spooler...");
    EXPECT_EQ(SvcDetail::resultMessage(start, failed).text, "Could not start Spooler: Access is denied");

    const auto restart = SvcDetail::makeRequest(ServiceActionKind::Restart, running);
    EXPECT_EQ(SvcDetail::progressText(restart), "Restarting Spooler...");
    EXPECT_EQ(SvcDetail::resultMessage(restart, Platform::ServiceActionResult::succeeded()).text, "Restarted Spooler");
    EXPECT_EQ(SvcDetail::resultMessage(restart, failed).text, "Could not restart Spooler: Access is denied");

    const auto disable = SvcDetail::makeRequest(ServiceActionKind::SetStartType, running, Platform::ServiceStartType::Disabled);
    EXPECT_EQ(SvcDetail::progressText(disable), "Setting Spooler to Disabled...");
    const auto refused = SvcDetail::resultMessage(disable, failed);
    EXPECT_FALSE(refused.ok);
    EXPECT_EQ(refused.text, "Could not set Spooler to Disabled: Access is denied");
}

TEST(ServiceActionsViewTest, UnsupportedPlatformHasNoActions)
{
    const ServiceActionsView view(std::make_shared<Platform::UnsupportedServiceActions>());
    EXPECT_FALSE(view.supported());
}

TEST(ServiceActionsViewTest, CancelEndsTheRunningActionAsCancelled)
{
    const auto fake = std::make_shared<GatedServiceActions>();
    ServiceActionsView view(fake);
    view.request(SvcDetail::makeRequest(ServiceActionKind::Start, service("Spooler", ServiceState::Stopped))); // no confirm
    fake->entered.wait();
    EXPECT_TRUE(view.busy());
    EXPECT_FALSE(view.takeFinished());

    view.cancel();
    ASSERT_TRUE(waitFinished(view));
    EXPECT_TRUE(fake->sawStop.load());
    EXPECT_FALSE(view.busy());
    EXPECT_FALSE(view.lastResult().ok);
    EXPECT_EQ(view.lastResult().text, "Could not start Spooler: Cancelled before it finished");
}

TEST(ServiceActionsViewTest, DestroyingTheViewCancelsTheRunningActionAndReturnsPromptly)
{
    const auto fake = std::make_shared<GatedServiceActions>();
    auto view = std::make_unique<ServiceActionsView>(fake);
    view->request(SvcDetail::makeRequest(ServiceActionKind::Start, service("Spooler", ServiceState::Stopped)));
    fake->entered.wait();

    const auto before = std::chrono::steady_clock::now();
    view.reset(); // as ServicesPanel::onDetach() does
    const auto took = std::chrono::steady_clock::now() - before;
    EXPECT_TRUE(fake->sawStop.load()) << "the destructor stopped the action rather than waiting it out";
    EXPECT_LT(took, GATE_LIMIT / 2);
}

/// The plain actions are refused as needing a password prompt; runAuthorized() answers `authorized`,
/// recording what it was asked (#2304).
class AuthorizingServiceActions final : public Platform::IServiceActions
{
  public:
    explicit AuthorizingServiceActions(bool canAuthorize = true)
    {
        m_Caps.canAuthorizeInteractively = canAuthorize;
    }

    Platform::ServiceActionResult authorized = Platform::ServiceActionResult::succeeded();
    std::atomic<int> authorizedCalls{0};
    std::atomic<Platform::ServiceAction> lastAction{Platform::ServiceAction::Restart};

    [[nodiscard]] Platform::ServiceActionCapabilities capabilities() const override
    {
        return m_Caps;
    }
    [[nodiscard]] Platform::ServiceActionResult start(std::string_view /*name*/, const std::stop_token& /*stopToken*/) override
    {
        return refused();
    }
    [[nodiscard]] Platform::ServiceActionResult stop(std::string_view /*name*/, const std::stop_token& /*stopToken*/) override
    {
        return refused();
    }
    [[nodiscard]] Platform::ServiceActionResult restart(std::string_view /*name*/, const std::stop_token& /*stopToken*/) override
    {
        return refused();
    }
    [[nodiscard]] Platform::ServiceActionResult setStartType(std::string_view /*name*/, Platform::ServiceStartType /*type*/) override
    {
        return refused();
    }
    [[nodiscard]] Platform::ServiceActionResult runAuthorized(Platform::ServiceAction action,
                                                              std::string_view /*name*/,
                                                              Platform::ServiceStartType /*startType*/,
                                                              const std::stop_token& /*stopToken*/) override
    {
        lastAction = action;
        ++authorizedCalls;
        return authorized;
    }

  private:
    [[nodiscard]] static Platform::ServiceActionResult refused()
    {
        Platform::ServiceActionResult result = Platform::ServiceActionResult::failed("Requires administrator");
        result.needsAuthorization = true;
        return result;
    }

    Platform::ServiceActionCapabilities m_Caps = ALL;
};

TEST(ServiceActionsViewTest, ARefusalAPromptCouldAllowOffersAnAuthorizedRetry)
{
    const auto fake = std::make_shared<AuthorizingServiceActions>();
    ServiceActionsView view(fake);
    view.request(SvcDetail::makeRequest(ServiceActionKind::Start, service("cron.service", ServiceState::Stopped)));
    ASSERT_TRUE(waitFinished(view));
    EXPECT_EQ(view.lastResult().text, "Could not start cron.service: Requires administrator");
    EXPECT_TRUE(view.lastResult().canAuthorize);
    EXPECT_EQ(fake->authorizedCalls.load(), 0) << "the plain action never prompts";

    view.retryAuthorized();
    ASSERT_TRUE(waitFinished(view));
    EXPECT_EQ(fake->authorizedCalls.load(), 1);
    EXPECT_EQ(fake->lastAction.load(), Platform::ServiceAction::Start);
    EXPECT_TRUE(view.lastResult().ok);
    EXPECT_EQ(view.lastResult().text, "Started cron.service");
    EXPECT_FALSE(view.lastResult().canAuthorize);
}

TEST(ServiceActionsViewTest, NoAuthorizedRetryWhereThePlatformCantPrompt)
{
    const auto fake = std::make_shared<AuthorizingServiceActions>(/*canAuthorize=*/false);
    ServiceActionsView view(fake);
    view.request(SvcDetail::makeRequest(ServiceActionKind::Start, service("cron.service", ServiceState::Stopped)));
    ASSERT_TRUE(waitFinished(view));
    EXPECT_FALSE(view.lastResult().canAuthorize);
    view.retryAuthorized(); // ignored
    EXPECT_FALSE(view.busy());
    EXPECT_EQ(fake->authorizedCalls.load(), 0);
}

TEST(ServiceActionsViewTest, WithNoAgentToPromptTheRetryIsntOfferedAgainThisSession)
{
    const auto fake = std::make_shared<AuthorizingServiceActions>();
    fake->authorized = Platform::ServiceActionResult::failed("No authentication agent is running to ask for the password");
    fake->authorized.authorizationUnavailable = true;
    ServiceActionsView view(fake);
    view.request(SvcDetail::makeRequest(ServiceActionKind::Start, service("cron.service", ServiceState::Stopped)));
    ASSERT_TRUE(waitFinished(view));
    view.retryAuthorized();
    ASSERT_TRUE(waitFinished(view));
    EXPECT_EQ(view.lastResult().text, "Could not start cron.service: No authentication agent is running to ask for the password");
    EXPECT_FALSE(view.lastResult().canAuthorize);

    view.request(SvcDetail::makeRequest(ServiceActionKind::Start, service("cron.service", ServiceState::Stopped)));
    ASSERT_TRUE(waitFinished(view));
    EXPECT_EQ(view.lastResult().text, "Could not start cron.service: Requires administrator");
    EXPECT_FALSE(view.lastResult().canAuthorize) << "no agent: not offered again";
}

TEST(ServiceActionsViewTest, CancelWhileIdleDoesNotCarryOverToTheNextAction)
{
    const auto fake = std::make_shared<FakeServiceActions>();
    ServiceActionsView view(fake);
    view.cancel(); // nothing running: a no-op
    EXPECT_FALSE(view.busy());
    view.request(SvcDetail::makeRequest(ServiceActionKind::Start, service("Spooler", ServiceState::Stopped)));
    ASSERT_TRUE(waitFinished(view));
    EXPECT_EQ(fake->starts.load(), 1);
    EXPECT_TRUE(view.lastResult().ok);
    EXPECT_EQ(view.lastResult().text, "Started Spooler");
}

TEST(ServiceActionsViewTest, ActionThatThrowsBecomesAPlainFailedResult)
{
    ServiceActionsView view(std::make_shared<ThrowingServiceActions>());
    view.request(SvcDetail::makeRequest(ServiceActionKind::Start, service("Spooler", ServiceState::Stopped)));
    ASSERT_TRUE(waitFinished(view));
    EXPECT_FALSE(view.lastResult().ok);
    EXPECT_EQ(view.lastResult().text, "Could not start Spooler: boom");
}

TEST(ServiceActionsViewTest, AStartTypeChangeThatThrowsAnythingElseIsAnUnknownError)
{
    // Not a std::exception: the worker still turns it into a failed result, worded for the start type.
    struct ThrowsAnything final : Platform::IServiceActions
    {
        [[nodiscard]] Platform::ServiceActionCapabilities capabilities() const override
        {
            return ALL;
        }
        [[nodiscard]] Platform::ServiceActionResult start(std::string_view /*name*/, const std::stop_token& /*stopToken*/) override
        {
            return Platform::ServiceActionResult::succeeded();
        }
        [[nodiscard]] Platform::ServiceActionResult stop(std::string_view /*name*/, const std::stop_token& /*stopToken*/) override
        {
            return Platform::ServiceActionResult::succeeded();
        }
        [[nodiscard]] Platform::ServiceActionResult restart(std::string_view /*name*/, const std::stop_token& /*stopToken*/) override
        {
            return Platform::ServiceActionResult::succeeded();
        }
        [[nodiscard]] Platform::ServiceActionResult setStartType(std::string_view /*name*/, Platform::ServiceStartType /*type*/) override
        {
            throw 42; // NOLINT(hicpp-exception-baseclass) - not a std::exception, on purpose
        }
    };
    ServiceActionsView view(std::make_shared<ThrowsAnything>());
    view.request(SvcDetail::makeRequest(
        ServiceActionKind::SetStartType, service("Spooler", ServiceState::Running), Platform::ServiceStartType::Automatic));
    ASSERT_TRUE(waitFinished(view));
    EXPECT_FALSE(view.lastResult().ok);
    EXPECT_EQ(view.lastResult().text, "Could not set Spooler to Automatic: Unknown error");
}

TEST(ServiceActionsDetailTest, AStartConfirmationNamesTheService)
{
    // Start doesn't ask by default, but its texts exist for a caller that asks first.
    const auto start = SvcDetail::makeRequest(ServiceActionKind::Start, service("Spooler", ServiceState::Stopped));
    EXPECT_EQ(SvcDetail::confirmTitle(start), "Start Spooler?");
    EXPECT_TRUE(SvcDetail::confirmQuestion(start).starts_with("Start Spooler service (Spooler)?")) << SvcDetail::confirmQuestion(start);
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

    /// One frame of the action bar above the table for @p selected, the result line and the confirm,
    /// as ServicesView draws them. Returns the bar's and the result line's text.
    static std::string barFrame(ServiceActionsView& view, const Platform::ServiceInfo* selected)
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
        ImGui::SetNextWindowSize(ImVec2(1200.0F, 800.0F));
        ImGui::Begin("Services", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
        ImGui::LogToBuffer();
        view.renderActionBar(selected);
        view.renderResultLine();
        std::string captured = GImGui->LogBuffer.c_str();
        ImGui::LogFinish();
        view.renderConfirmation();
        ImGui::End();
        ImGui::Render();
        return captured;
    }

    /// Clicks the action bar's button labelled @p label (a disabled one does nothing).
    static void clickBarButton(ServiceActionsView& view, const Platform::ServiceInfo* selected, const char* label)
    {
        static_cast<void>(barFrame(view, selected));
        const ImGuiWindow* window = ImGui::FindWindowByName("Services");
        ASSERT_NE(window, nullptr);
        ImGui::ActivateItemByID(ImHashStr(label, 0, window->ID));
        static_cast<void>(barFrame(view, selected));
        static_cast<void>(barFrame(view, selected));
    }

    /// Picks the item labelled @p label in the innermost open popup, drawing the bar for @p selected.
    static void pickBarPopupItem(ServiceActionsView& view, const Platform::ServiceInfo* selected, const char* label)
    {
        const ImGuiContext& g = *ImGui::GetCurrentContext();
        ASSERT_FALSE(g.OpenPopupStack.empty());
        ImGui::ActivateItemByID(ImHashStr(label, 0, g.OpenPopupStack.back().Window->ID));
        static_cast<void>(barFrame(view, selected));
        static_cast<void>(barFrame(view, selected));
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
    EXPECT_TRUE(SvcDetail::confirmQuestion(view.pendingConfirm().value_or(ServiceActionRequest{})).contains("RpcSs is critical to "));
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

// #1891: Restart, confirmed, runs on the worker and says so; its confirm button is labelled for it.
TEST_F(ServiceActionsViewRenderTest, ARestartConfirmedRunsAndSaysItRestarted)
{
    auto fake = std::make_shared<FakeServiceActions>();
    ServiceActionsView view(fake);
    const auto running = service("Spooler", ServiceState::Running);
    view.request(SvcDetail::makeRequest(ServiceActionKind::Restart, running));
    for (int i = 0; i < 3; ++i)
    {
        frame(view, running);
    }
    const ImGuiWindow* modal = ImGui::GetTopMostPopupModal();
    ASSERT_NE(modal, nullptr);
    EXPECT_TRUE(std::string(modal->Name).starts_with("Restart Spooler?")) << modal->Name;
    // Every service confirm is danger-filled: its button's ID is "##filled" under the label.
    ImGui::ActivateItemByID(
        ImHashStr("##filled", 0, ImHashStr(SvcDetail::confirmLabel(view.pendingConfirm().value_or(ServiceActionRequest{})), 0, modal->ID)));
    frame(view, running);
    frame(view, running);
    ASSERT_TRUE(waitFinished(view));
    EXPECT_TRUE(view.lastResult().ok);
    EXPECT_EQ(view.lastResult().text, "Restarted Spooler");
}

TEST_F(ServiceActionsViewRenderTest, ACancelledConfirmRunsNothing)
{
    auto fake = std::make_shared<FakeServiceActions>();
    ServiceActionsView view(fake);
    const auto running = service("Spooler", ServiceState::Running);
    view.request(SvcDetail::makeRequest(ServiceActionKind::Stop, running));
    for (int i = 0; i < 3; ++i)
    {
        frame(view, running);
    }
    const ImGuiWindow* modal = ImGui::GetTopMostPopupModal();
    ASSERT_NE(modal, nullptr);
    ImGui::ActivateItemByID(ImHashStr("Cancel", 0, modal->ID));
    frame(view, running);
    frame(view, running);
    EXPECT_FALSE(view.pendingConfirm().has_value());
    EXPECT_FALSE(view.takeFinished());
    EXPECT_EQ(fake->stops.load(), 0);
}

// The action bar above the table (#1577, #1395 Windows half): what it says beside its buttons.
TEST_F(ServiceActionsViewRenderTest, ActionBarSaysWhatTheButtonsActOn)
{
    const auto spooler = service("Spooler", ServiceState::Running);

    ServiceActionsView limited(std::make_shared<FakeServiceActions>());
    EXPECT_TRUE(barFrame(limited, &spooler).contains("Requires administrator for most services"));

    auto elevatedCaps = ALL;
    elevatedCaps.elevated = true;
    ServiceActionsView elevated(std::make_shared<FakeServiceActions>(elevatedCaps));
    const std::string nothingSelected = barFrame(elevated, nullptr);
    EXPECT_TRUE(nothingSelected.contains("Select a service"));
    EXPECT_FALSE(nothingSelected.contains("Requires administrator"));
    EXPECT_TRUE(barFrame(elevated, &spooler).contains("Spooler"));

    // A platform with no actions draws no bar at all.
    ServiceActionsView unsupported(std::make_shared<Platform::UnsupportedServiceActions>());
    EXPECT_TRUE(barFrame(unsupported, &spooler).empty());
}

TEST_F(ServiceActionsViewRenderTest, ActionBarButtonsFollowTheSelectedServicesState)
{
    const auto fake = std::make_shared<FakeServiceActions>();
    ServiceActionsView view(fake);
    const auto running = service("Spooler", ServiceState::Running);

    clickBarButton(view, nullptr, ICON_FA_STOP " Stop"); // nothing selected: disabled
    EXPECT_FALSE(view.confirmRequested());
    clickBarButton(view, &running, ICON_FA_PLAY " Start"); // running: disabled
    EXPECT_FALSE(view.confirmRequested());
    EXPECT_FALSE(view.busy());

    clickBarButton(view, &running, ICON_FA_ARROWS_ROTATE " Restart"); // asks first
    ASSERT_TRUE(view.confirmRequested());
    EXPECT_EQ(view.pendingConfirm().value_or(ServiceActionRequest{}).kind, ServiceActionKind::Restart);
    EXPECT_EQ(fake->starts.load() + fake->stops.load(), 0);

    // A stopped service starts from the bar straight away and reports it.
    ServiceActionsView startView(fake);
    const auto stopped = service("Spooler", ServiceState::Stopped);
    clickBarButton(startView, &stopped, ICON_FA_PLAY " Start");
    EXPECT_FALSE(startView.confirmRequested());
    ASSERT_TRUE(finishWithoutSleeping(startView));
    EXPECT_EQ(fake->starts.load(), 1);
    EXPECT_TRUE(barFrame(startView, &stopped).contains("Started Spooler"));
}

TEST_F(ServiceActionsViewRenderTest, StartupTypeMenuOffersTheOtherTypes)
{
    const auto fake = std::make_shared<FakeServiceActions>();
    const auto spooler = service("Spooler", ServiceState::Running); // Manual
    constexpr const char* MENU_BUTTON = "Startup type " ICON_FA_CARET_DOWN;

    {
        ServiceActionsView view(fake);
        clickBarButton(view, &spooler, MENU_BUTTON);
        pickBarPopupItem(view, &spooler, "Manual"); // its current type: disabled
        EXPECT_FALSE(view.confirmRequested());
        EXPECT_FALSE(view.busy());
    }
    {
        // Disabled asks first.
        ServiceActionsView view(fake);
        clickBarButton(view, &spooler, MENU_BUTTON);
        pickBarPopupItem(view, &spooler, "Disabled");
        ASSERT_TRUE(view.confirmRequested());
        const ServiceActionRequest pending = view.pendingConfirm().value_or(ServiceActionRequest{});
        EXPECT_EQ(pending.kind, ServiceActionKind::SetStartType);
        EXPECT_EQ(pending.startType, Platform::ServiceStartType::Disabled);
    }
    {
        // Another automatic type runs straight away.
        ServiceActionsView view(fake);
        clickBarButton(view, &spooler, MENU_BUTTON);
        pickBarPopupItem(view, &spooler, "Automatic (delayed)");
        EXPECT_FALSE(view.confirmRequested());
        ASSERT_TRUE(finishWithoutSleeping(view));
        EXPECT_EQ(fake->startTypes.load(), 1);
        EXPECT_EQ(fake->lastStartType.load(), Platform::ServiceStartType::AutomaticDelayed);
        EXPECT_EQ(view.lastResult().text, "Set Spooler to Automatic (delayed)");
    }
    EXPECT_EQ(fake->startTypes.load(), 1);
}

// #2006: where the platform has no delayed automatic start (systemd), the menu leaves it out.
TEST_F(ServiceActionsViewRenderTest, StartupTypeMenuLeavesOutADelayedStartThePlatformHasNot)
{
    Platform::ServiceActionCapabilities caps = ALL;
    caps.canDelayStart = false;
    const auto fake = std::make_shared<FakeServiceActions>(caps);
    const auto spooler = service("Spooler", ServiceState::Running);
    ServiceActionsView view(fake);
    clickBarButton(view, &spooler, "Startup type " ICON_FA_CARET_DOWN);
    const std::string text = barFrame(view, &spooler);
    EXPECT_TRUE(text.contains("Disabled")) << text; // the menu is open
    EXPECT_FALSE(text.contains("Automatic (delayed)")) << text;
}

TEST_F(ServiceActionsViewRenderTest, RowMenuStartupTypeSubmenuAsksBeforeDisabling)
{
    const auto fake = std::make_shared<FakeServiceActions>();
    ServiceActionsView view(fake);
    const auto spooler = service("Spooler", ServiceState::Running);

    pickMenuItem(view, spooler, "Startup type"); // opens the submenu
    const ImGuiContext& g = *ImGui::GetCurrentContext();
    ASSERT_GE(g.OpenPopupStack.Size, 2) << "the row menu and its Startup type submenu";
    ImGui::ActivateItemByID(ImHashStr("Disabled", 0, g.OpenPopupStack.back().Window->ID));
    frame(view, spooler);
    frame(view, spooler);

    ASSERT_TRUE(view.confirmRequested());
    EXPECT_EQ(view.pendingConfirm().value_or(ServiceActionRequest{}).startType, Platform::ServiceStartType::Disabled);
    EXPECT_EQ(fake->startTypes.load(), 0);
}

TEST_F(ServiceActionsViewRenderTest, RunningActionShowsProgressAndDisablesTheBar)
{
    const auto fake = std::make_shared<GatedServiceActions>();
    ServiceActionsView view(fake);
    const auto stopped = service("Spooler", ServiceState::Stopped);
    view.request(SvcDetail::makeRequest(ServiceActionKind::Start, stopped));
    fake->entered.wait();

    EXPECT_TRUE(barFrame(view, &stopped).contains("Starting Spooler..."));
    clickBarButton(view, &stopped, ICON_FA_PLAY " Start"); // busy: disabled, no second action
    EXPECT_FALSE(view.confirmRequested());

    view.cancel();
    // The destructor waits for the cancelled worker; the gate returns once its token is stopped.
}

} // namespace
} // namespace App
