/// @file test_ProcessActionsBlockRender.cpp
/// @brief The Overview's Actions, Priority and I/O priority cards (#1493, #1789, #3741), headless: the
/// Actions card holds the buttons at their labels' width, the Priority card the priority slider and the
/// I/O priority card, under it in the same column, the I/O slider, each as wide as its content. Each is
/// exactly as tall as its content, beside the Identity/Runtime row or wrapped below it, side by side or
/// one under the other in a narrow pane, and grows while a result or error line shows (#3741); and the
/// shared confirm dialog still opens for them, acting on nothing until confirmed. Identity, Runtime,
/// Actions, Priority and I/O priority are cards styled as a CPU Cores grid cell is (#1537).

#include "App/Panels/ProcessActionsBlock.h"
#include "App/Panels/ProcessActionsView.h"
#include "App/Panels/ProcessAffinityDialog.h"
#include "App/Panels/ProcessDetailsLayout.h"
#include "App/Panels/ProcessDetailsPanel_ActionHelpers.h"
#include "App/Panels/ProcessDumpView.h"
#include "App/Panels/ProcessOverviewCard.h"
#include "App/Panels/ProcessPriorityView.h"
#include "App/Panels/ProcessSchedPolicyDialog.h"
#include "Core/Application.h"
#include "Domain/PriorityConfig.h"
#include "Mocks/FakeHttpsClient.h"
#include "Mocks/MockProbes.h"
#include "Platform/CpuAffinity.h"
#include "Platform/IHttpsClient.h"
#include "Platform/IProcessActions.h"
#include "Platform/IoPriorityTypes.h"
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
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>

namespace App::ProcessActionsBlockRenderTestDetail
{

/// Look up on VirusTotal's client in these tests (#3094): the shared fake (#3849).
using FakeLookupClient = TestMocks::FakeHttpsClient;

} // namespace App::ProcessActionsBlockRenderTestDetail

namespace App
{
namespace
{

constexpr Platform::ProcessTarget BLOCK_RENDER_TARGET{.pid = 1001, .startTimeTicks = 5000};

constexpr Platform::ProcessActionCapabilities BLOCK_RENDER_ACTIONS{
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

/// The I/O priority card's child window, as the last frame left it (#3741).
[[nodiscard]] const ImGuiWindow* ioPriorityWindow()
{
    return childWindow("ProcessIoPriorityBlock_");
}

/// How much taller @p card was drawn than what it holds: its height less its content's and its padding
/// top and bottom. 0 for a card exactly as tall as its content (#3741); a card given a row's height, as
/// they were (#1789), had the rest of the row as blank card under (or around) its rows.
[[nodiscard]] float spareHeight(const ImGuiWindow& card)
{
    return card.Size.y - (card.ContentSize.y + (2.0F * card.WindowPadding.y));
}

/// The Identity/Runtime card height for @p rows rows, worked out as ProcessDetailsPanel does: the
/// header line and the rows, inside the card's padding (#1537).
[[nodiscard]] float infoCardHeight(float rows)
{
    return ProcessDetailsLayout::computeInfoCardHeight(rows, ImGui::GetTextLineHeightWithSpacing(), ImGui::GetStyle().WindowPadding.y);
}

/// The layout ProcessDetailsPanel picks for @p capabilities in @p paneWidth after Identity and Runtime, which
/// take @p infoRowWidth with the gap between them (#4029).
[[nodiscard]] ProcessDetailsLayout::OverviewLayout layoutFor(const Platform::ProcessActionCapabilities& capabilities,
                                                             float paneWidth,
                                                             float infoRowWidth = INFO_ROW,
                                                             bool virusTotalOffered = false)
{
    const ProcessActionsBlock::Widths widths = ProcessActionsBlock::measure(capabilities, virusTotalOffered);
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float info = (infoRowWidth - spacing) * 0.5F;
    return ProcessDetailsLayout::layoutOverview(paneWidth,
                                                spacing,
                                                {info,
                                                 info,
                                                 widths.actionsCard(),
                                                 capabilities.canSetPriority ? widths.priorityCard() : 0.0F,
                                                 capabilities.canSetIoPriority ? widths.priorityCard() : 0.0F});
}

/// The cards' own content heights, last measured, as ProcessDetailsPanel keeps them (#4029). Reset per test.
[[nodiscard]] std::array<float, ProcessDetailsLayout::OVERVIEW_CARD_COUNT>& measuredHeights()
{
    static std::array<float, ProcessDetailsLayout::OVERVIEW_CARD_COUNT> heights{};
    return heights;
}

/// renderCards() as the panel calls it: each row as tall as its tallest card, from the heights
/// measured last frame, and this frame's measured back into them.
void renderCards(const ProcessActionsBlock::Context& context, const ProcessDetailsLayout::OverviewLayout& layout)
{
    ProcessActionsBlock::render(context, layout, ProcessDetailsLayout::rowHeights(layout, measuredHeights()), measuredHeights());
}

using ProcessDetailsLayout::OverviewCard;

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
        measuredHeights() = {};
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
        TestMocks::MockAffinityLayoutActions mock; // counts the Set affinity dialog's layout reads (#3182)
        ProcessActionsView actionsView;
        ProcessPriorityView priorityView;
        std::string name = "victim";

        Harness()
        {
            // The view's default reads this session's elevation (#4141): pinned, so an elevated run uses %TEMP% too.
            actionsView.dumpView().setNoDefaultFolder(false);
        }

        [[nodiscard]] ProcessActionsBlock::Context context(const Platform::ProcessActionCapabilities& capabilities)
        {
            return ProcessActionsBlock::Context{
                .actionsView = &actionsView,
                .priorityView = &priorityView,
                .actions = &mock,
                .capabilities = capabilities,
                .processName = &name,
                .target = BLOCK_RENDER_TARGET,
                .currentNice = std::optional<std::int32_t>{0},
            };
        }
    };

    /// Draws the cards for @p capabilities, laid out as the panel lays them out in @p paneWidth beside
    /// an Identity/Runtime row @p infoRowWidth wide; three frames, so the auto-resizing cards have settled.
    static ProcessDetailsLayout::OverviewLayout
    renderBlock(Harness& h, const Platform::ProcessActionCapabilities& capabilities, float paneWidth, float infoRowWidth)
    {
        ProcessDetailsLayout::OverviewLayout layout;
        measuredHeights() = {}; // a fresh panel: nothing measured yet
        const auto body = [&]
        {
            layout = layoutFor(capabilities, paneWidth, infoRowWidth, h.actionsView.virusTotalOffered(capabilities));
            renderCards(h.context(capabilities), layout);
        };
        for (int frame = 0; frame < 3; ++frame)
        {
            (void) runFrame(body);
        }
        return layout;
    }

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
            buttons = ProcessActionsBlock::measure(NO_PRIORITY, false).buttons;
            const float widest = std::max(ImGui::CalcTextSize(Detail::ACTION_BUTTONS.at(0).label).x,
                                          ImGui::CalcTextSize(Detail::ACTION_BUTTONS.at(1).label).x);
            expected = ProcessDetailsLayout::computeActionButtonRowWidth(
                ProcessDetailsLayout::computeActionButtonWidth(widest, ImGui::GetStyle().FramePadding.x),
                2,
                ImGui::GetStyle().ItemSpacing.x);
            withoutPriority = ProcessActionsBlock::measure(NO_PRIORITY, false).priority;
            withoutPriorityCard = ProcessActionsBlock::measure(NO_PRIORITY, false).priorityCard();
            Platform::ProcessActionCapabilities ioOnly = NO_PRIORITY;
            ioOnly.canSetIoPriority = true;
            withIoPriorityOnly = ProcessActionsBlock::measure(ioOnly, false).priority;
        });
    EXPECT_FLOAT_EQ(buttons, expected);
    EXPECT_FLOAT_EQ(withoutPriority, 0.0F);
    EXPECT_FLOAT_EQ(withoutPriorityCard, 0.0F); // No priority control: no Priority card, and no gap for it
    // ProcessPriorityView also draws the I/O priority row on its own (#803), so it is measured.
    EXPECT_GT(withIoPriorityOnly, 0.0F);
}

TEST_F(ProcessActionsBlockRenderTest, BesideTheRowTheCardsShareTheRowsHeightAndAreAsWideAsTheirRows)
{
    // #4029: a pane wide enough puts Actions and Priority on the Identity/Runtime row, Priority to the right of
    // Actions, each as wide as its rows, and the row is as tall as its tallest card: both cards that height.
    Harness h;
    const auto layout = renderBlock(h, BLOCK_RENDER_ACTIONS, 1900.0F, INFO_ROW);
    ASSERT_TRUE(layout.onFirstRow(OverviewCard::Actions));
    ASSERT_TRUE(layout.sameRow(OverviewCard::Actions, OverviewCard::Priority));

    const ImGuiWindow* actions = actionsWindow();
    const ImGuiWindow* priority = priorityWindow();
    ASSERT_NE(actions, nullptr);
    ASSERT_NE(priority, nullptr);
    for (const ImGuiWindow* card : {actions, priority})
    {
        EXPECT_FLOAT_EQ(card->ScrollMax.y, 0.0F);
        EXPECT_FLOAT_EQ(card->Pos.y, actions->Pos.y);
    }
    // One band: the same height, the tallest card's (the slider and its Apply), with no spare card under it.
    EXPECT_NEAR(actions->Size.y, priority->Size.y, 1.0F);
    EXPECT_NEAR(std::min(spareHeight(*actions), spareHeight(*priority)), 0.0F, 1.0F);
    EXPECT_NEAR(actions->Size.x, layout[OverviewCard::Actions].width, 1.0F); // ImGui rounds a window's size to whole pixels
    EXPECT_NEAR(priority->Size.x, layout[OverviewCard::Priority].width, 1.0F);
    EXPECT_GT(priority->Pos.x, actions->Pos.x + actions->Size.x);
    // Nothing in either is wider than the card allows: no stretched buttons, no row running out of it.
    EXPECT_LE(actions->ContentSize.x, layout[OverviewCard::Actions].width - (2.0F * ImGui::GetStyle().WindowPadding.x) + 1.0F);
    EXPECT_LE(priority->ContentSize.x, layout[OverviewCard::Priority].width - (2.0F * ImGui::GetStyle().WindowPadding.x) + 1.0F);
}

#ifdef _WIN32
TEST_F(ProcessActionsBlockRenderTest, OnWindowsThePriorityCardDrawsTheClassSliderWhole)
{
    // Windows' actions: [Terminate] [Kill] in the Actions card, and the priority slider with a stop per
    // class (#1538), not a combo, in the Priority card. The card is measured wide enough for the
    // slider's five stop names, and wrapped below the row it is shown whole: nothing to scroll. 780px: too narrow
    // for Actions beside Identity and Runtime, so the cards wrap (#4029; at 1000px Actions now fits on the row).
    constexpr Platform::ProcessActionCapabilities WINDOWS_ACTIONS{
        .canTerminate = true, .canKill = true, .canStop = false, .canContinue = false, .canSetPriority = true};
    Harness h;
    const auto layout = renderBlock(h, WINDOWS_ACTIONS, 780.0F, INFO_ROW);
    ASSERT_FALSE(layout.onFirstRow(OverviewCard::Actions));
    float priorityWidth = 0.0F;
    float sliderMinWidth = 0.0F;
    (void) runFrame(
        [&]
        {
            priorityWidth = ProcessActionsBlock::measure(WINDOWS_ACTIONS, false).priority;
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
        renderCards(h.context(WINDOWS_ACTIONS), layoutFor(WINDOWS_ACTIONS, 780.0F));
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

TEST_F(ProcessActionsBlockRenderTest, WrappedBelowTheRowTheCardsSitSideBySideAsOneBand)
{
    Harness h;
    // #4029: a pane too narrow for Actions beside the 776px Identity/Runtime row, but wide enough for Actions and
    // Priority side by side on the next.
    const auto layout = renderBlock(h, BLOCK_RENDER_ACTIONS, 780.0F, INFO_ROW);
    ASSERT_FALSE(layout.onFirstRow(OverviewCard::Actions));
    ASSERT_TRUE(layout.sameRow(OverviewCard::Actions, OverviewCard::Priority));

    const ImGuiWindow* actions = actionsWindow();
    const ImGuiWindow* priority = priorityWindow();
    ASSERT_NE(actions, nullptr);
    ASSERT_NE(priority, nullptr);
    for (const ImGuiWindow* card : {actions, priority})
    {
        EXPECT_FLOAT_EQ(card->ScrollMax.y, 0.0F);
        EXPECT_FLOAT_EQ(card->Pos.y, actions->Pos.y);
    }
    // The row as tall as its tallest card, both cards that height.
    EXPECT_NEAR(actions->Size.y, priority->Size.y, 1.0F);
    EXPECT_NEAR(std::min(spareHeight(*actions), spareHeight(*priority)), 0.0F, 1.0F);
    // Still their contents' widths, left-aligned, not the pane's.
    EXPECT_LT(actions->Size.x + priority->Size.x, 780.0F);
}

TEST_F(ProcessActionsBlockRenderTest, InANarrowPaneTheCardsStackHeldToItAndTheButtonsWrap)
{
    Harness h;
    const auto layout = renderBlock(h, BLOCK_RENDER_ACTIONS, 120.0F, INFO_ROW);
    ASSERT_FALSE(layout.onFirstRow(OverviewCard::Actions));
    ASSERT_FALSE(layout.sameRow(OverviewCard::Actions, OverviewCard::Priority));

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
    ASSERT_TRUE(h.actionsView.requestKillShortcut(BLOCK_RENDER_ACTIONS, BLOCK_RENDER_TARGET, h.name));

    std::string modalName;
    const auto body = [&]
    {
        renderCards(h.context(BLOCK_RENDER_ACTIONS), layoutFor(BLOCK_RENDER_ACTIONS, 1600.0F, 600.0F));
        // As ProcessDetailsPanel does: the dialog from panel scope, every frame.
        h.actionsView.renderConfirmation(&h.mock, BLOCK_RENDER_TARGET);
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
    view.requestAction(Detail::ProcessAction::Kill, BLOCK_RENDER_TARGET, "elevated.exe");
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
              (Platform::Broker::Request{
                  Platform::Broker::SignalRequest{.target = BLOCK_RENDER_TARGET, .signal = Platform::Broker::Signal::Kill}}));
    EXPECT_TRUE(view.lastResult().ok) << view.lastResult().text;
}
#endif

// #1887: where the platform can set affinity, the Actions card has Set affinity...; it opens the affinity
// dialog with the process's CPUs ticked, Apply sets them on that process and the card's line says so, and
// a selection change closes the dialog unapplied.
TEST_F(ProcessActionsBlockRenderTest, SetAffinityFromTheCardOpensTheDialogAndAppliesToTheProcess)
{
    Harness h;
    Platform::ProcessActionCapabilities caps = BLOCK_RENDER_ACTIONS;
    caps.canSetAffinity = true;
    const Platform::CpuAffinity current = Platform::CpuAffinity::fromMask(0b11U);
    std::string text;
    const auto body = [&]
    {
        ImGui::LogToBuffer();
        ProcessActionsBlock::Context context = h.context(caps);
        context.currentAffinity = &current;
        renderCards(context, layoutFor(caps, 1600.0F, 600.0F));
        h.actionsView.renderConfirmation(&h.mock, BLOCK_RENDER_TARGET);
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
    // The CPU layout is read on its worker (#3182): the boxes are there once it has arrived.
    h.actionsView.waitForAffinityLayoutForTesting();
    ProcessAffinityDialog& dialog = h.actionsView.affinityDialog();
    EXPECT_FALSE(dialog.awaitingLayout());
    EXPECT_EQ(dialog.target().pid, BLOCK_RENDER_TARGET.pid);
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
    EXPECT_EQ(h.mock.lastTarget().pid, BLOCK_RENDER_TARGET.pid);
    static_cast<void>(runFrame(body));
    EXPECT_TRUE(text.contains("CPU affinity of victim (PID 1001) set to 0-2")) << text;

    // Opened again, then the selection moves on: closed, nothing applied.
    ImGui::ActivateItemByID(ImHashStr(Detail::SET_AFFINITY_LABEL, 0, actionsWindow()->ID));
    static_cast<void>(runFrame(body));
    ASSERT_TRUE(runFrame(body));
    EXPECT_FALSE(dialog.awaitingLayout()) << "the layout read for the first opening serves the second";
    h.actionsView.onSelectionChanged();
    static_cast<void>(runFrame(body));
    EXPECT_FALSE(runFrame(body));
    EXPECT_EQ(h.mock.setAffinityCount(), 1);
    EXPECT_EQ(h.mock.affinityLayoutCount(), 1) << "read once, for both openings";
    EXPECT_NE(h.mock.affinityLayoutThread(), std::this_thread::get_id()) << "read on its worker, never the UI thread";
}

// #3086: where the platform can set a scheduling policy (Linux), the Actions card has Set scheduling policy...;
// it opens the dialog on the process's policy, Apply with Batch chosen sets it on that process and the card's
// line says so; a refusal for want of permission offers Authenticate and retry, which sends the same policy; and
// a selection change closes the dialog unapplied.
TEST_F(ProcessActionsBlockRenderTest, SetSchedulingPolicyFromTheCardOpensTheDialogAndAppliesToTheProcess)
{
    Harness h;
    Platform::ProcessActionCapabilities caps = BLOCK_RENDER_ACTIONS;
    caps.canSetSchedPolicy = true;
    caps.canAuthorizeInteractively = true;
    h.mock.setCapabilities(caps);
    std::string text;
    const auto body = [&]
    {
        ImGui::LogToBuffer();
        ProcessActionsBlock::Context context = h.context(caps);
        context.currentSchedPolicy = Platform::SchedPolicy::Normal;
        renderCards(context, layoutFor(caps, 1600.0F, 600.0F));
        h.actionsView.renderConfirmation(&h.mock, BLOCK_RENDER_TARGET);
        text = GImGui->LogBuffer.c_str();
        ImGui::LogFinish();
    };
    ASSERT_FALSE(runFrame(body));
    EXPECT_TRUE(text.contains("Set scheduling policy...")) << text;
    const auto pressButton = [&]
    {
        const ImGuiWindow* actions = actionsWindow();
        ASSERT_NE(actions, nullptr);
        ImGui::ActivateItemByID(ImHashStr(Detail::SET_SCHED_POLICY_LABEL, 0, actions->ID));
        static_cast<void>(runFrame(body));
        ASSERT_TRUE(runFrame(body));
    };
    const auto pressInModal = [](const char* label, bool filled)
    {
        const ImGuiWindow* modal = ImGui::GetTopMostPopupModal();
        ASSERT_NE(modal, nullptr);
        const ImGuiID id = ImHashStr(label, 0, modal->ID);
        ImGui::ActivateItemByID(filled ? ImHashStr("##filled", 0, id) : id);
    };
    pressButton();
    ProcessSchedPolicyDialog& dialog = h.actionsView.schedPolicyDialog();
    EXPECT_EQ(dialog.target().pid, BLOCK_RENDER_TARGET.pid);
    EXPECT_EQ(dialog.choice(), Platform::SchedPolicy::Normal);

    pressInModal("Batch", false);
    static_cast<void>(runFrame(body));
    pressInModal("Apply", true);
    for (int i = 0; i < 3 && h.mock.setSchedPolicyCount() == 0; ++i)
    {
        static_cast<void>(runFrame(body));
        h.actionsView.waitForActionForTesting(); // the change runs on the action worker (#2305)
    }
    EXPECT_EQ(h.mock.setSchedPolicyCount(), 1);
    EXPECT_EQ(h.mock.lastSchedPolicy(), Platform::SchedPolicy::Batch);
    EXPECT_EQ(h.mock.lastRealtimeThreads(), Platform::RealtimeThreads::Keep) << "opened on Normal: no warning, none demoted";
    EXPECT_EQ(h.mock.lastTarget().pid, BLOCK_RENDER_TARGET.pid);
    static_cast<void>(runFrame(body));
    EXPECT_TRUE(text.contains("Scheduling policy of victim (PID 1001) set to Batch")) << text;

    // Refused for want of permission: no retry, though the platform could ask for authorization (#3886): the policy
    // is Linux's, and no helper redoes it (#3580).
    h.mock.setSchedPolicyResult(Platform::ProcessActionResult::denied("Permission denied"));
    pressButton();
    pressInModal("Idle", false);
    static_cast<void>(runFrame(body));
    pressInModal("Apply", true);
    for (int i = 0; i < 3 && h.mock.setSchedPolicyCount() == 1; ++i)
    {
        static_cast<void>(runFrame(body));
        h.actionsView.waitForActionForTesting();
    }
    EXPECT_EQ(h.mock.setSchedPolicyCount(), 2);
    static_cast<void>(runFrame(body));
    EXPECT_TRUE(text.contains("Permission denied")) << text;
    EXPECT_FALSE(h.actionsView.offersAuthenticateAndRetry());
    EXPECT_EQ(h.mock.runAuthorizedCount(), 0);

    // Opened again, then the selection moves on: closed, nothing applied.
    pressButton();
    h.actionsView.onSelectionChanged();
    static_cast<void>(runFrame(body));
    EXPECT_FALSE(runFrame(body));
    EXPECT_EQ(h.mock.setSchedPolicyCount(), 2);
}

// #3219: a process already on FIFO opens the card's dialog on FIFO at its own priority; choosing RR and Apply asks
// to confirm, naming the process, and Set sets RR at that priority through the action worker; refused for want of
// permission, no retry is offered (#3886).
TEST_F(ProcessActionsBlockRenderTest, RoundRobinFromTheCardIsSetAfterTheConfirm)
{
    Harness h;
    Platform::ProcessActionCapabilities caps = BLOCK_RENDER_ACTIONS;
    caps.canSetSchedPolicy = true;
    caps.canSetRealtimeSchedPolicy = true;
    caps.canAuthorizeInteractively = true;
    h.mock.setCapabilities(caps);
    h.mock.setSchedPolicyResult(Platform::ProcessActionResult::denied("Permission denied"));
    std::string text;
    const auto body = [&]
    {
        ImGui::LogToBuffer();
        ProcessActionsBlock::Context context = h.context(caps);
        context.currentSchedPolicy = Platform::SchedPolicy::Fifo;
        context.currentRtPriority = 40;
        renderCards(context, layoutFor(caps, 1600.0F, 600.0F));
        h.actionsView.renderConfirmation(&h.mock, BLOCK_RENDER_TARGET);
        text = GImGui->LogBuffer.c_str();
        ImGui::LogFinish();
    };
    ASSERT_FALSE(runFrame(body));
    const auto pressInModal = [](const char* label, bool filled)
    {
        const ImGuiWindow* modal = ImGui::GetTopMostPopupModal();
        ASSERT_NE(modal, nullptr);
        const ImGuiID id = ImHashStr(label, 0, modal->ID);
        ImGui::ActivateItemByID(filled ? ImHashStr("##filled", 0, id) : id);
    };
    const ImGuiWindow* actions = actionsWindow();
    ASSERT_NE(actions, nullptr);
    ImGui::ActivateItemByID(ImHashStr(Detail::SET_SCHED_POLICY_LABEL, 0, actions->ID));
    static_cast<void>(runFrame(body));
    ASSERT_TRUE(runFrame(body));
    ProcessSchedPolicyDialog& dialog = h.actionsView.schedPolicyDialog();
    EXPECT_EQ(dialog.setting(), (SchedPolicySetting{.policy = Platform::SchedPolicy::Fifo, .rtPriority = 40}));
    EXPECT_TRUE(text.contains("RT throttling")) << text;

    pressInModal("RR", false);
    static_cast<void>(runFrame(body));
    pressInModal("Apply", true);
    static_cast<void>(runFrame(body));
    static_cast<void>(runFrame(body));
    EXPECT_TRUE(dialog.confirming());
    EXPECT_TRUE(text.contains("Set victim (PID 1001) to RR 40?")) << text;
    EXPECT_EQ(h.mock.setSchedPolicyCount(), 0) << "nothing is set before the confirm";
    pressInModal("Set", true);
    for (int i = 0; i < 3 && h.mock.setSchedPolicyCount() == 0; ++i)
    {
        static_cast<void>(runFrame(body));
        h.actionsView.waitForActionForTesting();
    }
    EXPECT_EQ(h.mock.setSchedPolicyCount(), 1);
    EXPECT_EQ(h.mock.lastSchedPolicy(), Platform::SchedPolicy::RoundRobin);
    EXPECT_EQ(h.mock.lastRtPriority(), 40);
    static_cast<void>(runFrame(body));
    EXPECT_FALSE(h.actionsView.offersAuthenticateAndRetry());
    EXPECT_EQ(h.mock.runAuthorizedCount(), 0);
}

// #3086: no Set scheduling policy... where the platform can't set one (Windows).
TEST_F(ProcessActionsBlockRenderTest, NoSetSchedulingPolicyWhereThePlatformCantSetOne)
{
    Harness h;
    std::string text;
    (void) runFrame(
        [&]
        {
            ImGui::LogToBuffer();
            renderCards(h.context(BLOCK_RENDER_ACTIONS), layoutFor(BLOCK_RENDER_ACTIONS, 1600.0F, 600.0F));
            text = GImGui->LogBuffer.c_str();
            ImGui::LogFinish();
        });
    EXPECT_FALSE(text.contains("Set scheduling policy")) << text;
    EXPECT_TRUE(text.contains("Suspend")) << text;
}

// #3182: Set affinity... pressed before the CPU layout has been read opens the dialog at once, saying it
// is reading it; the boxes are arranged by the layout when it arrives. Never read on the UI thread.
TEST_F(ProcessActionsBlockRenderTest, SetAffinityBeforeTheLayoutIsReadOpensTheDialogAwaitingIt)
{
    Harness h;
    Platform::CpuAffinityLayout layout;
    layout.online = Platform::CpuAffinity::fromMask(0b1111U);
    h.mock.setAffinityLayout(layout);
    h.mock.holdAffinityLayout();
    Platform::ProcessActionCapabilities caps = BLOCK_RENDER_ACTIONS;
    caps.canSetAffinity = true;
    const Platform::CpuAffinity current = Platform::CpuAffinity::fromMask(0b1U);
    std::string text;
    const auto body = [&]
    {
        ImGui::LogToBuffer();
        ProcessActionsBlock::Context context = h.context(caps);
        context.currentAffinity = &current;
        renderCards(context, layoutFor(caps, 1600.0F, 600.0F));
        h.actionsView.renderConfirmation(&h.mock, BLOCK_RENDER_TARGET);
        text = GImGui->LogBuffer.c_str();
        ImGui::LogFinish();
    };
    ASSERT_FALSE(runFrame(body));
    ImGui::ActivateItemByID(ImHashStr(Detail::SET_AFFINITY_LABEL, 0, actionsWindow()->ID));
    static_cast<void>(runFrame(body));
    ASSERT_TRUE(runFrame(body)) << "up at once, without waiting for the read";
    const ProcessAffinityDialog& dialog = h.actionsView.affinityDialog();
    EXPECT_TRUE(dialog.awaitingLayout());
    EXPECT_TRUE(text.contains("Reading this PC's CPU layout...")) << text;

    h.mock.releaseAffinityLayout();
    h.actionsView.waitForAffinityLayoutForTesting();
    ASSERT_TRUE(runFrame(body));
    EXPECT_FALSE(dialog.awaitingLayout());
    EXPECT_EQ(dialog.cpuCount(), 4U);
    EXPECT_EQ(dialog.choice(), current);
    EXPECT_FALSE(text.contains("Reading this PC's CPU layout...")) << text;
    EXPECT_EQ(h.mock.affinityLayoutCount(), 1);
}

// #3182: the update path starts the layout's read early, so Set affinity... finds it read and the dialog
// opens with its boxes; without actions there is nothing to read.
TEST_F(ProcessActionsBlockRenderTest, TheUpdatePathReadsTheLayoutBeforeSetAffinityIsPressed)
{
    Harness h;
    Platform::CpuAffinityLayout layout;
    layout.online = Platform::CpuAffinity::fromMask(0b111U);
    h.mock.setAffinityLayout(layout);
    h.actionsView.updateAffinityLayout(nullptr);
    EXPECT_EQ(h.mock.affinityLayoutCount(), 0);
    for (int frame = 0; frame < 3; ++frame)
    {
        h.actionsView.updateAffinityLayout(&h.mock); // each frame's update: one read in all
    }
    h.actionsView.waitForAffinityLayoutForTesting(); // no dialog awaits it: kept for the press

    Platform::ProcessActionCapabilities caps = BLOCK_RENDER_ACTIONS;
    caps.canSetAffinity = true;
    const auto body = [&]
    {
        renderCards(h.context(caps), layoutFor(caps, 1600.0F, 600.0F));
        h.actionsView.renderConfirmation(&h.mock, BLOCK_RENDER_TARGET);
    };
    ASSERT_FALSE(runFrame(body));
    ImGui::ActivateItemByID(ImHashStr(Detail::SET_AFFINITY_LABEL, 0, actionsWindow()->ID));
    static_cast<void>(runFrame(body));
    ASSERT_TRUE(runFrame(body));
    const ProcessAffinityDialog& dialog = h.actionsView.affinityDialog();
    EXPECT_FALSE(dialog.awaitingLayout()) << "read already: the boxes are there on the first frame";
    EXPECT_EQ(dialog.cpuCount(), 3U);
    EXPECT_EQ(h.mock.affinityLayoutCount(), 1);
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
        renderCards(h.context(BLOCK_RENDER_ACTIONS), layoutFor(BLOCK_RENDER_ACTIONS, 1600.0F, 600.0F));
        h.actionsView.renderConfirmation(&h.mock, BLOCK_RENDER_TARGET);
        text = GImGui->LogBuffer.c_str();
        ImGui::LogFinish();
    };

    // Cancelled: nothing is done.
    ASSERT_TRUE(h.actionsView.requestKillShortcut(BLOCK_RENDER_ACTIONS, BLOCK_RENDER_TARGET, h.name));
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
    ASSERT_TRUE(h.actionsView.requestKillShortcut(BLOCK_RENDER_ACTIONS, BLOCK_RENDER_TARGET, h.name));
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

// #3071: where the platform has Efficiency mode, the Actions card draws its checkbox, ticked while the process
// is in it, and measures it. Ticking it asks to confirm and the confirm turns it on; unticking it turns it off
// at once. Where the platform has none, nothing is drawn or measured.
TEST_F(ProcessActionsBlockRenderTest, TheEfficiencyModeCheckboxAsksToTurnItOnAndTurnsItOffAtOnce)
{
    constexpr Platform::ProcessActionCapabilities CAPS{
        .canTerminate = true, .canKill = true, .canSetPriority = true, .canSetEfficiencyMode = true};
    Harness h;
    bool on = false;
    std::string text;
    const auto body = [&]
    {
        ImGui::LogToBuffer();
        ProcessActionsBlock::Context context = h.context(CAPS);
        context.efficiencyMode = on;
        renderCards(context, layoutFor(CAPS, 1600.0F, 600.0F));
        h.actionsView.renderConfirmation(&h.mock, BLOCK_RENDER_TARGET);
        text = GImGui->LogBuffer.c_str();
        ImGui::LogFinish();
    };
    float toggleWidth = 0.0F;
    float buttons = 0.0F;
    float withoutToggle = -1.0F;
    (void) runFrame(
        [&]
        {
            toggleWidth = ProcessActionsView::efficiencyModeToggleWidth(CAPS);
            buttons = ProcessActionsBlock::measure(CAPS, false).buttons;
            withoutToggle = ProcessActionsView::efficiencyModeToggleWidth(BLOCK_RENDER_ACTIONS);
        });
    EXPECT_GT(toggleWidth, 0.0F);
    EXPECT_GE(buttons, toggleWidth);
    EXPECT_FLOAT_EQ(withoutToggle, 0.0F);
    EXPECT_TRUE(ProcessActionsBlock::hasAnyAction(Platform::ProcessActionCapabilities{.canSetEfficiencyMode = true}));

    (void) runFrame(body);
    (void) runFrame(body);
    EXPECT_TRUE(text.contains("Efficiency mode")) << text;
    const ImGuiWindow* card = actionsWindow();
    ASSERT_NE(card, nullptr);
    ImGui::ActivateItemByID(ImHashStr(Detail::EFFICIENCY_MODE_LABEL, 0, card->ID));
    (void) runFrame(body);
    ASSERT_TRUE(runFrame(body));
    const ImGuiWindow* modal = ImGui::GetTopMostPopupModal();
    ASSERT_NE(modal, nullptr);
    EXPECT_TRUE(std::string_view(modal->Name).starts_with("Turn on Efficiency mode for victim (PID 1001)?")) << modal->Name;
    EXPECT_EQ(h.mock.efficiencyModeCount(), 0); // asked, not done
    ImGui::ActivateItemByID(ImHashStr("Turn on Efficiency mode", 0, modal->ID));
    (void) runFrame(body);
    h.actionsView.waitForActionForTesting(); // on the action worker (#2305)
    (void) runFrame(body);
    EXPECT_EQ(h.mock.efficiencyModeCount(), 1);
    EXPECT_TRUE(h.mock.lastEfficiencyMode());
    EXPECT_TRUE(text.contains("Efficiency mode on for PID 1001")) << text;

    on = true; // the process's Status says so from the next sample
    (void) runFrame(body);
    ImGui::ActivateItemByID(ImHashStr(Detail::EFFICIENCY_MODE_LABEL, 0, actionsWindow()->ID));
    EXPECT_FALSE(runFrame(body)) << "turning it off isn't confirmed";
    h.actionsView.waitForActionForTesting();
    EXPECT_EQ(h.mock.efficiencyModeCount(), 2);
    EXPECT_FALSE(h.mock.lastEfficiencyMode());
    EXPECT_EQ(h.mock.lastTarget(), BLOCK_RENDER_TARGET);
}

// #3429: where the platform can set Realtime (Windows), the Priority card has Set Realtime... under the slider.
// It asks first, with the warning, in the Actions card's confirm, which then sets Realtime's nice; it is greyed out
// for a process already at Realtime, and not drawn where the platform can't set it.
TEST_F(ProcessActionsBlockRenderTest, SetRealtimeInThePriorityCardWarnsThenSetsRealtime)
{
    constexpr Platform::ProcessActionCapabilities CAPS{
        .canTerminate = true,
        .canKill = true,
        .canSetPriority = true,
        .canSetRealtimePriority = true,
    };
    Harness h;
    std::optional<std::int32_t> currentNice = 0;
    Platform::ProcessActionCapabilities caps = CAPS;
    std::string text;
    const auto body = [&]
    {
        ImGui::LogToBuffer();
        ProcessActionsBlock::Context context = h.context(caps);
        context.currentNice = currentNice;
        renderCards(context, layoutFor(caps, 1600.0F, 600.0F));
        h.actionsView.renderConfirmation(&h.mock, BLOCK_RENDER_TARGET);
        text = GImGui->LogBuffer.c_str();
        ImGui::LogFinish();
    };

    (void) runFrame(body);
    (void) runFrame(body);
    EXPECT_TRUE(text.contains("Set Realtime...")) << text;
    const ImGuiWindow* card = priorityWindow();
    ASSERT_NE(card, nullptr);
    ImGui::ActivateItemByID(ImHashStr(Detail::SET_REALTIME_PRIORITY_BUTTON_LABEL, 0, card->ID));
    (void) runFrame(body);
    ASSERT_TRUE(runFrame(body));
    const ImGuiWindow* modal = ImGui::GetTopMostPopupModal();
    ASSERT_NE(modal, nullptr);
    EXPECT_TRUE(std::string_view(modal->Name).starts_with("Set Realtime priority for victim (PID 1001)?")) << modal->Name;
    EXPECT_TRUE(text.contains("can make the desktop unresponsive")) << text;
    EXPECT_EQ(h.mock.setPriorityCount(), 0);                                                             // asked, not done
    ImGui::ActivateItemByID(ImHashStr("##filled", 0, ImHashStr("Set Realtime priority", 0, modal->ID))); // the danger fill
    (void) runFrame(body);
    h.actionsView.waitForActionForTesting(); // on the action worker (#2305)
    (void) runFrame(body);
    EXPECT_EQ(h.mock.setPriorityCount(), 1);
    EXPECT_EQ(h.mock.lastSetPriorityNice(), Detail::REALTIME_PRIORITY_NICE);
    EXPECT_EQ(h.mock.lastTarget(), BLOCK_RENDER_TARGET);
    EXPECT_TRUE(text.contains("Priority of PID 1001 set to Realtime")) << text;

    // Already at Realtime (the next sample): drawn, greyed out, and a press asks nothing.
    currentNice = Detail::REALTIME_PRIORITY_NICE;
    (void) runFrame(body);
    ImGui::ActivateItemByID(ImHashStr(Detail::SET_REALTIME_PRIORITY_BUTTON_LABEL, 0, priorityWindow()->ID));
    (void) runFrame(body);
    EXPECT_FALSE(runFrame(body)) << "nothing to confirm for a process already at Realtime";

    caps.canSetRealtimePriority = false; // Linux
    (void) runFrame(body);
    EXPECT_FALSE(text.contains("Set Realtime...")) << text;
}

/// Actions whose setPriority waits until the test opens the gate: an apply or a Set Realtime priority still
/// on its worker, as a UAC prompt or the helper's round trip leaves it.
class GatedPriorityActions : public TestMocks::MockProcessActions
{
  public:
    [[nodiscard]] Platform::ProcessActionResult setPriority(const Platform::ProcessTarget& target, std::int32_t nice) override
    {
        m_Gate.wait(false);
        return TestMocks::MockProcessActions::setPriority(target, nice);
    }

    void open()
    {
        m_Gate = true;
        m_Gate.notify_all();
    }

  private:
    std::atomic<bool> m_Gate{false};
};

constexpr Platform::ProcessActionCapabilities REALTIME_CAPS{
    .canTerminate = true,
    .canKill = true,
    .canSetPriority = true,
    .canSetRealtimePriority = true,
};

// #3429 review: while the slider's Apply is still on its worker, Set Realtime... is greyed out, so the two never
// set the same process's priority at once and the older can't land last, overwriting Realtime.
TEST_F(ProcessActionsBlockRenderTest, SetRealtimeWaitsForTheSlidersApply)
{
    Harness h;
    GatedPriorityActions gated;
    const auto body = [&]
    {
        ProcessActionsBlock::Context context = h.context(REALTIME_CAPS);
        context.actions = &gated;
        renderCards(context, layoutFor(REALTIME_CAPS, 1600.0F, 600.0F));
        h.actionsView.renderConfirmation(&gated, BLOCK_RENDER_TARGET);
    };
    h.priorityView.editNice(Domain::Priority::classNice(Domain::Priority::PriorityClass::High), BLOCK_RENDER_TARGET);
    h.priorityView.apply(&gated, BLOCK_RENDER_TARGET, std::optional<std::int32_t>{0});
    ASSERT_TRUE(h.priorityView.applying());

    (void) runFrame(body);
    ImGui::ActivateItemByID(ImHashStr(Detail::SET_REALTIME_PRIORITY_BUTTON_LABEL, 0, priorityWindow()->ID));
    (void) runFrame(body);
    EXPECT_FALSE(runFrame(body)) << "no confirm while the apply runs";
    EXPECT_EQ(h.actionsView.pendingAction(), Detail::ProcessAction::None);

    gated.open();
    h.priorityView.update(/*wait=*/true);
    (void) runFrame(body);
    ImGui::ActivateItemByID(ImHashStr(Detail::SET_REALTIME_PRIORITY_BUTTON_LABEL, 0, priorityWindow()->ID));
    (void) runFrame(body);
    EXPECT_TRUE(runFrame(body)) << "once it has finished, Set Realtime... asks again";
    EXPECT_EQ(h.actionsView.pendingAction(), Detail::ProcessAction::SetRealtimePriority);
}

// #3429 review, the other way: while Set Realtime priority is asked about or still running, the slider's Apply is
// held, and its press sends nothing; once it has finished, Apply works again.
TEST_F(ProcessActionsBlockRenderTest, TheSlidersApplyWaitsForSetRealtime)
{
    Harness h;
    GatedPriorityActions gated;
    const auto body = [&]
    {
        ProcessActionsBlock::Context context = h.context(REALTIME_CAPS);
        context.actions = &gated;
        renderCards(context, layoutFor(REALTIME_CAPS, 1600.0F, 600.0F));
        h.actionsView.renderConfirmation(&gated, BLOCK_RENDER_TARGET);
    };
    const std::int32_t high = Domain::Priority::classNice(Domain::Priority::PriorityClass::High);

    h.actionsView.requestAction(Detail::ProcessAction::SetRealtimePriority, BLOCK_RENDER_TARGET, h.name);
    (void) runFrame(body);
    ASSERT_TRUE(runFrame(body)) << "its confirm is up";
    EXPECT_TRUE(h.priorityView.heldForOtherPriorityChange());

    // Confirmed through the dialog's danger button: on the action worker, held at the gate.
    const ImGuiWindow* modal = ImGui::GetTopMostPopupModal();
    ASSERT_NE(modal, nullptr);
    ImGui::ActivateItemByID(ImHashStr("##filled", 0, ImHashStr("Set Realtime priority", 0, modal->ID)));
    (void) runFrame(body);
    ASSERT_TRUE(h.actionsView.actionRunning());
    (void) runFrame(body);
    EXPECT_TRUE(h.priorityView.heldForOtherPriorityChange()) << "still running";
    h.priorityView.editNice(high, BLOCK_RENDER_TARGET);
    h.priorityView.apply(&gated, BLOCK_RENDER_TARGET, std::optional<std::int32_t>{0});
    EXPECT_FALSE(h.priorityView.applying()) << "Apply sends nothing while Set Realtime priority runs";

    gated.open();
    h.actionsView.waitForActionForTesting();
    EXPECT_EQ(gated.setPriorityCount(), 1);
    EXPECT_EQ(gated.lastSetPriorityNice(), Detail::REALTIME_PRIORITY_NICE);
    (void) runFrame(body);
    EXPECT_FALSE(h.priorityView.heldForOtherPriorityChange());
    h.priorityView.apply(&gated, BLOCK_RENDER_TARGET, std::optional<std::int32_t>{0}); // the edit is still pending
    h.priorityView.update(/*wait=*/true);
    EXPECT_EQ(gated.setPriorityCount(), 2);
    EXPECT_EQ(gated.lastSetPriorityNice(), high);
}

// #3429 review: a refused Set Realtime priority's "Retry as administrator" waits while the slider's Apply runs, and
// is offered again once it has finished.
TEST_F(ProcessActionsBlockRenderTest, SetRealtimesRetryWaitsForTheSlidersApply)
{
    Harness h;
    TestMocks::MockProcessActions refusing;
    Platform::ProcessActionCapabilities caps = REALTIME_CAPS;
    caps.canAuthorizeInteractively = true;
    refusing.setCapabilities(caps);
    refusing.setPriorityResult(Platform::ProcessActionResult::denied("Access denied"));
    GatedPriorityActions gated;
    const auto body = [&]
    {
        ProcessActionsBlock::Context context = h.context(caps);
        context.actions = &refusing;
        renderCards(context, layoutFor(caps, 1600.0F, 600.0F));
    };

    h.actionsView.requestAction(Detail::ProcessAction::SetRealtimePriority, BLOCK_RENDER_TARGET, h.name);
    h.actionsView.dispatchConfirmed(&refusing);
    h.actionsView.waitForActionForTesting();
    (void) runFrame(body);
    ASSERT_TRUE(h.actionsView.offersAuthenticateAndRetry()) << "refused: the retry is offered";

    h.priorityView.editNice(Domain::Priority::classNice(Domain::Priority::PriorityClass::High), BLOCK_RENDER_TARGET);
    h.priorityView.apply(&gated, BLOCK_RENDER_TARGET, std::optional<std::int32_t>{0});
    ASSERT_TRUE(h.priorityView.applying());
    (void) runFrame(body);
    EXPECT_FALSE(h.actionsView.offersAuthenticateAndRetry()) << "held while the apply runs";
    h.actionsView.retryAuthorizedForTesting();
    EXPECT_FALSE(h.actionsView.actionRunning()) << "a press sends nothing meanwhile";
    EXPECT_EQ(refusing.runAuthorizedCount(), 0);

    gated.open();
    h.priorityView.update(/*wait=*/true);
    (void) runFrame(body);
    EXPECT_TRUE(h.actionsView.offersAuthenticateAndRetry()) << "offered again once it has finished";
}

// Any other action running (a Kill) doesn't hold Apply: only a priority change would race it.
TEST(ProcessActionsViewInFlightTest, OnlySetRealtimePriorityIsAPriorityChangeInFlight)
{
    ProcessActionsView view;
    EXPECT_FALSE(view.priorityChangeInFlight());
    view.requestAction(Detail::ProcessAction::Kill, BLOCK_RENDER_TARGET, "victim");
    EXPECT_FALSE(view.priorityChangeInFlight());
    view.cancelConfirm();
    view.requestAction(Detail::ProcessAction::SetRealtimePriority, BLOCK_RENDER_TARGET, "victim");
    EXPECT_TRUE(view.priorityChangeInFlight());
    view.cancelConfirm();
    EXPECT_FALSE(view.priorityChangeInFlight());

    const Detail::RealtimePriorityItem item =
        Detail::realtimePriorityItem(REALTIME_CAPS, BLOCK_RENDER_TARGET, false, false, 0, /*priorityApplying=*/true);
    EXPECT_EQ(item.state, Detail::RealtimePriorityState::Disabled);
    EXPECT_TRUE(std::string_view(item.tooltip).contains("priority change being applied")) << item.tooltip;
}

// #1395: each action button explains itself on hover; Kill names its key.
TEST_F(ProcessActionsBlockRenderTest, TheActionButtonsExplainThemselvesOnHover)
{
    Harness h;
    std::string seen;
    const auto body = [&]
    {
        ImGui::LogToBuffer();
        renderCards(h.context(BLOCK_RENDER_ACTIONS), layoutFor(BLOCK_RENDER_ACTIONS, 1600.0F, 600.0F));
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
        renderCards(h.context(BLOCK_RENDER_ACTIONS), layoutFor(BLOCK_RENDER_ACTIONS, 1000.0F, 0.0F));
        const ImGuiWindow* card = actionsWindow();
        cardSkipped = card == nullptr || card->SkipItems;
        ImGui::EndChild();
        h.actionsView.renderConfirmation(&h.mock, BLOCK_RENDER_TARGET);
        if (const ImGuiWindow* modal = ImGui::GetTopMostPopupModal(); modal != nullptr)
        {
            modalName = modal->Name;
        }
    };
    (void) runFrame(body); // Lays the Overview out, so the next frames know the card is clipped
    ASSERT_FALSE(runFrame(body));

    ASSERT_TRUE(h.actionsView.requestKillShortcut(BLOCK_RENDER_ACTIONS, BLOCK_RENDER_TARGET, h.name)); // F9
    EXPECT_TRUE(runFrame(body));
    EXPECT_TRUE(runFrame(body));
    EXPECT_TRUE(cardSkipped) << "the card was drawn, so this does not test the scrolled-out case";
    EXPECT_TRUE(modalName.starts_with("Kill")) << modalName;
    EXPECT_EQ(h.mock.killCount(), 0);
}

// --- Priority and I/O priority cards, and their heights (#1874, #3741) -----------------------------

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
        renderCards(h.context(PRIORITY_ONLY_ROW), layoutFor(PRIORITY_ONLY_ROW, 1900.0F));
        text = GImGui->LogBuffer.c_str();
        ImGui::LogFinish();
    };
    (void) runFrame(body);
    (void) runFrame(body);
    EXPECT_EQ(occurrences(text, "Priority"), 1U) << text; // The header's

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

TEST_F(ProcessActionsBlockRenderTest, PriorityAndIoPriorityAreSeparateCardsThatFlowSideBySide)
{
    // #3741: the I/O priority control is a card of its own, after Priority. #4029: the two flow like every other
    // card, side by side where they fit, I/O priority to the right of Priority.
    Harness h;
    std::string text;
    const auto body = [&]
    {
        ImGui::LogToBuffer();
        renderCards(h.context(WITH_IO_PRIORITY), layoutFor(WITH_IO_PRIORITY, 1600.0F, 0.0F));
        text = GImGui->LogBuffer.c_str();
        ImGui::LogFinish();
    };
    for (int frame = 0; frame < 3; ++frame)
    {
        (void) runFrame(body);
    }
    EXPECT_EQ(occurrences(text, "Priority"), 1U) << text;     // The Priority card's header, and no row label
    EXPECT_EQ(occurrences(text, "I/O priority"), 1U) << text; // The I/O priority card's header
    EXPECT_LT(text.find("Priority"), text.find("I/O priority")) << text;

    const ImGuiWindow* priority = priorityWindow();
    const ImGuiWindow* io = ioPriorityWindow();
    ASSERT_NE(priority, nullptr);
    ASSERT_NE(io, nullptr);
    // Side by side: the same top edge and width, I/O priority to the right of Priority, the row one band.
    EXPECT_FLOAT_EQ(io->Pos.y, priority->Pos.y);
    EXPECT_NEAR(io->Size.x, priority->Size.x, 1.0F);
    EXPECT_GT(io->Pos.x, priority->Pos.x + priority->Size.x);
    EXPECT_NEAR(io->Size.y, priority->Size.y, 1.0F);
    for (const ImGuiWindow* card : {priority, io})
    {
        EXPECT_FLOAT_EQ(card->ScrollMax.x, 0.0F);
        EXPECT_FLOAT_EQ(card->ScrollMax.y, 0.0F);
    }
    // Hovering the I/O priority header shows the current I/O priority, as the row label did: unknown here,
    // since nothing has read it.
    ImGui::GetIO().AddMousePosEvent(io->Pos.x + io->WindowPadding.x + 4.0F,
                                    io->Pos.y + io->WindowPadding.y + (ImGui::GetTextLineHeight() * 0.5F));
    std::string hovered;
    for (int frame = 0; frame < 4; ++frame) // The tooltip waits for the hover delay
    {
        ImGui::GetIO().DeltaTime = 0.5F;
        (void) runFrame(body);
        hovered += text;
    }
    EXPECT_TRUE(hovered.contains("Current I/O priority: unknown")) << hovered;
}

TEST_F(ProcessActionsBlockRenderTest, TheActionsCardMeasuresTheSameContentBesideTheRowOrWrapped)
{
    // #1874 centred the Actions card's one row of buttons in a card as tall as Identity and Runtime. #4029: the
    // card is drawn as tall as its row, and what it measures, and feeds back as its own height, is its content's,
    // wherever it sits, so a row never grows from its own stretch.
    Harness beside;
    ASSERT_TRUE(renderBlock(beside, PRIORITY_ONLY_ROW, 1900.0F, INFO_ROW).onFirstRow(OverviewCard::Actions));
    const float besideContent = measuredHeights().at(static_cast<std::size_t>(OverviewCard::Actions));
    EXPECT_GT(besideContent, 0.0F);
    EXPECT_LT(besideContent, infoCardHeight(8.0F)); // Shorter than an Identity card of eight rows

    Harness wrapped;
    ASSERT_FALSE(renderBlock(wrapped, PRIORITY_ONLY_ROW, 780.0F, INFO_ROW).onFirstRow(OverviewCard::Actions));
    EXPECT_NEAR(measuredHeights().at(static_cast<std::size_t>(OverviewCard::Actions)), besideContent, 1.0F);
}

TEST_F(ProcessActionsBlockRenderTest, StackedInANarrowPaneEachCardIsAsTallAsItsContent)
{
    Harness h;
    const auto layout = renderBlock(h, WITH_IO_PRIORITY, 120.0F, INFO_ROW);
    ASSERT_FALSE(layout.sameRow(OverviewCard::Actions, OverviewCard::Priority));
    for (const ImGuiWindow* card : {actionsWindow(), priorityWindow(), ioPriorityWindow()})
    {
        ASSERT_NE(card, nullptr);
        EXPECT_NEAR(spareHeight(*card), 0.0F, 1.0F);
    }
}

TEST_F(ProcessActionsBlockRenderTest, EachCardGrowsWhileItsErrorLineShowsAndShrinksBackAfter)
{
    // #3741: no fixed height and no line held for a result. A failed apply's error line makes its card that
    // much taller, and the other card keeps its height; once the line is gone the card is its old height.
    Harness h;
    constexpr Platform::IoPriority BEST_EFFORT_4{.ioClass = Platform::IoPriorityClass::BestEffort, .level = 4};
    h.mock.setIoPriorityReadResult(BEST_EFFORT_4);
    h.mock.setIoPriorityResult(Platform::ProcessActionResult::error("Permission denied"));
    h.mock.setPriorityResult(Platform::ProcessActionResult::error("Operation not permitted"));
    const auto body = [&]
    {
        // As the panel's update path does (#2734): the I/O priority read on its worker, waited for here.
        (void) h.priorityView.updateReads(&h.mock, BLOCK_RENDER_TARGET, 1.0F / 60.0F, /*wait=*/true);
        renderCards(h.context(WITH_IO_PRIORITY), layoutFor(WITH_IO_PRIORITY, 1000.0F));
    };
    const auto settle = [&]
    {
        for (int frame = 0; frame < 3; ++frame)
        {
            (void) runFrame(body);
        }
    };
    settle();
    ASSERT_EQ(h.priorityView.ioPriorityView().currentIoPriority(), std::optional<Platform::IoPriority>{BEST_EFFORT_4});
    const ImGuiWindow* priority = priorityWindow();
    const ImGuiWindow* io = ioPriorityWindow();
    ASSERT_NE(priority, nullptr);
    ASSERT_NE(io, nullptr);
    const float priorityHeight = priority->Size.y;
    const float ioHeight = io->Size.y;

    // The I/O priority card's apply fails: its error line shows, and only its card grows.
    h.priorityView.ioPriorityView().editIoPriority({.ioClass = Platform::IoPriorityClass::Idle, .level = 0}, BLOCK_RENDER_TARGET);
    h.priorityView.ioPriorityView().apply(&h.mock, BLOCK_RENDER_TARGET);
    h.priorityView.update(/*wait=*/true);
    ASSERT_EQ(h.priorityView.ioPriorityView().error(), "Permission denied");
    settle();
    EXPECT_GE(io->Size.y, ioHeight + ImGui::GetTextLineHeight());
    EXPECT_NEAR(spareHeight(*io), 0.0F, 1.0F);
    EXPECT_FLOAT_EQ(priority->Size.y, priorityHeight);

    // The Priority card's too.
    h.priorityView.editNice(10, BLOCK_RENDER_TARGET);
    h.priorityView.apply(&h.mock, BLOCK_RENDER_TARGET, std::optional<std::int32_t>{0});
    h.priorityView.update(/*wait=*/true);
    ASSERT_EQ(h.priorityView.error(), "Operation not permitted");
    settle();
    EXPECT_GE(priority->Size.y, priorityHeight + ImGui::GetTextLineHeight());
    EXPECT_NEAR(spareHeight(*priority), 0.0F, 1.0F);

    // A selection change clears both lines, and both cards are their old heights again.
    h.priorityView.onSelectionChanged();
    settle();
    ASSERT_TRUE(h.priorityView.error().empty());
    ASSERT_TRUE(h.priorityView.ioPriorityView().error().empty());
    EXPECT_NEAR(priority->Size.y, priorityHeight, 1.0F);
    EXPECT_NEAR(io->Size.y, ioHeight, 1.0F);
}

// --- Trace system calls (#182) ---------------------------------------------------------------------

/// The four buttons and the priority row with the trace button in @p availability.
constexpr Platform::ProcessActionCapabilities withTrace(Platform::SyscallTraceAvailability availability)
{
    Platform::ProcessActionCapabilities caps = BLOCK_RENDER_ACTIONS;
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
            rowOnly = ProcessActionsBlock::measure(withTrace(Platform::SyscallTraceAvailability::Unsupported), false).buttons;
            traceWidth = ProcessActionsView::syscallTraceButtonWidth(withTrace(Platform::SyscallTraceAvailability::Available));
            available = ProcessActionsBlock::measure(withTrace(Platform::SyscallTraceAvailability::Available), false).buttons;
            greyedOut = ProcessActionsBlock::measure(withTrace(Platform::SyscallTraceAvailability::NoTracer), false).buttons;
            // A platform whose only button is the trace one: the Actions card is as wide as that button.
            Platform::ProcessActionCapabilities traceOnly;
            traceOnly.syscallTrace = Platform::SyscallTraceAvailability::Available;
            traceOnlyButtons = ProcessActionsBlock::measure(traceOnly, false).buttons;
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
        const auto layout = renderBlock(h, CAPS, paneWidth, INFO_ROW);
        for (const ImGuiWindow* card : {actionsWindow(), priorityWindow()})
        {
            ASSERT_NE(card, nullptr);
            // Nothing runs out of a card or makes it scroll, sideways or down: the button followed the
            // row or started its own, and the row is as tall as its tallest card (#1789).
            EXPECT_FLOAT_EQ(card->ScrollMax.x, 0.0F);
            EXPECT_FLOAT_EQ(card->ScrollMax.y, 0.0F);
        }
        EXPECT_LE(actionsWindow()->ContentSize.x, layout[OverviewCard::Actions].width - (2.0F * ImGui::GetStyle().WindowPadding.x) + 1.0F);
        EXPECT_EQ(h.mock.syscallTraceCount(), 0); // Drawing it launches nothing
    }
}

TEST_F(ProcessActionsBlockRenderTest, TheTraceButtonAddsARowOnlyWhereItIsShown)
{
    // The Actions card's own content grows by the trace button's row on Linux and not at all where it is hidden.
    // Its drawn height is its row's (#4029), so its measured content is what's compared.
    const auto actionsContent = []
    {
        return measuredHeights().at(static_cast<std::size_t>(OverviewCard::Actions));
    };
    Harness hidden;
    (void) renderBlock(hidden, withTrace(Platform::SyscallTraceAvailability::Unsupported), 1000.0F, INFO_ROW);
    const float hiddenHeight = actionsContent();
    Harness shown;
    (void) renderBlock(shown, withTrace(Platform::SyscallTraceAvailability::Available), 1000.0F, INFO_ROW);
    const float shownHeight = actionsContent();
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

/// Identity and Runtime as ProcessDetailsPanel draws them, @p cardWidth wide, then the Actions,
/// Priority and I/O priority cards laid out for @p paneWidth, then a one-cell chart grid as CPU Cores
/// draws its cells, for comparison. Returns the cards' layout.
ProcessDetailsLayout::OverviewLayout renderOverviewRow(ProcessActionsBlock::Context context, float paneWidth, float cardWidth)
{
    // ProcessDetailsPanel's cache: the cards' measured heights, carried from frame to frame (#4029).
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float infoHeight = infoCardHeight(6.0F);
    const auto layout = layoutFor(context.capabilities, paneWidth, cardWidth + spacing + cardWidth);
    const auto rows = []
    {
        for (int row = 0; row < 6; ++row)
        {
            ImGui::TextUnformatted("Label  value");
        }
    };
    // As ProcessDetailsPanel draws them (#4029): Identity and Runtime by their rows, each row as tall as its tallest.
    measuredHeights().at(static_cast<std::size_t>(OverviewCard::Identity)) = infoHeight;
    measuredHeights().at(static_cast<std::size_t>(OverviewCard::Runtime)) = infoHeight;
    const auto heights = ProcessDetailsLayout::rowHeights(layout, measuredHeights());
    (void) ProcessOverviewCard::render("BasicInfoLeft",
                                       ICON_FA_ID_CARD,
                                       "Identity",
                                       ImVec2(layout[OverviewCard::Identity].width, std::max(infoHeight, heights.at(0))),
                                       ImGuiChildFlags_None,
                                       rows);
    if (layout[OverviewCard::Runtime].sameLine)
    {
        ImGui::SameLine();
    }
    (void) ProcessOverviewCard::render("BasicInfoRight",
                                       ICON_FA_CLOCK,
                                       "Runtime",
                                       ImVec2(layout[OverviewCard::Runtime].width, std::max(infoHeight, heights.at(1))),
                                       ImGuiChildFlags_None,
                                       rows);
    ProcessActionsBlock::render(context, layout, heights, measuredHeights());

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
        ProcessDetailsLayout::OverviewLayout layout;
        const auto body = [&]
        {
            layout = renderOverviewRow(h.context(WITH_IO_PRIORITY), beside ? 1900.0F : 1000.0F, 384.0F);
        };
        (void) runFrame(body);
        (void) runFrame(body);
        ASSERT_EQ(layout.onFirstRow(OverviewCard::Actions), beside);

        const ImGuiWindow* cell = childWindow("GridCell");
        ASSERT_NE(cell, nullptr);
        const CardLook cellLook = cardLookOf(*cell);
        ASSERT_TRUE(cellLook.bordered);
        ASSERT_TRUE(cellLook.neverScrolls);
        ASSERT_FALSE(cellLook.drawnInTheSentinelColours);

        for (const std::string_view id :
             {"BasicInfoLeft", "BasicInfoRight", "ProcessActionsBlock", "ProcessPriorityBlock", "ProcessIoPriorityBlock"})
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

TEST_F(ProcessActionsBlockRenderTest, TheCardsFlowLeftToRightAndEachRowIsOneBand)
{
    // #4029, the issue's window: Identity, Runtime, Actions and Priority fit on the first row; I/O priority
    // doesn't, so it starts the next row at the left edge. Every card on a row is the row's tallest card's height.
    Harness h;
    ProcessDetailsLayout::OverviewLayout layout;
    const auto body = [&]
    {
        layout = renderOverviewRow(h.context(WITH_IO_PRIORITY), 1900.0F, 384.0F);
    };
    // The first frame measures the cards, the second sizes the rows, the third settles.
    for (int frame = 0; frame < 3; ++frame)
    {
        (void) runFrame(body);
    }
    ASSERT_TRUE(layout.onFirstRow(OverviewCard::Actions));
    ASSERT_TRUE(layout.onFirstRow(OverviewCard::Priority));
    ASSERT_FALSE(layout.onFirstRow(OverviewCard::IoPriority));

    const ImGuiWindow* identity = childWindow("BasicInfoLeft");
    const ImGuiWindow* runtime = childWindow("BasicInfoRight");
    const ImGuiWindow* actions = actionsWindow();
    const ImGuiWindow* priority = priorityWindow();
    const ImGuiWindow* io = ioPriorityWindow();
    ASSERT_NE(identity, nullptr);
    ASSERT_NE(runtime, nullptr);
    ASSERT_NE(actions, nullptr);
    ASSERT_NE(priority, nullptr);
    ASSERT_NE(io, nullptr);
    // The first row: one top edge, one height, the tallest card's (at least Identity's rows).
    for (const ImGuiWindow* card : {runtime, actions, priority})
    {
        EXPECT_FLOAT_EQ(card->Pos.y, identity->Pos.y);
        EXPECT_NEAR(card->Size.y, identity->Size.y, 1.0F);
        EXPECT_FLOAT_EQ(card->ScrollMax.y, 0.0F);
    }
    EXPECT_GE(identity->Size.y, infoCardHeight(6.0F));
    // Left to right, none overlapping.
    EXPECT_GT(runtime->Pos.x, identity->Pos.x + identity->Size.x);
    EXPECT_GT(actions->Pos.x, runtime->Pos.x + runtime->Size.x);
    EXPECT_GT(priority->Pos.x, actions->Pos.x + actions->Size.x);
    // I/O priority starts the second row, at the left edge, below the first.
    EXPECT_FLOAT_EQ(io->Pos.x, identity->Pos.x);
    EXPECT_GT(io->Pos.y, identity->Pos.y + identity->Size.y);
    EXPECT_NEAR(spareHeight(*io), 0.0F, 1.0F) << "alone on its row, it is as tall as its own content";
    // The header is inside each card, at its top (as a core's name heads its grid cell), so nothing
    // of the section is drawn above the card's top edge.
    EXPECT_FLOAT_EQ(identity->Pos.y, ImGui::GetStyle().WindowPadding.y);
}

TEST_F(ProcessActionsBlockRenderTest, WrappedBelowTheRowTheCardsStartARowOfTheirOwn)
{
    // #4029: too narrow for Actions beside Identity and Runtime, so it starts the second row at the left edge;
    // the first row keeps Identity's own height, since nothing on it is taller.
    Harness h;
    ProcessDetailsLayout::OverviewLayout layout;
    const auto body = [&]
    {
        layout = renderOverviewRow(h.context(BLOCK_RENDER_ACTIONS), 780.0F, 384.0F);
    };
    for (int frame = 0; frame < 3; ++frame)
    {
        (void) runFrame(body);
    }
    ASSERT_FALSE(layout.onFirstRow(OverviewCard::Actions));

    const ImGuiWindow* identity = childWindow("BasicInfoLeft");
    const ImGuiWindow* actions = actionsWindow();
    const ImGuiWindow* priority = priorityWindow();
    ASSERT_NE(identity, nullptr);
    ASSERT_NE(actions, nullptr);
    ASSERT_NE(priority, nullptr);
    EXPECT_FLOAT_EQ(identity->Size.y, infoCardHeight(6.0F));
    EXPECT_GE(actions->Pos.y, identity->Pos.y + identity->Size.y);
    EXPECT_FLOAT_EQ(actions->Pos.x, identity->Pos.x);
    EXPECT_NEAR(actions->Size.x, layout[OverviewCard::Actions].width, 1.0F);
    for (const ImGuiWindow* card : {actions, priority})
    {
        EXPECT_FLOAT_EQ(card->ScrollMax.y, 0.0F);
    }
    if (layout.sameRow(OverviewCard::Actions, OverviewCard::Priority))
    {
        EXPECT_FLOAT_EQ(priority->Pos.y, actions->Pos.y);
        EXPECT_NEAR(priority->Size.y, actions->Size.y, 1.0F) << "one band";
    }
}

TEST_F(ProcessActionsBlockRenderTest, CreateDumpFromTheCardConfirmsWritesAndNamesTheFile)
{
    Harness h;
    Platform::ProcessActionCapabilities caps = BLOCK_RENDER_ACTIONS;
    std::string text;
    const auto body = [&]
    {
        ImGui::LogToBuffer();
        renderCards(h.context(caps), layoutFor(caps, 1600.0F, 600.0F));
        h.actionsView.renderConfirmation(&h.mock, BLOCK_RENDER_TARGET);
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
            withoutDump = ProcessActionsBlock::measure(caps, false).buttons;
            Platform::ProcessActionCapabilities dumpCaps = caps;
            dumpCaps.canCreateDump = true;
            withDump = ProcessActionsBlock::measure(dumpCaps, false).buttons;
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
    EXPECT_FALSE(h.actionsView.requestKillShortcut(caps, BLOCK_RENDER_TARGET, h.name));

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
    EXPECT_EQ(h.mock.lastTarget(), BLOCK_RENDER_TARGET);
    EXPECT_EQ(h.mock.lastDumpDirectory(), DumpDetail::defaultDumpDirectory(false));
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

// #3094: Look up on VirusTotal is on the card only with a key, a client and a platform that can hash; pressed, it
// hashes the shown process's image and asks once, on the card's worker, and the result line says what came back.
TEST_F(ProcessActionsBlockRenderTest, LookUpOnVirusTotalFromTheCardHashesAndShowsTheVerdict)
{
    Harness h;
    Platform::ProcessActionCapabilities caps = BLOCK_RENDER_ACTIONS;
    std::string text;
    const auto body = [&]
    {
        ImGui::LogToBuffer();
        renderCards(h.context(caps), layoutFor(caps, 1600.0F, 600.0F));
        h.actionsView.renderConfirmation(&h.mock, BLOCK_RENDER_TARGET);
        text = GImGui->LogBuffer.c_str();
        ImGui::LogFinish();
    };
    const auto client = std::make_shared<ProcessActionsBlockRenderTestDetail::FakeLookupClient>(Platform::HttpsResponse{
        .body = R"({"data":{"attributes":{"last_analysis_stats":{"malicious":2,"suspicious":0,"undetected":60,"harmless":0}}}})",
        .status = 200,
    });
    h.actionsView.setVirusTotal({.client = client, .apiKey = std::string(64, 'a'), .userAgent = "UA"});

    // Not offered where the platform can't hash the image (canHashImage false).
    ASSERT_FALSE(runFrame(body));
    EXPECT_FALSE(text.contains(VirusTotalLookup::MENU_LABEL)) << text;

    caps.canHashImage = true;
    h.mock.setImageSha256Result(std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    ASSERT_FALSE(runFrame(body));
    EXPECT_TRUE(text.contains(VirusTotalLookup::MENU_LABEL)) << text;

    ImGui::ActivateItemByID(ImHashStr(VirusTotalLookup::MENU_LABEL, 0, actionsWindow()->ID));
    (void) runFrame(body);
    h.actionsView.waitForActionForTesting();
    (void) runFrame(body);
    EXPECT_EQ(h.mock.imageSha256Count(), 1);
    EXPECT_EQ(client->requestCount(), 1U);
    EXPECT_TRUE(text.contains("2 of 62 engines flag")) << text;

    // Without a key the button goes.
    h.actionsView.setVirusTotal({.client = client, .apiKey = {}, .userAgent = "UA"});
    (void) runFrame(body);
    EXPECT_FALSE(text.contains(VirusTotalLookup::MENU_LABEL)) << text;
}

// #3813: the card is measured for Look up on VirusTotal where it's offered, as for the other buttons after the
// row, so the button never runs out of the card; where it isn't offered it adds nothing.
TEST_F(ProcessActionsBlockRenderTest, MeasureCountsTheVirusTotalButtonWhereItIsOffered)
{
    Platform::ProcessActionCapabilities caps = NO_PRIORITY;
    caps.canHashImage = true;
    float rowOnly = 0.0F;
    float withVirusTotal = 0.0F;
    float button = 0.0F;
    float buttonOnly = 0.0F;
    (void) runFrame(
        [&]
        {
            rowOnly = ProcessActionsBlock::measure(caps, false).buttons;
            withVirusTotal = ProcessActionsBlock::measure(caps, true).buttons;
            button = ProcessActionsView::virusTotalButtonWidth(true);
            // A platform whose only button is the lookup: the Actions card is as wide as that button.
            buttonOnly = ProcessActionsBlock::measure(Platform::ProcessActionCapabilities{.canHashImage = true}, true).buttons;
        });
    EXPECT_FLOAT_EQ(ProcessActionsView::virusTotalButtonWidth(false), 0.0F);
    EXPECT_GT(button, 0.0F);
    EXPECT_FLOAT_EQ(withVirusTotal, std::max(rowOnly, button));
    EXPECT_FLOAT_EQ(buttonOnly, button);

    // Offered only with a client and a key, which setVirusTotal() gives the view.
    Harness h;
    EXPECT_FALSE(h.actionsView.virusTotalOffered(caps));
    h.actionsView.setVirusTotal({
        .client = std::make_shared<ProcessActionsBlockRenderTestDetail::FakeLookupClient>(Platform::HttpsResponse{}),
        .apiKey = std::string(64, 'a'),
        .userAgent = "UA",
    });
    EXPECT_TRUE(h.actionsView.virusTotalOffered(caps));

    // Laid out as ProcessDetailsPanel does it, wide or narrow, the button fits the card.
    for (const float paneWidth : {1900.0F, 1000.0F, 300.0F})
    {
        SCOPED_TRACE(paneWidth);
        const auto layout = renderBlock(h, caps, paneWidth, INFO_ROW);
        const ImGuiWindow* card = actionsWindow();
        ASSERT_NE(card, nullptr);
        EXPECT_FLOAT_EQ(card->ScrollMax.x, 0.0F);
        EXPECT_LE(card->ContentSize.x, layout[OverviewCard::Actions].width - (2.0F * ImGui::GetStyle().WindowPadding.x) + 1.0F);
    }
}

// #3084: Analyze wait chain... where the platform can read one, at its own width; pressing it opens the wait
// chain dialog for the process shown, which reads its chains on its worker and shows them.
TEST_F(ProcessActionsBlockRenderTest, AnalyzeWaitChainFromTheCardOpensTheDialogForTheProcess)
{
    Harness h;
    Platform::ProcessActionCapabilities caps = BLOCK_RENDER_ACTIONS;
    std::string text;
    const auto body = [&]
    {
        ImGui::LogToBuffer();
        renderCards(h.context(caps), layoutFor(caps, 1600.0F, 600.0F));
        h.actionsView.renderConfirmation(&h.mock, BLOCK_RENDER_TARGET);
        text = GImGui->LogBuffer.c_str();
        ImGui::LogFinish();
    };

    ASSERT_FALSE(runFrame(body));
    EXPECT_FALSE(text.contains("Analyze wait chain")) << "not offered where the platform can't: " << text;
    float without = 0.0F;
    float with = 0.0F;
    float button = 0.0F;
    (void) runFrame(
        [&]
        {
            without = ProcessActionsBlock::measure(caps, false).buttons;
            Platform::ProcessActionCapabilities waitCaps = caps;
            waitCaps.canAnalyzeWaitChain = true;
            with = ProcessActionsBlock::measure(waitCaps, false).buttons;
            button = ProcessActionsView::analyzeWaitChainButtonWidth(waitCaps);
        });
    EXPECT_FLOAT_EQ(ProcessActionsView::analyzeWaitChainButtonWidth(caps), 0.0F);
    EXPECT_GT(button, 0.0F);
    EXPECT_FLOAT_EQ(with, std::max(without, button));

    caps.canAnalyzeWaitChain = true;
    EXPECT_TRUE(ProcessActionsBlock::hasAnyAction(Platform::ProcessActionCapabilities{.canAnalyzeWaitChain = true}));
    Platform::WaitChainReadResult read;
    read.status = Platform::WaitChainReadStatus::Ok;
    read.threadCount = 1;
    Platform::ThreadWaitChain running;
    running.tid = 55;
    Platform::WaitChainNode self;
    self.type = Platform::WaitObjectType::Thread;
    self.status = Platform::WaitNodeStatus::Running;
    running.nodes = {self};
    read.threads.push_back(running);
    h.mock.setWaitChainResult(read);
    ASSERT_FALSE(runFrame(body));
    EXPECT_TRUE(text.contains("Analyze wait chain...")) << text;

    ImGui::ActivateItemByID(ImHashStr(WaitChainDetail::ANALYZE_WAIT_CHAIN_LABEL, 0, actionsWindow()->ID));
    (void) runFrame(body);
    EXPECT_TRUE(h.actionsView.waitChainDialog().isOpen());
    EXPECT_EQ(h.actionsView.waitChainDialog().target(), BLOCK_RENDER_TARGET);
    h.actionsView.waitChainDialog().update(true);
    (void) runFrame(body);
    (void) runFrame(body);
    EXPECT_TRUE(text.contains("Thread 55: Running")) << text;
    EXPECT_EQ(h.mock.waitChainCount(), 1);
    EXPECT_EQ(h.mock.lastTarget(), BLOCK_RENDER_TARGET);
    h.actionsView.shutdownWaitChainDialog();
    EXPECT_FALSE(h.actionsView.waitChainDialog().isOpen());
}

// #2001: the dump's confirm warns when the folder's drive is too full, and offers Save dump to... where
// the platform has a folder dialog; pressing it opens that dialog and leaves the confirm up.
TEST_F(ProcessActionsBlockRenderTest, TheDumpConfirmWarnsOfLowSpaceAndOffersAFolder)
{
    Harness h;
    Platform::ProcessActionCapabilities caps = BLOCK_RENDER_ACTIONS;
    caps.canCreateDump = true;
    caps.canChooseDumpFolder = true;
    h.mock.setCapabilities(caps);
    // Too little room for a 3 GiB process; and the folder dialog is cancelled (the mock's default), so
    // no folder is remembered in the config.
    constexpr std::uint64_t BLOCK_RENDER_GIB = std::uint64_t{1024} * 1024U * 1024U;
    h.mock.setDumpSpace({.freeBytes = BLOCK_RENDER_GIB, .processBytes = 3 * BLOCK_RENDER_GIB});
    std::string text;
    const auto body = [&]
    {
        ImGui::LogToBuffer();
        renderCards(h.context(caps), layoutFor(caps, 1600.0F, 600.0F));
        h.actionsView.renderConfirmation(&h.mock, BLOCK_RENDER_TARGET);
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
    EXPECT_EQ(h.mock.lastFolderInitial(), DumpDetail::defaultDumpDirectory(false));
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
    Platform::ProcessActionCapabilities caps = BLOCK_RENDER_ACTIONS;
    caps.canCreateDump = true;
    caps.canChooseDumpFolder = true;
    mock.setCapabilities(caps);
    const ProcessActionsBlock::Context context{
        .actionsView = &actionsView,
        .priorityView = &priorityView,
        .actions = &mock,
        .capabilities = caps,
        .processName = &name,
        .target = BLOCK_RENDER_TARGET,
        .currentNice = std::optional<std::int32_t>{0},
    };
    const auto frame = [&]
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
        ImGui::SetNextWindowSize(ImVec2(1600.0F, 1000.0F));
        ImGui::Begin("Details", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
        renderCards(context, layoutFor(caps, 1600.0F, 600.0F));
        actionsView.renderConfirmation(&mock, BLOCK_RENDER_TARGET);
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
