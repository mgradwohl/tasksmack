/// @file test_ProcessActionsBlockRender.cpp
/// @brief The Overview's Actions and Priority cards (#1493, #1789), headless: the Actions card holds the
/// buttons at their labels' width, the Priority card the priority row(s), each as wide as its content.
/// On the Identity/Runtime row they share its height (the tallest card's); wrapped below it they sit
/// side by side at the taller one's height, or one under the other at their own, held to a narrow
/// pane; and the shared confirm dialog still opens for them, acting on nothing until confirmed.
/// Identity, Runtime, Actions and Priority are cards styled as a CPU Cores grid cell is, sharing their
/// edges beside one another (#1537).

#include "App/Panels/ProcessActionsBlock.h"
#include "App/Panels/ProcessActionsView.h"
#include "App/Panels/ProcessAffinityDialog.h"
#include "App/Panels/ProcessDetailsLayout.h"
#include "App/Panels/ProcessDetailsPanel_ActionHelpers.h"
#include "App/Panels/ProcessDumpView.h"
#include "App/Panels/ProcessOverviewCard.h"
#include "App/Panels/ProcessPriorityView.h"
#include "Core/Application.h"
#include "Mocks/MockProbes.h"
#include "Platform/CpuAffinity.h"
#include "Platform/IProcessActions.h"
#include "UI/Card.h"
#include "UI/ChartGrid.h"
#include "UI/ChartGridLayout.h"
#include "UI/IconsFontAwesome6.h"

#ifdef _WIN32
#include "App/Panels/AuthorizeWording.h"
#include "App/Panels/ProcessDetailsPanel_PriorityHelpers.h" // The Windows class slider: its ID and minimum width
#include "Core/HeadlessVideoDriverTestUtils.h"              // The folder dialog's owner: a display for the main window
#include "Core/Window.h"
#include "Mocks/FakeBroker.h"
#include "Platform/BrokerProtocol.h"
#include "Platform/BrokeredProcessActions.h"
#include "Platform/Windows/WindowsProcessActions.h" // A Kill Windows refuses for want of access (#2602)

#include <SDL3/SDL_properties.h>
#include <SDL3/SDL_video.h>

#include <memory>
#include <vector>
#endif

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h> // The cards' child windows, for their size, and the open modal

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace App
{
namespace
{

constexpr Platform::ProcessTarget TARGET{.pid = 1001, .startTimeTicks = 5000};

constexpr Platform::ProcessActionCapabilities ALL_ACTIONS{
    .canTerminate = true, .canKill = true, .canStop = true, .canContinue = true, .canSetPriority = true};
constexpr Platform::ProcessActionCapabilities NO_PRIORITY{
    .canTerminate = true, .canKill = true, .canStop = false, .canContinue = false, .canSetPriority = false};

constexpr float INFO_ROW = 776.0F; // Identity and Runtime at their 384px cap, 8px apart

/// The child window whose name contains @p id, as the last frame left it.
[[nodiscard]] const ImGuiWindow* childWindow(std::string_view id)
{
    for (const ImGuiWindow* window : ImGui::GetCurrentContext()->Windows)
    {
        if ((window->Flags & ImGuiWindowFlags_ChildWindow) != 0 && std::string_view{window->Name}.contains(id))
        {
            return window;
        }
    }
    return nullptr;
}

/// The Actions card's child window, as the last frame left it.
[[nodiscard]] const ImGuiWindow* actionsWindow()
{
    return childWindow("ProcessActionsBlock_");
}

/// The Priority card's child window, as the last frame left it.
[[nodiscard]] const ImGuiWindow* priorityWindow()
{
    return childWindow("ProcessPriorityBlock_");
}

/// The Identity/Runtime card height for @p rows rows, worked out as ProcessDetailsPanel does: the
/// header line and the rows, inside the card's padding (#1537).
[[nodiscard]] float infoCardHeight(float rows)
{
    return ProcessDetailsLayout::computeInfoCardHeight(rows, ImGui::GetTextLineHeightWithSpacing(), ImGui::GetStyle().WindowPadding.y);
}

/// The height ProcessDetailsPanel hands the Actions and Priority cards for @p layout: the top row's
/// (the tallest of Identity/Runtime's @p infoHeight and the cards' @p last heights), the wrapped side
/// by side row's (the taller card's), or 0 (each its own) stacked.
[[nodiscard]] float controlsHeightFor(const ProcessDetailsLayout::TopRowLayout& layout, float infoHeight, ProcessActionsBlock::Heights last)
{
    const float actions = layout.actionsWidth > 0.0F ? last.actions : 0.0F;
    const float priority = layout.priorityWidth > 0.0F ? last.priority : 0.0F;
    if (layout.besideInfo)
    {
        return ProcessDetailsLayout::computeRowCardHeight(infoHeight, actions, priority);
    }
    return layout.priorityBesideActions ? ProcessDetailsLayout::computeRowCardHeight(0.0F, actions, priority) : 0.0F;
}

/// The layout ProcessDetailsPanel picks for @p capabilities in @p paneWidth beside a @p infoRowWidth row.
[[nodiscard]] ProcessDetailsLayout::TopRowLayout
layoutFor(const Platform::ProcessActionCapabilities& capabilities, float paneWidth, float infoRowWidth = INFO_ROW)
{
    const ProcessActionsBlock::Widths widths = ProcessActionsBlock::measure(capabilities);
    return ProcessDetailsLayout::chooseTopRowLayout(
        paneWidth, infoRowWidth, ImGui::GetStyle().ItemSpacing.x, widths.actionsCard(), widths.priorityCard());
}

class ProcessActionsBlockRenderTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Context = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.DisplaySize = ImVec2(1600.0F, 1000.0F);
        io.DeltaTime = 1.0F / 60.0F;
        // No renderer: let ImGui build and own the font atlas itself.
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        io.Fonts->AddFontDefault();
    }

    void TearDown() override
    {
        ImGui::DestroyContext(m_Context);
    }

    /// One frame of a window like the Process Details pane, running @p body inside it; returns whether
    /// any popup (the confirm modal) is open at the end of it.
    static bool runFrame(const std::function<void()>& body)
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
        ImGui::SetNextWindowSize(ImVec2(1600.0F, 1000.0F));
        ImGui::Begin("Details", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
        body();
        const bool open = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);
        ImGui::End();
        ImGui::Render();
        return open;
    }

    struct Harness
    {
        TestMocks::MockProcessActions mock;
        ProcessActionsView actionsView;
        ProcessPriorityView priorityView;
        std::string name = "victim";

        [[nodiscard]] ProcessActionsBlock::Context context(const Platform::ProcessActionCapabilities& capabilities)
        {
            return ProcessActionsBlock::Context{
                .actionsView = &actionsView,
                .priorityView = &priorityView,
                .actions = &mock,
                .capabilities = capabilities,
                .processName = &name,
                .target = TARGET,
                .currentNice = std::optional<std::int32_t>{0},
            };
        }
    };

    /// Draws the cards for @p capabilities, laid out as the panel lays them out in @p paneWidth beside
    /// an Identity/Runtime row @p infoRowWidth wide whose cards hold @p infoRows rows, with the heights
    /// the cards reported the frame before; three frames, so the heights and any auto-resizing card
    /// have settled. The height the cards were handed last is left in lastCardHeight.
    static ProcessDetailsLayout::TopRowLayout
    renderBlock(Harness& h, const Platform::ProcessActionCapabilities& capabilities, float paneWidth, float infoRowWidth, float infoRows)
    {
        ProcessDetailsLayout::TopRowLayout layout;
        lastHeights = {};
        const auto body = [&]
        {
            layout = layoutFor(capabilities, paneWidth, infoRowWidth);
            lastCardHeight = controlsHeightFor(layout, infoCardHeight(infoRows), lastHeights);
            lastHeights = ProcessActionsBlock::render(h.context(capabilities), layout, lastCardHeight);
        };
        for (int frame = 0; frame < 3; ++frame)
        {
            (void) runFrame(body);
        }
        return layout;
    }

    /// What the last renderBlock() frame's render() reported the cards need down.
    static inline ProcessActionsBlock::Heights lastHeights{};
    /// The height the last renderBlock() frame handed the cards (0: each its own).
    static inline float lastCardHeight = 0.0F;

  private:
    ImGuiContext* m_Context = nullptr;
};

TEST_F(ProcessActionsBlockRenderTest, TheButtonsAreAsWideAsTheirLabelsNotAShareOfTheCard)
{
    // The maintainer's #1511 review: Terminate and Kill were stretched to half the block each. They are
    // now equal at the wider label's natural width, side by side with the normal spacing.
    float buttons = 0.0F;
    float expected = 0.0F;
    float withoutPriority = 0.0F;
    float withoutPriorityCard = -1.0F;
    float withIoPriorityOnly = 0.0F;
    (void) runFrame(
        [&]
        {
            buttons = ProcessActionsBlock::measure(NO_PRIORITY).buttons;
            const float widest = std::max(ImGui::CalcTextSize(Detail::ACTION_BUTTONS.at(0).label).x,
                                          ImGui::CalcTextSize(Detail::ACTION_BUTTONS.at(1).label).x);
            expected = ProcessDetailsLayout::computeActionButtonRowWidth(
                ProcessDetailsLayout::computeActionButtonWidth(widest, ImGui::GetStyle().FramePadding.x),
                2,
                ImGui::GetStyle().ItemSpacing.x);
            withoutPriority = ProcessActionsBlock::measure(NO_PRIORITY).priority;
            withoutPriorityCard = ProcessActionsBlock::measure(NO_PRIORITY).priorityCard();
            Platform::ProcessActionCapabilities ioOnly = NO_PRIORITY;
            ioOnly.canSetIoPriority = true;
            withIoPriorityOnly = ProcessActionsBlock::measure(ioOnly).priority;
        });
    EXPECT_FLOAT_EQ(buttons, expected);
    EXPECT_FLOAT_EQ(withoutPriority, 0.0F);
    EXPECT_FLOAT_EQ(withoutPriorityCard, 0.0F); // No priority control: no Priority card, and no gap for it
    // ProcessPriorityView also draws the I/O priority row on its own (#803), so it is measured.
    EXPECT_GT(withIoPriorityOnly, 0.0F);
}

TEST_F(ProcessActionsBlockRenderTest, BesideTheRowBothCardsAreTheRowsHeightAndTheirContentsWidth)
{
    // The issue (#1789): a pane wide enough for all four cards puts Actions and Priority on the
    // Identity/Runtime row, Priority to the right of Actions, at the row's height.
    Harness h;
    const auto layout = renderBlock(h, ALL_ACTIONS, 1900.0F, INFO_ROW, 6.0F);
    ASSERT_TRUE(layout.besideInfo);
    ASSERT_TRUE(layout.priorityBesideActions);

    const ImGuiWindow* actions = actionsWindow();
    const ImGuiWindow* priority = priorityWindow();
    ASSERT_NE(actions, nullptr);
    ASSERT_NE(priority, nullptr);
    // The row is at least Identity/Runtime's height, and as tall as the taller card when that is more.
    EXPECT_GE(lastCardHeight, infoCardHeight(6.0F));
    EXPECT_FLOAT_EQ(lastCardHeight, std::max({infoCardHeight(6.0F), lastHeights.actions, lastHeights.priority}));
    for (const ImGuiWindow* card : {actions, priority})
    {
        EXPECT_FLOAT_EQ(card->Size.y, lastCardHeight);
        EXPECT_FLOAT_EQ(card->ScrollMax.y, 0.0F);
        EXPECT_FLOAT_EQ(card->Pos.y, actions->Pos.y);
    }
    EXPECT_NEAR(actions->Size.x, layout.actionsWidth, 1.0F); // ImGui rounds a window's size to whole pixels
    EXPECT_NEAR(priority->Size.x, layout.priorityWidth, 1.0F);
    EXPECT_GT(priority->Pos.x, actions->Pos.x + actions->Size.x);
    // Nothing in either is wider than the card allows: no stretched buttons, no row running out of it.
    EXPECT_LE(actions->ContentSize.x, layout.actionsWidth - (2.0F * ImGui::GetStyle().WindowPadding.x) + 1.0F);
    EXPECT_LE(priority->ContentSize.x, layout.priorityWidth - (2.0F * ImGui::GetStyle().WindowPadding.x) + 1.0F);
}

#ifdef _WIN32
TEST_F(ProcessActionsBlockRenderTest, OnWindowsThePriorityCardDrawsTheClassSliderWhole)
{
    // Windows' actions: [Terminate] [Kill] in the Actions card, and the priority slider with a stop per
    // class (#1538), not a combo, in the Priority card. The card is measured wide enough for the
    // slider's five stop names, and wrapped below the row it is shown whole: nothing to scroll.
    constexpr Platform::ProcessActionCapabilities WINDOWS_ACTIONS{
        .canTerminate = true, .canKill = true, .canStop = false, .canContinue = false, .canSetPriority = true};
    Harness h;
    const auto layout = renderBlock(h, WINDOWS_ACTIONS, 1000.0F, INFO_ROW, 0.0F);
    ASSERT_FALSE(layout.besideInfo);
    float priorityWidth = 0.0F;
    float sliderMinWidth = 0.0F;
    (void) runFrame(
        [&]
        {
            priorityWidth = ProcessActionsBlock::measure(WINDOWS_ACTIONS).priority;
            sliderMinWidth = Detail::discretePrioritySliderMinWidth(Detail::WINDOWS_PRIORITY_SLIDER);
        });
    EXPECT_GE(priorityWidth, sliderMinWidth);
    const ImGuiWindow* card = priorityWindow();
    ASSERT_NE(card, nullptr);
    EXPECT_FLOAT_EQ(card->ScrollMax.x, 0.0F);
    EXPECT_FLOAT_EQ(card->ScrollMax.y, 0.0F);

    // The slider is there, under its own ID, and its keys pick the next class down from Normal.
    ImGuiWindow* cardForFocus = ImGui::FindWindowByID(card->ID);
    const ImGuiID sliderId = ImHashStr(Detail::WINDOWS_PRIORITY_SLIDER_ID, 0, card->ID);
    bool focus = true;
    const auto body = [&]
    {
        if (focus)
        {
            ImGui::SetFocusID(sliderId, cardForFocus);
            focus = false;
        }
        (void) ProcessActionsBlock::render(h.context(WINDOWS_ACTIONS), layoutFor(WINDOWS_ACTIONS, 1000.0F), 0.0F);
    };
    (void) runFrame(body);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_RightArrow, true);
    (void) runFrame(body);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_RightArrow, false);
    (void) runFrame(body);
    EXPECT_EQ(h.priorityView.niceValue(), Detail::windowsPriorityClassNice(Detail::WindowsPriorityClass::BelowNormal));
    EXPECT_TRUE(h.priorityView.hasPendingEdit());
    EXPECT_EQ(h.mock.setPriorityCount(), 0);
}
#endif

TEST_F(ProcessActionsBlockRenderTest, WrappedBelowTheRowTheCardsShareTheTallerContentHeight)
{
    Harness h;
    // A pane too narrow for two more cards beside a 776px row, but wide enough for the two side by side.
    const auto layout = renderBlock(h, ALL_ACTIONS, 1000.0F, INFO_ROW, 0.0F);
    ASSERT_FALSE(layout.besideInfo);
    ASSERT_TRUE(layout.priorityBesideActions);

    const ImGuiWindow* actions = actionsWindow();
    const ImGuiWindow* priority = priorityWindow();
    ASSERT_NE(actions, nullptr);
    ASSERT_NE(priority, nullptr);
    // Not the empty row's height: as tall as the taller card's content, both of them, nothing to scroll.
    EXPECT_GT(actions->Size.y, infoCardHeight(0.0F));
    EXPECT_FLOAT_EQ(lastCardHeight, std::max(lastHeights.actions, lastHeights.priority));
    for (const ImGuiWindow* card : {actions, priority})
    {
        EXPECT_FLOAT_EQ(card->Size.y, lastCardHeight);
        EXPECT_FLOAT_EQ(card->ScrollMax.y, 0.0F);
        EXPECT_FLOAT_EQ(card->Pos.y, actions->Pos.y);
    }
    // Still their contents' widths, left-aligned, not the pane's.
    EXPECT_LT(actions->Size.x + priority->Size.x, 1000.0F);
}

TEST_F(ProcessActionsBlockRenderTest, InANarrowPaneTheCardsStackHeldToItAndTheButtonsWrap)
{
    Harness h;
    const auto layout = renderBlock(h, ALL_ACTIONS, 120.0F, INFO_ROW, 0.0F);
    ASSERT_FALSE(layout.besideInfo);
    ASSERT_FALSE(layout.priorityBesideActions);

    const ImGuiWindow* actions = actionsWindow();
    const ImGuiWindow* priority = priorityWindow();
    ASSERT_NE(actions, nullptr);
    ASSERT_NE(priority, nullptr);
    EXPECT_LE(actions->Size.x, 120.0F);
    EXPECT_LE(priority->Size.x, 120.0F);
    // One under the other, never overlapping.
    EXPECT_GE(priority->Pos.y, actions->Pos.y + actions->Size.y);
    EXPECT_FLOAT_EQ(priority->Pos.x, actions->Pos.x);
    // Kill went to a row of its own rather than being clipped off the right edge: under the card's
    // header line (#1537), more than one framed row.
    EXPECT_FLOAT_EQ(actions->ScrollMax.x, 0.0F);
    EXPECT_GT(actions->ContentSize.y, ImGui::GetTextLineHeightWithSpacing() + ImGui::GetFrameHeightWithSpacing());
}

TEST_F(ProcessActionsBlockRenderTest, KillFromTheCardOpensTheSharedConfirmAndActsOnNothingYet)
{
    Harness h;
    ASSERT_TRUE(h.actionsView.requestKillShortcut(ALL_ACTIONS, TARGET, h.name));

    std::string modalName;
    const auto body = [&]
    {
        (void) ProcessActionsBlock::render(h.context(ALL_ACTIONS), layoutFor(ALL_ACTIONS, 1600.0F, 600.0F), infoCardHeight(6.0F));
        // As ProcessDetailsPanel does: the dialog from panel scope, every frame.
        h.actionsView.renderConfirmation(&h.mock, TARGET);
        if (const ImGuiWindow* modal = ImGui::GetTopMostPopupModal(); modal != nullptr)
        {
            modalName = modal->Name;
        }
    };
    EXPECT_TRUE(runFrame(body));
    EXPECT_TRUE(runFrame(body));
    EXPECT_TRUE(modalName.starts_with("Kill")) << modalName;
    EXPECT_EQ(h.actionsView.pendingAction(), Detail::ProcessAction::Kill);
    EXPECT_EQ(h.mock.killCount(), 0);

    // A selection change still closes it unconfirmed.
    h.actionsView.onSelectionChanged();
    EXPECT_FALSE(runFrame(body));
    EXPECT_EQ(h.mock.killCount(), 0);
}

#ifdef _WIN32
/// OpenProcess refusing every open as Windows refuses an unelevated TaskSmack an elevated process.
HANDLE WINAPI refuseEveryOpen(DWORD /*access*/, BOOL /*inherit*/, DWORD /*pid*/)
{
    SetLastError(ERROR_ACCESS_DENIED);
    return nullptr;
}

// #2602: a Kill Windows refuses for want of access, through the real WindowsProcessActions and the
// helper's decorator with no helper running, shows its error with "Retry as administrator" beside it,
// and pressing that kills through the helper. Nothing real is opened, let alone killed.
TEST_F(ProcessActionsBlockRenderTest, AKillWindowsRefusesForWantOfAccessOffersRetryAsAdministrator)
{
    Platform::WindowsProcessFunctions functions{};
    functions.openProcess = &refuseEveryOpen;
    const auto broker = std::make_shared<Mocks::FakeBroker>(/*connected=*/false);
    Platform::BrokeredProcessActions actions(std::make_unique<Platform::WindowsProcessActions>(functions), broker);

    ProcessActionsView view;
    view.requestAction(Detail::ProcessAction::Kill, TARGET, "elevated.exe");
    view.dispatchConfirmed(&actions);
    view.waitForActionForTesting();

    std::string text;
    const auto body = [&]
    {
        ImGui::LogToBuffer();
        view.renderResultLine();
        text = GImGui->LogBuffer.c_str();
        ImGui::LogFinish();
    };
    (void) runFrame(body);
    EXPECT_TRUE(text.contains("Could not kill PID 1001: Access denied: can't end process 1001. It may run elevated or as another user."))
        << text;
    EXPECT_TRUE(text.contains("Retry as administrator")) << text;
    ASSERT_TRUE(view.offersAuthenticateAndRetry());
    EXPECT_TRUE(broker->requests().empty()) << "no helper running: nothing is asked until the button is pressed";

    // The button starts the helper (a UAC prompt; here the fake) and kills through it.
    const ImGuiWindow* details = ImGui::FindWindowByName("Details");
    ASSERT_NE(details, nullptr);
    ImGui::ActivateItemByID(ImHashStr(AuthorizeWording::RETRY_LABEL, 0, details->ID));
    for (int frame = 0; frame < 3 && broker->connectCount() == 0; ++frame)
    {
        (void) runFrame(body);
        view.waitForActionForTesting(); // the retry runs on the action worker (#2305)
    }
    EXPECT_EQ(broker->connectCount(), 1);
    const std::vector<Platform::Broker::Request> requests = broker->requests();
    ASSERT_EQ(requests.size(), 1U);
    EXPECT_EQ(requests.front(),
              (Platform::Broker::Request{Platform::Broker::SignalRequest{.target = TARGET, .signal = Platform::Broker::Signal::Kill}}));
    EXPECT_TRUE(view.lastResult().ok) << view.lastResult().text;
}
#endif

// #1887: where the platform can set affinity, the Actions card has Set affinity...; it opens the affinity
// dialog with the process's CPUs ticked, Apply sets them on that process and the card's line says so, and
// a selection change closes the dialog unapplied.
TEST_F(ProcessActionsBlockRenderTest, SetAffinityFromTheCardOpensTheDialogAndAppliesToTheProcess)
{
    Harness h;
    Platform::ProcessActionCapabilities caps = ALL_ACTIONS;
    caps.canSetAffinity = true;
    const Platform::CpuAffinity current = Platform::CpuAffinity::fromMask(0b11U);
    std::string text;
    const auto body = [&]
    {
        ImGui::LogToBuffer();
        ProcessActionsBlock::Context context = h.context(caps);
        context.currentAffinity = &current;
        (void) ProcessActionsBlock::render(context, layoutFor(caps, 1600.0F, 600.0F), infoCardHeight(6.0F));
        h.actionsView.renderConfirmation(&h.mock, TARGET);
        text = GImGui->LogBuffer.c_str();
        ImGui::LogFinish();
    };
    ASSERT_FALSE(runFrame(body));
    EXPECT_TRUE(text.contains("Set affinity...")) << text;
    const ImGuiWindow* actions = actionsWindow();
    ASSERT_NE(actions, nullptr);
    ImGui::ActivateItemByID(ImHashStr(Detail::SET_AFFINITY_LABEL, 0, actions->ID));
    static_cast<void>(runFrame(body));
    ASSERT_TRUE(runFrame(body));
    ProcessAffinityDialog& dialog = h.actionsView.affinityDialog();
    EXPECT_EQ(dialog.target().pid, TARGET.pid);
    EXPECT_EQ(dialog.choice(), current);

    dialog.choose(2, true);
    const ImGuiWindow* modal = ImGui::GetTopMostPopupModal();
    ASSERT_NE(modal, nullptr);
    ImGui::ActivateItemByID(ImHashStr("##filled", 0, ImHashStr("Apply", 0, modal->ID)));
    for (int i = 0; i < 3 && h.mock.setAffinityCount() == 0; ++i)
    {
        static_cast<void>(runFrame(body));
        h.actionsView.waitForActionForTesting(); // the change runs on the action worker (#2305)
    }
    EXPECT_EQ(h.mock.setAffinityCount(), 1);
    EXPECT_EQ(h.mock.lastAffinity(), Platform::CpuAffinity::fromMask(0b111U));
    EXPECT_EQ(h.mock.lastTarget().pid, TARGET.pid);
    static_cast<void>(runFrame(body));
    EXPECT_TRUE(text.contains("CPU affinity of victim (PID 1001) set to 0-2")) << text;

    // Opened again, then the selection moves on: closed, nothing applied.
    ImGui::ActivateItemByID(ImHashStr(Detail::SET_AFFINITY_LABEL, 0, actionsWindow()->ID));
    static_cast<void>(runFrame(body));
    ASSERT_TRUE(runFrame(body));
    h.actionsView.onSelectionChanged();
    static_cast<void>(runFrame(body));
    EXPECT_FALSE(runFrame(body));
    EXPECT_EQ(h.mock.setAffinityCount(), 1);
}

// #1395: confirming the dialog acts on the process and the card shows the result in its line; cancelling it
// acts on nothing.
TEST_F(ProcessActionsBlockRenderTest, ConfirmingActsAndShowsTheResultAndCancellingDoesNot)
{
    Harness h;
    std::string text;
    const auto body = [&]
    {
        ImGui::LogToBuffer();
        (void) ProcessActionsBlock::render(h.context(ALL_ACTIONS), layoutFor(ALL_ACTIONS, 1600.0F, 600.0F), infoCardHeight(6.0F));
        h.actionsView.renderConfirmation(&h.mock, TARGET);
        text = GImGui->LogBuffer.c_str();
        ImGui::LogFinish();
    };

    // Cancelled: nothing is done.
    ASSERT_TRUE(h.actionsView.requestKillShortcut(ALL_ACTIONS, TARGET, h.name));
    ASSERT_TRUE(runFrame(body));
    ASSERT_TRUE(runFrame(body));
    const ImGuiWindow* modal = ImGui::GetTopMostPopupModal();
    ASSERT_NE(modal, nullptr);
    ImGui::ActivateItemByID(ImHashStr("Cancel", 0, modal->ID));
    (void) runFrame(body);
    EXPECT_FALSE(runFrame(body));
    EXPECT_EQ(h.mock.killCount(), 0);
    EXPECT_EQ(h.actionsView.pendingAction(), Detail::ProcessAction::None);

    // Confirmed: the process is killed and the result line is drawn in the card.
    ASSERT_TRUE(h.actionsView.requestKillShortcut(ALL_ACTIONS, TARGET, h.name));
    ASSERT_TRUE(runFrame(body));
    ASSERT_TRUE(runFrame(body));
    modal = ImGui::GetTopMostPopupModal();
    ASSERT_NE(modal, nullptr);
    ImGui::ActivateItemByID(ImHashStr("##filled", 0, ImHashStr("Kill", 0, modal->ID)));
    (void) runFrame(body);
    h.actionsView.waitForActionForTesting(); // the kill runs on the action worker (#2305)
    (void) runFrame(body);
    EXPECT_EQ(h.mock.killCount(), 1);
    EXPECT_FALSE(h.actionsView.lastResult().text.empty());
    EXPECT_TRUE(text.contains(h.actionsView.lastResult().text)) << text;
}

// #1395: each action button explains itself on hover; Kill names its key.
TEST_F(ProcessActionsBlockRenderTest, TheActionButtonsExplainThemselvesOnHover)
{
    Harness h;
    std::string seen;
    const auto body = [&]
    {
        ImGui::LogToBuffer();
        (void) ProcessActionsBlock::render(h.context(ALL_ACTIONS), layoutFor(ALL_ACTIONS, 1600.0F, 600.0F), infoCardHeight(6.0F));
        seen += GImGui->LogBuffer.c_str();
        ImGui::LogFinish();
    };
    (void) runFrame(body);
    (void) runFrame(body);
    const ImGuiWindow* card = actionsWindow();
    ASSERT_NE(card, nullptr);
    const ImVec2 origin = card->Pos;
    const ImVec2 size = card->Size;
    seen.clear();
    for (int row = 0; row < 24; ++row)
    {
        for (int column = 0; column < 24; ++column)
        {
            ImGui::GetIO().AddMousePosEvent(origin.x + (size.x * (static_cast<float>(column) + 0.5F) / 24.0F),
                                            origin.y + (size.y * (static_cast<float>(row) + 0.5F) / 24.0F));
            (void) runFrame(body);
        }
    }
    EXPECT_TRUE(seen.contains("(F9)")) << seen; // Kill's tooltip names its key
}

TEST_F(ProcessActionsBlockRenderTest, F9OpensTheKillConfirmWhileTheCardIsScrolledOutOfView)
{
    // The Overview scrolled so the Actions card is out of view: ImGui skips the card's child, and
    // everything in it. The confirm dialog is submitted from outside it, so F9's Kill confirm still
    // opens (Copilot review on #1511) instead of staying pending with no dialog and refusing every
    // later F9.
    Harness h;
    bool cardSkipped = false;
    std::string modalName;
    const auto body = [&]
    {
        // A short scrolling Overview with the cards far below its visible part.
        ImGui::BeginChild("##OverviewContent", ImVec2(1000.0F, 100.0F));
        ImGui::Dummy(ImVec2(10.0F, 2000.0F));
        (void) ProcessActionsBlock::render(h.context(ALL_ACTIONS), layoutFor(ALL_ACTIONS, 1000.0F, 0.0F), infoCardHeight(6.0F));
        const ImGuiWindow* card = actionsWindow();
        cardSkipped = card == nullptr || card->SkipItems;
        ImGui::EndChild();
        h.actionsView.renderConfirmation(&h.mock, TARGET);
        if (const ImGuiWindow* modal = ImGui::GetTopMostPopupModal(); modal != nullptr)
        {
            modalName = modal->Name;
        }
    };
    (void) runFrame(body); // Lays the Overview out, so the next frames know the card is clipped
    ASSERT_FALSE(runFrame(body));

    ASSERT_TRUE(h.actionsView.requestKillShortcut(ALL_ACTIONS, TARGET, h.name)); // F9
    EXPECT_TRUE(runFrame(body));
    EXPECT_TRUE(runFrame(body));
    EXPECT_TRUE(cardSkipped) << "the card was drawn, so this does not test the scrolled-out case";
    EXPECT_TRUE(modalName.starts_with("Kill")) << modalName;
    EXPECT_EQ(h.mock.killCount(), 0);
}

// --- Priority label and centred rows (#1874) -------------------------------------------------------

/// How many times @p needle occurs in @p text.
[[nodiscard]] std::size_t occurrences(std::string_view text, std::string_view needle)
{
    std::size_t count = 0;
    std::size_t at = text.find(needle);
    while (at != std::string_view::npos)
    {
        ++count;
        at = text.find(needle, at + needle.size());
    }
    return count;
}

/// Windows' actions: [Terminate] [Kill] and the priority row alone.
constexpr Platform::ProcessActionCapabilities PRIORITY_ONLY_ROW{
    .canTerminate = true, .canKill = true, .canStop = false, .canContinue = false, .canSetPriority = true};
/// Linux's: the priority row and the I/O priority row under it.
constexpr Platform::ProcessActionCapabilities WITH_IO_PRIORITY{
    .canTerminate = true, .canKill = true, .canStop = true, .canContinue = true, .canSetPriority = true, .canSetIoPriority = true};

TEST_F(ProcessActionsBlockRenderTest, ThePriorityRowAloneHasNoLabelOfItsOwnAndTheHeaderShowsTheCurrentPriority)
{
    // The issue (#1874): "Priority" was both the card's header and the row's label above the slider.
    // Alone, the row is unlabelled and the header carries the label's tooltip.
    Harness h;
    std::string text;
    const auto body = [&]
    {
        ImGui::LogToBuffer();
        (void) ProcessActionsBlock::render(h.context(PRIORITY_ONLY_ROW), layoutFor(PRIORITY_ONLY_ROW, 1900.0F), infoCardHeight(6.0F));
        text = GImGui->LogBuffer.c_str();
        ImGui::LogFinish();
    };
    (void) runFrame(body);
    (void) runFrame(body);
    EXPECT_EQ(occurrences(text, "Priority"), 1U) << text; // The header's
    EXPECT_FALSE(ProcessPriorityView::labelsRows(PRIORITY_ONLY_ROW));

    // Hovering the header shows the current priority, as hovering the row label did.
    const ImGuiWindow* card = priorityWindow();
    ASSERT_NE(card, nullptr);
    const ImGuiStyle& style = ImGui::GetStyle();
    ImGui::GetIO().AddMousePosEvent(card->Pos.x + style.WindowPadding.x + 4.0F,
                                    card->Pos.y + style.WindowPadding.y + (ImGui::GetTextLineHeight() * 0.5F));
    std::string hovered;
    int frame = 0;
    while (frame < 4) // The tooltip waits for the hover delay
    {
        ImGui::GetIO().DeltaTime = 0.5F;
        (void) runFrame(body);
        hovered += text;
        ++frame;
    }
#ifdef _WIN32
    EXPECT_TRUE(hovered.contains("Current priority class: Normal")) << hovered;
#else
    EXPECT_TRUE(hovered.contains("Current nice value: 0")) << hovered;
#endif
}

TEST_F(ProcessActionsBlockRenderTest, BesideTheIoPriorityRowThePriorityRowKeepsItsLabel)
{
    // Linux's two rows stay told apart: the header, the nice row's "Priority" label and "I/O priority".
    Harness h;
    std::string text;
    const auto body = [&]
    {
        ImGui::LogToBuffer();
        (void) ProcessActionsBlock::render(h.context(WITH_IO_PRIORITY), layoutFor(WITH_IO_PRIORITY, 1000.0F), 0.0F);
        text = GImGui->LogBuffer.c_str();
        ImGui::LogFinish();
    };
    (void) runFrame(body);
    (void) runFrame(body);
    EXPECT_TRUE(ProcessPriorityView::labelsRows(WITH_IO_PRIORITY));
    EXPECT_EQ(occurrences(text, "Priority"), 2U) << text; // The header and the nice row's label
    EXPECT_TRUE(text.contains("I/O priority")) << text;
}

TEST_F(ProcessActionsBlockRenderTest, OnARowTallerThanItTheActionsCardCentresItsButtonsUnderTheHeader)
{
    // The issue (#1874): beside Identity and Runtime the Actions card is the row's height, and its one
    // row of buttons sat at the top over a block of empty card. They are now centred in the room under
    // the header, as much card above them as below, and the height reported stays their own.
    Harness h;
    const auto layout = renderBlock(h, PRIORITY_ONLY_ROW, 1900.0F, INFO_ROW, 8.0F);
    ASSERT_TRUE(layout.besideInfo);
    const ImGuiWindow* card = actionsWindow();
    ASSERT_NE(card, nullptr);
    const ImGuiStyle& style = ImGui::GetStyle();
    ASSERT_GT(lastCardHeight, lastHeights.actions + ImGui::GetFrameHeight()); // Room enough to show it

    // In the card's own coordinates: where the rows end as drawn, and where they would end at the top.
    const float rowsBottom = style.WindowPadding.y + card->ContentSize.y;
    const float topAlignedBottom = lastHeights.actions - style.WindowPadding.y;
    const float above = rowsBottom - topAlignedBottom; // The offset added under the header
    const float below = (card->Size.y - style.WindowPadding.y) - rowsBottom;
    EXPECT_GT(above, 0.0F);
    EXPECT_NEAR(above, below, 1.5F);
    EXPECT_FLOAT_EQ(card->ScrollMax.y, 0.0F);
    // The header is still at the top, where every card's is.
    EXPECT_LT(lastHeights.actions, lastCardHeight);
}

TEST_F(ProcessActionsBlockRenderTest, SizedToTheirContentTheCardsAddNoOffset)
{
    // Stacked in a narrow pane each card is as tall as its content: nothing to centre in.
    Harness h;
    const auto layout = renderBlock(h, PRIORITY_ONLY_ROW, 120.0F, INFO_ROW, 0.0F);
    ASSERT_FALSE(layout.priorityBesideActions);
    const ImGuiWindow* card = actionsWindow();
    ASSERT_NE(card, nullptr);
    EXPECT_NEAR(card->Size.y, lastHeights.actions, 1.0F);
}

// --- Trace system calls (#182) ---------------------------------------------------------------------

/// The four buttons and the priority row with the trace button in @p availability.
constexpr Platform::ProcessActionCapabilities withTrace(Platform::SyscallTraceAvailability availability)
{
    Platform::ProcessActionCapabilities caps = ALL_ACTIONS;
    caps.syscallTrace = availability;
    return caps;
}

TEST_F(ProcessActionsBlockRenderTest, MeasureCountsTheTraceButtonWhereItIsShown)
{
    float rowOnly = 0.0F;
    float traceWidth = 0.0F;
    float available = 0.0F;
    float greyedOut = 0.0F;
    float traceOnlyButtons = 0.0F;
    (void) runFrame(
        [&]
        {
            rowOnly = ProcessActionsBlock::measure(withTrace(Platform::SyscallTraceAvailability::Unsupported)).buttons;
            traceWidth = ProcessActionsView::syscallTraceButtonWidth(withTrace(Platform::SyscallTraceAvailability::Available));
            available = ProcessActionsBlock::measure(withTrace(Platform::SyscallTraceAvailability::Available)).buttons;
            greyedOut = ProcessActionsBlock::measure(withTrace(Platform::SyscallTraceAvailability::NoTracer)).buttons;
            // A platform whose only button is the trace one: the Actions card is as wide as that button.
            Platform::ProcessActionCapabilities traceOnly;
            traceOnly.syscallTrace = Platform::SyscallTraceAvailability::Available;
            traceOnlyButtons = ProcessActionsBlock::measure(traceOnly).buttons;
        });
    EXPECT_GT(traceWidth, 0.0F);
    EXPECT_FLOAT_EQ(ProcessActionsView::syscallTraceButtonWidth(withTrace(Platform::SyscallTraceAvailability::Unsupported)), 0.0F);
    EXPECT_FLOAT_EQ(available, std::max(rowOnly, traceWidth));
    // Greyed out it is still drawn, so it is still measured.
    EXPECT_FLOAT_EQ(greyedOut, available);
    EXPECT_FLOAT_EQ(traceOnlyButtons, traceWidth);
    EXPECT_TRUE(ProcessActionsBlock::hasAnyAction(
        Platform::ProcessActionCapabilities{.syscallTrace = Platform::SyscallTraceAvailability::NoTerminal}));
}

TEST_F(ProcessActionsBlockRenderTest, TheTraceButtonFitsTheCardWideOrNarrow)
{
    // Laid out as ProcessDetailsPanel does it, with the heights the cards reported last frame.
    constexpr Platform::ProcessActionCapabilities CAPS = withTrace(Platform::SyscallTraceAvailability::Available);
    for (const float paneWidth : {1900.0F, 1000.0F, 300.0F})
    {
        SCOPED_TRACE(paneWidth);
        Harness h;
        const auto layout = renderBlock(h, CAPS, paneWidth, INFO_ROW, 6.0F);
        for (const ImGuiWindow* card : {actionsWindow(), priorityWindow()})
        {
            ASSERT_NE(card, nullptr);
            // Nothing runs out of a card or makes it scroll, sideways or down: the button followed the
            // row or started its own, and the row is as tall as its tallest card (#1789).
            EXPECT_FLOAT_EQ(card->ScrollMax.x, 0.0F);
            EXPECT_FLOAT_EQ(card->ScrollMax.y, 0.0F);
        }
        EXPECT_LE(actionsWindow()->ContentSize.x, layout.actionsWidth - (2.0F * ImGui::GetStyle().WindowPadding.x) + 1.0F);
        EXPECT_EQ(h.mock.syscallTraceCount(), 0); // Drawing it launches nothing
    }
}

TEST_F(ProcessActionsBlockRenderTest, TheTraceButtonAddsARowOnlyWhereItIsShown)
{
    // Wrapped below the Identity/Runtime row, the Actions card grows by the trace button's row on Linux
    // and not at all where it is hidden.
    Harness hidden;
    (void) renderBlock(hidden, withTrace(Platform::SyscallTraceAvailability::Unsupported), 1000.0F, INFO_ROW, 0.0F);
    const float hiddenHeight = lastHeights.actions;
    Harness shown;
    (void) renderBlock(shown, withTrace(Platform::SyscallTraceAvailability::Available), 1000.0F, INFO_ROW, 0.0F);
    const float shownHeight = lastHeights.actions;
    EXPECT_GT(shownHeight, hiddenHeight);
    EXPECT_LE(shownHeight - hiddenHeight, ImGui::GetFrameHeightWithSpacing() + 1.0F);
}

// --- Section cards (#1537) -------------------------------------------------------------------------

/// What makes a child window look like a card: its flags, and whether it pushed its own border and
/// background colours. The test's sentinel colours are what a child that pushed nothing would draw in.
struct CardLook
{
    bool bordered = false;
    bool neverScrolls = false;
    bool drawnInTheSentinelColours = false;
};

/// The style colours a child gets when nothing is pushed for it: distinct from every colour the theme
/// stub hands out, so a card that does not push its own shows up.
constexpr ImVec4 SENTINEL_BORDER{1.0F, 0.0F, 1.0F, 1.0F};
constexpr ImVec4 SENTINEL_CHILD_BG{0.0F, 1.0F, 1.0F, 1.0F};

[[nodiscard]] CardLook cardLookOf(const ImGuiWindow& window)
{
    const ImU32 sentinelBorder = ImGui::ColorConvertFloat4ToU32(SENTINEL_BORDER);
    const ImU32 sentinelBackground = ImGui::ColorConvertFloat4ToU32(SENTINEL_CHILD_BG);
    bool sentinel = false;
    for (const ImDrawVert& vertex : window.DrawList->VtxBuffer)
    {
        sentinel = sentinel || vertex.col == sentinelBorder || vertex.col == sentinelBackground;
    }
    return CardLook{
        .bordered = (window.ChildFlags & ImGuiChildFlags_Borders) != 0,
        .neverScrolls = (window.Flags & UI::Widgets::CARD_WINDOW_FLAGS) == UI::Widgets::CARD_WINDOW_FLAGS,
        .drawnInTheSentinelColours = sentinel,
    };
}

/// Identity and Runtime as ProcessDetailsPanel draws them, @p cardWidth wide, then the Actions and
/// Priority cards laid out for @p paneWidth, the row's height the tallest card's as of @p last (updated
/// to this frame's), then a one-cell chart grid as CPU Cores draws its cells, for comparison. Returns
/// the cards' layout.
ProcessDetailsLayout::TopRowLayout
renderOverviewRow(ProcessActionsBlock::Context context, float paneWidth, float cardWidth, ProcessActionsBlock::Heights& last)
{
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float infoHeight = infoCardHeight(6.0F);
    const auto layout = layoutFor(context.capabilities, paneWidth, cardWidth + spacing + cardWidth);
    const float controlsHeight = controlsHeightFor(layout, infoHeight, last);
    const float rowHeight = layout.besideInfo ? controlsHeight : infoHeight;
    const auto rows = []
    {
        for (int row = 0; row < 6; ++row)
        {
            ImGui::TextUnformatted("Label  value");
        }
    };
    (void) ProcessOverviewCard::render(
        "BasicInfoLeft", ICON_FA_ID_CARD, "Identity", ImVec2(cardWidth, rowHeight), ImGuiChildFlags_None, rows);
    ImGui::SameLine();
    (void) ProcessOverviewCard::render(
        "BasicInfoRight", ICON_FA_CLOCK, "Runtime", ImVec2(cardWidth, rowHeight), ImGuiChildFlags_None, rows);

    if (layout.besideInfo)
    {
        ImGui::SameLine();
    }
    last = ProcessActionsBlock::render(context, layout, controlsHeight);

    UI::Widgets::ChartGridConfig grid;
    grid.availableWidth = 400.0F;
    grid.availableHeight = 300.0F;
    UI::Widgets::renderChartGrid(
        "CoresLikeGrid", 1, grid, [](std::size_t /*index*/, float /*width*/, float /*height*/) { ImGui::TextUnformatted("cpu0"); });
    return layout;
}

TEST_F(ProcessActionsBlockRenderTest, TheOverviewCardsLookLikeAChartGridCellBesideTheRowAndBelowIt)
{
    ImGui::GetStyle().Colors[ImGuiCol_Border] = SENTINEL_BORDER;
    ImGui::GetStyle().Colors[ImGuiCol_ChildBg] = SENTINEL_CHILD_BG;
    for (const bool beside : {true, false})
    {
        SCOPED_TRACE(beside ? "beside the row" : "wrapped below it");
        Harness h;
        ProcessDetailsLayout::TopRowLayout layout;
        ProcessActionsBlock::Heights last;
        const auto body = [&]
        {
            layout = renderOverviewRow(h.context(ALL_ACTIONS), beside ? 1900.0F : 1000.0F, 384.0F, last);
        };
        (void) runFrame(body);
        (void) runFrame(body);
        ASSERT_EQ(layout.besideInfo, beside);

        const ImGuiWindow* cell = childWindow("GridCell");
        ASSERT_NE(cell, nullptr);
        const CardLook cellLook = cardLookOf(*cell);
        ASSERT_TRUE(cellLook.bordered);
        ASSERT_TRUE(cellLook.neverScrolls);
        ASSERT_FALSE(cellLook.drawnInTheSentinelColours);

        for (const std::string_view id : {"BasicInfoLeft", "BasicInfoRight", "ProcessActionsBlock", "ProcessPriorityBlock"})
        {
            SCOPED_TRACE(id);
            const ImGuiWindow* card = childWindow(id);
            ASSERT_NE(card, nullptr);
            const CardLook look = cardLookOf(*card);
            EXPECT_EQ(look.bordered, cellLook.bordered);
            EXPECT_EQ(look.neverScrolls, cellLook.neverScrolls);
            EXPECT_EQ(look.drawnInTheSentinelColours, cellLook.drawnInTheSentinelColours);
        }
    }
}

TEST_F(ProcessActionsBlockRenderTest, BesideTheRowTheFourCardsShareTheirTopAndBottomEdges)
{
    Harness h;
    ProcessDetailsLayout::TopRowLayout layout;
    ProcessActionsBlock::Heights last;
    const auto body = [&]
    {
        layout = renderOverviewRow(h.context(ALL_ACTIONS), 1900.0F, 384.0F, last);
    };
    // The first frame measures the cards, the second sizes the row by them, the third settles.
    for (int frame = 0; frame < 3; ++frame)
    {
        (void) runFrame(body);
    }
    ASSERT_TRUE(layout.besideInfo);

    const ImGuiWindow* identity = childWindow("BasicInfoLeft");
    const ImGuiWindow* runtime = childWindow("BasicInfoRight");
    const ImGuiWindow* actions = actionsWindow();
    const ImGuiWindow* priority = priorityWindow();
    ASSERT_NE(identity, nullptr);
    ASSERT_NE(runtime, nullptr);
    ASSERT_NE(actions, nullptr);
    ASSERT_NE(priority, nullptr);
    for (const ImGuiWindow* card : {runtime, actions, priority})
    {
        EXPECT_FLOAT_EQ(card->Pos.y, identity->Pos.y);
        EXPECT_FLOAT_EQ(card->Pos.y + card->Size.y, identity->Pos.y + identity->Size.y);
        EXPECT_FLOAT_EQ(card->ScrollMax.y, 0.0F); // Every card's content fits the shared height
    }
    // Left to right: Identity, Runtime, Actions, Priority, none overlapping.
    EXPECT_GT(runtime->Pos.x, identity->Pos.x + identity->Size.x);
    EXPECT_GT(actions->Pos.x, runtime->Pos.x + runtime->Size.x);
    EXPECT_GT(priority->Pos.x, actions->Pos.x + actions->Size.x);
    // The header is inside each card, at its top (as a core's name heads its grid cell), so nothing
    // of the section is drawn above the card's top edge.
    EXPECT_FLOAT_EQ(identity->Pos.y, ImGui::GetStyle().WindowPadding.y);
}

TEST_F(ProcessActionsBlockRenderTest, WrappedBelowTheRowTheCardsStartARowOfTheirOwn)
{
    Harness h;
    ProcessDetailsLayout::TopRowLayout layout;
    ProcessActionsBlock::Heights last;
    const auto body = [&]
    {
        layout = renderOverviewRow(h.context(ALL_ACTIONS), 1000.0F, 384.0F, last);
    };
    for (int frame = 0; frame < 3; ++frame)
    {
        (void) runFrame(body);
    }
    ASSERT_FALSE(layout.besideInfo);

    const ImGuiWindow* identity = childWindow("BasicInfoLeft");
    const ImGuiWindow* actions = actionsWindow();
    const ImGuiWindow* priority = priorityWindow();
    ASSERT_NE(identity, nullptr);
    ASSERT_NE(actions, nullptr);
    ASSERT_NE(priority, nullptr);
    // Identity keeps its own height: the cards below do not stretch it.
    EXPECT_FLOAT_EQ(identity->Size.y, infoCardHeight(6.0F));
    EXPECT_GE(actions->Pos.y, identity->Pos.y + identity->Size.y);
    EXPECT_FLOAT_EQ(actions->Pos.x, identity->Pos.x);
    EXPECT_NEAR(actions->Size.x, layout.actionsWidth, 1.0F);
    EXPECT_FLOAT_EQ(priority->Pos.y, actions->Pos.y);
    EXPECT_FLOAT_EQ(priority->Size.y, actions->Size.y);
    EXPECT_FLOAT_EQ(actions->ScrollMax.y, 0.0F);
    EXPECT_FLOAT_EQ(priority->ScrollMax.y, 0.0F);
}

// #807: Create dump file... is in the card only where the platform can write a dump; from it, a confirm,
// the dump through the platform for the process shown, and a result naming the file, closed by Close.
TEST_F(ProcessActionsBlockRenderTest, CreateDumpFromTheCardConfirmsWritesAndNamesTheFile)
{
    Harness h;
    Platform::ProcessActionCapabilities caps = ALL_ACTIONS;
    std::string text;
    const auto body = [&]
    {
        ImGui::LogToBuffer();
        (void) ProcessActionsBlock::render(h.context(caps), layoutFor(caps, 1600.0F, 600.0F), infoCardHeight(6.0F));
        h.actionsView.renderConfirmation(&h.mock, TARGET);
        text = GImGui->LogBuffer.c_str();
        ImGui::LogFinish();
    };

    // Not offered where the platform can't (Linux until its half lands).
    ASSERT_FALSE(runFrame(body));
    EXPECT_FALSE(text.contains("Create dump file")) << text;
    float withoutDump = 0.0F;
    float withDump = 0.0F;
    float dumpButton = 0.0F;
    (void) runFrame(
        [&]
        {
            withoutDump = ProcessActionsBlock::measure(caps).buttons;
            Platform::ProcessActionCapabilities dumpCaps = caps;
            dumpCaps.canCreateDump = true;
            withDump = ProcessActionsBlock::measure(dumpCaps).buttons;
            dumpButton = ProcessActionsView::createDumpButtonWidth(dumpCaps);
        });
    EXPECT_FLOAT_EQ(ProcessActionsView::createDumpButtonWidth(caps), 0.0F);
    EXPECT_GT(dumpButton, 0.0F);
    EXPECT_FLOAT_EQ(withDump, std::max(withoutDump, dumpButton));

    caps.canCreateDump = true;
    const std::filesystem::path file = std::filesystem::path("dumps") / "victim_1001.dmp";
    h.mock.setDumpResult(Platform::ProcessDump{
        .file = file, .sizeBytes = 4096, .note = "Written by the 32-bit helper", .fromSnapshot = true, .liveReason = {}});
    ASSERT_FALSE(runFrame(body));
    EXPECT_TRUE(text.contains("Create dump file...")) << text;

    ImGui::ActivateItemByID(ImHashStr(DumpDetail::CREATE_DUMP_LABEL, 0, actionsWindow()->ID));
    (void) runFrame(body);
    // The folder is checked on the confirm's worker (#3155): taken in here, or by a frame.
    static_cast<void>(h.actionsView.dumpView().takeConfirmCheck(&h.mock, true));
    (void) runFrame(body);
    EXPECT_TRUE(h.actionsView.dumpView().confirmPending());
    EXPECT_FALSE(h.actionsView.dumpView().checkingFolder());
    EXPECT_TRUE(text.contains("A full memory dump of victim (PID 1001)")) << text;
    EXPECT_EQ(h.mock.createDumpCount(), 0); // nothing until confirmed
    // F9 waits while a dump's dialog is up.
    EXPECT_FALSE(h.actionsView.requestKillShortcut(caps, TARGET, h.name));

    const ImGuiWindow* modal = ImGui::GetTopMostPopupModal();
    ASSERT_NE(modal, nullptr);
    ImGui::ActivateItemByID(ImHashStr(DumpDetail::CONFIRM_LABEL, 0, modal->ID));
    for (int frame = 0; frame < 200 && !h.actionsView.dumpView().result().has_value(); ++frame)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        (void) runFrame(body);
    }
    (void) runFrame(body);
    EXPECT_EQ(h.mock.createDumpCount(), 1);
    EXPECT_EQ(h.mock.lastTarget(), TARGET);
    EXPECT_EQ(h.mock.lastDumpDirectory(), DumpDetail::defaultDumpDirectory());
    EXPECT_TRUE(text.contains("Wrote a dump file of victim (PID 1001)")) << text;
    EXPECT_TRUE(text.contains(file.string())) << text;
    EXPECT_TRUE(text.contains("Written from a snapshot")) << text;      // #2001
    EXPECT_TRUE(text.contains("Written by the 32-bit helper")) << text; // the platform's note, under the result (#1891)

    modal = ImGui::GetTopMostPopupModal();
    ASSERT_NE(modal, nullptr);
    ImGui::ActivateItemByID(ImHashStr("Close", 0, modal->ID));
    (void) runFrame(body);
    EXPECT_FALSE(runFrame(body));
    EXPECT_FALSE(h.actionsView.dumpView().active());
}

// #2001: the dump's confirm warns when the folder's drive is too full, and offers Save dump to... where
// the platform has a folder dialog; pressing it opens that dialog and leaves the confirm up.
TEST_F(ProcessActionsBlockRenderTest, TheDumpConfirmWarnsOfLowSpaceAndOffersAFolder)
{
    Harness h;
    Platform::ProcessActionCapabilities caps = ALL_ACTIONS;
    caps.canCreateDump = true;
    caps.canChooseDumpFolder = true;
    h.mock.setCapabilities(caps);
    // Too little room for a 3 GiB process; and the folder dialog is cancelled (the mock's default), so
    // no folder is remembered in the config.
    constexpr std::uint64_t GIB = std::uint64_t{1024} * 1024U * 1024U;
    h.mock.setDumpSpace({.freeBytes = GIB, .processBytes = 3 * GIB});
    std::string text;
    const auto body = [&]
    {
        ImGui::LogToBuffer();
        (void) ProcessActionsBlock::render(h.context(caps), layoutFor(caps, 1600.0F, 600.0F), infoCardHeight(6.0F));
        h.actionsView.renderConfirmation(&h.mock, TARGET);
        text = GImGui->LogBuffer.c_str();
        ImGui::LogFinish();
    };
    (void) runFrame(body);
    ImGui::ActivateItemByID(ImHashStr(DumpDetail::CREATE_DUMP_LABEL, 0, actionsWindow()->ID));
    (void) runFrame(body);
    // The folder and its drive are read on the confirm's worker (#3155): taken in here, or by a frame,
    // so the mock's count is read after the worker wrote it.
    static_cast<void>(h.actionsView.dumpView().takeConfirmCheck(&h.mock, true));
    (void) runFrame(body);
    ASSERT_TRUE(h.actionsView.dumpView().confirmPending());
    EXPECT_EQ(h.mock.dumpSpaceCount(), 1);
    EXPECT_TRUE(text.contains("may not fit")) << text;
    EXPECT_TRUE(text.contains("Save dump to...")) << text;

    const ImGuiWindow* modal = ImGui::GetTopMostPopupModal();
    ASSERT_NE(modal, nullptr);
    ImGui::ActivateItemByID(ImHashStr(DumpDetail::CHOOSE_FOLDER_LABEL, 0, modal->ID));
    (void) runFrame(body); // the press
    (void) runFrame(body);
    // The dialog's answer is taken in by a frame, or here; either way after the worker is done, so the
    // mock's counts are read after it wrote them.
    static_cast<void>(h.actionsView.dumpView().takeFolderChoice(&h.mock, true));
    (void) runFrame(body);
    EXPECT_EQ(h.mock.chooseFolderCount(), 1);
    EXPECT_EQ(h.mock.lastFolderInitial(), DumpDetail::defaultDumpDirectory());
    EXPECT_TRUE(h.actionsView.dumpView().confirmPending()); // cancelled: the confirm is still up, as it was
    EXPECT_EQ(h.mock.createDumpCount(), 0);

    modal = ImGui::GetTopMostPopupModal();
    ASSERT_NE(modal, nullptr);
    ImGui::ActivateItemByID(ImHashStr("Cancel", 0, modal->ID));
    (void) runFrame(body);
    (void) runFrame(body);
    EXPECT_FALSE(h.actionsView.dumpView().active());
}

// --- The folder dialog's owner window (#2040) ------------------------------------------------------

/// Opens the dump's confirm, presses Save dump to..., and returns the owner window the folder dialog
/// was handed (the mock records it; the dialog is the mock's, so nothing is shown).
void* folderDialogOwner()
{
    TestMocks::MockProcessActions mock;
    ProcessActionsView actionsView;
    ProcessPriorityView priorityView;
    const std::string name = "victim";
    Platform::ProcessActionCapabilities caps = ALL_ACTIONS;
    caps.canCreateDump = true;
    caps.canChooseDumpFolder = true;
    mock.setCapabilities(caps);
    const ProcessActionsBlock::Context context{
        .actionsView = &actionsView,
        .priorityView = &priorityView,
        .actions = &mock,
        .capabilities = caps,
        .processName = &name,
        .target = TARGET,
        .currentNice = std::optional<std::int32_t>{0},
    };
    const auto frame = [&]
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
        ImGui::SetNextWindowSize(ImVec2(1600.0F, 1000.0F));
        ImGui::Begin("Details", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
        (void) ProcessActionsBlock::render(context, layoutFor(caps, 1600.0F, 600.0F), infoCardHeight(6.0F));
        actionsView.renderConfirmation(&mock, TARGET);
        ImGui::End();
        ImGui::Render();
    };
    frame();
    const ImGuiWindow* actions = actionsWindow();
    EXPECT_NE(actions, nullptr);
    if (actions == nullptr)
    {
        return nullptr;
    }
    ImGui::ActivateItemByID(ImHashStr(DumpDetail::CREATE_DUMP_LABEL, 0, actions->ID));
    frame();
    // The folder is checked on the confirm's worker (#3155), and Save dump to... waits for it.
    static_cast<void>(actionsView.dumpView().takeConfirmCheck(&mock, true));
    frame();
    const ImGuiWindow* modal = ImGui::GetTopMostPopupModal();
    EXPECT_NE(modal, nullptr);
    if (modal == nullptr)
    {
        return nullptr;
    }
    ImGui::ActivateItemByID(ImHashStr(DumpDetail::CHOOSE_FOLDER_LABEL, 0, modal->ID));
    frame(); // the press
    frame();
    static_cast<void>(actionsView.dumpView().takeFolderChoice(&mock, true)); // after the worker wrote the mock's record
    EXPECT_EQ(mock.chooseFolderCount(), 1);
    return mock.lastFolderOwner();
}

TEST_F(ProcessActionsBlockRenderTest, WithoutAnApplicationTheFolderDialogHasNoOwner)
{
    EXPECT_EQ(folderDialogOwner(), nullptr);
}

TEST_F(ProcessActionsBlockRenderTest, AHeadlessApplicationGivesTheFolderDialogNoOwner)
{
    // No window to be modal to (#880's headless driver): the dialog is shown without an owner.
    Core::ApplicationSpecification spec;
    spec.Name = "FolderDialogOwnerTest";
    spec.Headless = true;
    const Core::Application app(spec);
    ASSERT_FALSE(app.hasWindow());
    EXPECT_EQ(folderDialogOwner(), nullptr);
}

#ifdef _WIN32
/// A display that can back the app's window: a GL 3.3 core context on the desktop, CI included
/// (#2134), as test_Application.cpp checks.
[[nodiscard]] bool hasDisplay()
{
    return TestSupport::enforceDisplayRequirement(TestSupport::detectWindowsDisplay());
}

// The one Windows-only line: the main window's HWND, which SDL has only on Windows.
TEST_F(ProcessActionsBlockRenderTest, TheFolderDialogIsOwnedByTheMainWindowsHwnd)
{
    if (!hasDisplay())
    {
        GTEST_SKIP() << "No display available (headless environment)";
    }
    Core::ApplicationSpecification spec;
    spec.Name = "FolderDialogOwnerTest";
    spec.Width = 320;
    spec.Height = 240;
    spec.VSync = false;
    const Core::Application app(spec);
    ASSERT_TRUE(app.hasWindow());
    const void* const hwnd =
        SDL_GetPointerProperty(SDL_GetWindowProperties(app.getWindow().getHandle()), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
    ASSERT_NE(hwnd, nullptr);
    EXPECT_EQ(folderDialogOwner(), hwnd); // modal to the main window, so it can't fall behind it
}
#endif

} // namespace
} // namespace App
