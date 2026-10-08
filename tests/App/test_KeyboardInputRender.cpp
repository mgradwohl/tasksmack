/// @file test_KeyboardInputRender.cpp
/// @brief The keyboard shortcuts' real ImGui adapter (App/KeyboardInput.cpp), headless (#160, #170):
/// F9 opens the Actions view's Kill confirm and kills nothing; keys pressed while a text field has the
/// keyboard do nothing; and the navigation keys move the selection in a small scrolling table the way
/// the Processes table uses them, without ImGui's own keyboard navigation also moving.

#include "App/KeyboardInput.h"
#include "App/KeyboardShortcuts.h"
#include "App/Panels/ProcessActionsView.h"
#include "App/Panels/ProcessDetailsPanel_ActionHelpers.h"
#include "App/Panels/ProcessTableNavigation.h"
#include "Mocks/MockProbes.h"
#include "Platform/IProcessActions.h"

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h> // ImGuiContext::NavId: which item ImGui's own keyboard navigation is on

#include <cstddef>
#include <functional>
#include <optional>
#include <string>

#include <misc/cpp/imgui_stdlib.h>

namespace App
{
namespace
{

using KeyboardShortcuts::ShortcutAction;
using ProcessTableNavigation::NavCommand;

constexpr const char* CONFIRM_POPUP_ID = "###ConfirmAction"; // ProcessActionConfirm's modal
constexpr Platform::ProcessTarget TARGET{.pid = 4242, .startTimeTicks = 777};
constexpr Platform::ProcessActionCapabilities CAN_KILL{.canTerminate = true, .canKill = true, .canStop = true, .canContinue = true};
constexpr Platform::ProcessActionCapabilities CANNOT_KILL{.canTerminate = true, .canKill = false};
constexpr float WINDOW_SIZE = 800.0F;

class KeyboardInputRenderTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Context = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.DisplaySize = ImVec2(WINDOW_SIZE, WINDOW_SIZE);
        io.DeltaTime = 1.0F / 60.0F;
        // As the app runs (UILayer): ImGui's own keyboard navigation is on, so the tests see whether
        // the table's keys are kept from it.
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        io.Fonts->AddFontDefault();
    }

    void TearDown() override
    {
        ImGui::DestroyContext(m_Context);
    }

    /// One frame of a window standing in for a panel, running @p body inside it.
    static void runFrame(const std::function<void()>& body)
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
        ImGui::SetNextWindowSize(ImVec2(WINDOW_SIZE, WINDOW_SIZE));
        ImGui::Begin("Panel", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
        body();
        ImGui::End();
        ImGui::Render();
    }

    /// Presses and releases @p key (with Shift when @p shift) over two frames while @p body draws.
    static void pressKey(ImGuiKey key, const std::function<void()>& body, bool shift = false)
    {
        ImGuiIO& io = ImGui::GetIO();
        if (shift)
        {
            io.AddKeyEvent(ImGuiMod_Shift, true);
        }
        io.AddKeyEvent(key, true);
        runFrame(body);
        io.AddKeyEvent(key, false);
        if (shift)
        {
            io.AddKeyEvent(ImGuiMod_Shift, false);
        }
        runFrame(body);
    }

    /// Moves the mouse over the panel (it is then hovered, which arms the table's keys).
    static void hoverPanel(const std::function<void()>& body)
    {
        ImGui::GetIO().AddMousePosEvent(WINDOW_SIZE * 0.5F, WINDOW_SIZE * 0.75F);
        runFrame(body);
    }

  private:
    ImGuiContext* m_Context = nullptr;
};

/// A view of the Actions tab driven as ShellLayer drives it: F9 asks the view for the Kill confirm.
struct ActionsHarness
{
    TestMocks::MockProcessActions mock;
    ProcessActionsView view;
    Platform::ProcessActionCapabilities capabilities = CAN_KILL;
    Platform::ProcessTarget target = TARGET;
    bool popupOpen = false;
    int f9Requests = 0;
    int f9Accepted = 0;
    // An action button pressed at the start of the next frame, before the keys are read (as the
    // Processes row menu is drawn before its table reads them).
    std::optional<Detail::ProcessAction> buttonPress;

    void frame()
    {
        if (buttonPress.has_value())
        {
            view.requestAction(*buttonPress, target, "victim");
            buttonPress.reset();
        }
        if (KeyboardInput::pollFunctionKeys() == ShortcutAction::KillSelected)
        {
            ++f9Requests;
            f9Accepted += view.requestKillShortcut(capabilities, target, "victim") ? 1 : 0;
        }
        view.render(&mock, capabilities, "victim", target);
        popupOpen = ImGui::IsPopupOpen(CONFIRM_POPUP_ID);
    }
};

TEST_F(KeyboardInputRenderTest, F9OpensTheKillConfirmAndKillsNothing)
{
    ActionsHarness h;
    const auto body = [&h]
    {
        h.frame();
    };
    runFrame(body);
    ASSERT_FALSE(h.popupOpen);

    pressKey(ImGuiKey_F9, body);
    runFrame(body);

    EXPECT_EQ(h.f9Requests, 1);
    EXPECT_TRUE(h.popupOpen);
    EXPECT_TRUE(h.view.confirmRequested());
    EXPECT_EQ(h.view.pendingAction(), Detail::ProcessAction::Kill);
    EXPECT_EQ(h.view.confirmTarget().target.pid, TARGET.pid);
    EXPECT_EQ(h.view.confirmTarget().target.startTimeTicks, TARGET.startTimeTicks);
    EXPECT_EQ(h.mock.killCount(), 0);
    EXPECT_EQ(h.mock.terminateCount(), 0);

    // With the dialog up, another F9 is the dialog's business, not a second request; still no kill.
    pressKey(ImGuiKey_F9, body);
    EXPECT_EQ(h.f9Requests, 1);
    EXPECT_TRUE(h.popupOpen);
    EXPECT_EQ(h.mock.killCount(), 0);
}

TEST_F(KeyboardInputRenderTest, F9NeverReplacesARequestedConfirm)
{
    // Suspend requested on the very frame F9 is pressed: its dialog is not on ImGui's popup stack
    // yet, so only the pending request can stop F9 replacing it (Copilot review on #1471).
    ActionsHarness sameFrame;
    const auto sameBody = [&sameFrame]
    {
        sameFrame.frame();
    };
    runFrame(sameBody);
    sameFrame.buttonPress = Detail::ProcessAction::Stop;
    pressKey(ImGuiKey_F9, sameBody);
    runFrame(sameBody);
    EXPECT_EQ(sameFrame.f9Requests, 1);
    EXPECT_EQ(sameFrame.f9Accepted, 0);
    EXPECT_TRUE(sameFrame.popupOpen);
    EXPECT_EQ(sameFrame.view.pendingAction(), Detail::ProcessAction::Stop);
    EXPECT_EQ(sameFrame.view.confirmTarget().target.pid, TARGET.pid);
    EXPECT_EQ(sameFrame.mock.killCount(), 0);
}

TEST_F(KeyboardInputRenderTest, F9OnTheFrameAfterARequestDoesNotReplaceIt)
{
    // Requested one frame, F9 on the next, while the dialog is only just opening. A fresh test, so no
    // dialog from another harness is still open on the shared ImGui context.
    ActionsHarness nextFrame;
    const auto nextBody = [&nextFrame]
    {
        nextFrame.frame();
    };
    runFrame(nextBody);
    nextFrame.view.requestAction(Detail::ProcessAction::Terminate, TARGET, "victim");
    pressKey(ImGuiKey_F9, nextBody);
    runFrame(nextBody);
    EXPECT_EQ(nextFrame.f9Accepted, 0);
    EXPECT_EQ(nextFrame.view.pendingAction(), Detail::ProcessAction::Terminate);
    EXPECT_EQ(nextFrame.mock.killCount(), 0);
    EXPECT_EQ(nextFrame.mock.terminateCount(), 0);
}

TEST_F(KeyboardInputRenderTest, F9AfterASelectionChangeOpensItsConfirm)
{
    // A selection change with no confirm open must not leave a dismissal queued for the next render,
    // which would close F9's confirm as it opens (Copilot review on #1471).
    ActionsHarness h;
    const auto body = [&h]
    {
        h.frame();
    };
    h.view.onSelectionChanged();
    ASSERT_TRUE(h.view.requestKillShortcut(CAN_KILL, TARGET, "victim"));
    runFrame(body);
    runFrame(body);
    EXPECT_TRUE(h.popupOpen);
    EXPECT_EQ(h.view.pendingAction(), Detail::ProcessAction::Kill);
    EXPECT_EQ(h.mock.killCount(), 0);
}

TEST_F(KeyboardInputRenderTest, F9DoesNothingWithoutTheCapabilityOrASelection)
{
    ActionsHarness noKill;
    noKill.capabilities = CANNOT_KILL;
    pressKey(ImGuiKey_F9, [&noKill] { noKill.frame(); });
    runFrame([&noKill] { noKill.frame(); });
    EXPECT_EQ(noKill.f9Requests, 1);
    EXPECT_FALSE(noKill.popupOpen);
    EXPECT_FALSE(noKill.view.confirmRequested());

    ActionsHarness noSelection;
    noSelection.target = Platform::ProcessTarget{.pid = -1, .startTimeTicks = 0};
    pressKey(ImGuiKey_F9, [&noSelection] { noSelection.frame(); });
    runFrame([&noSelection] { noSelection.frame(); });
    EXPECT_FALSE(noSelection.popupOpen);
    EXPECT_FALSE(noSelection.view.confirmRequested());
    EXPECT_EQ(noSelection.mock.killCount(), 0);
}

TEST_F(KeyboardInputRenderTest, ModifiedFunctionKeysDoNothing)
{
    ShortcutAction seen = ShortcutAction::None;
    const auto body = [&seen]
    {
        if (const ShortcutAction action = KeyboardInput::pollFunctionKeys(); action != ShortcutAction::None)
        {
            seen = action;
        }
    };
    pressKey(ImGuiKey_F10, body, /*shift=*/true); // Shift+F10 is ImGui's context-menu key
    EXPECT_EQ(seen, ShortcutAction::None);
    pressKey(ImGuiKey_F10, body);
    EXPECT_EQ(seen, ShortcutAction::Quit);
}

TEST_F(KeyboardInputRenderTest, CtrlASelectsAllOnlyAsThatExactChord)
{
    // The Processes table's select-all (#804): Ctrl+A, not a bare A (and not Ctrl+Shift+A).
    int seen = 0;
    const auto body = [&seen]
    {
        if (KeyboardInput::pollSelectAll())
        {
            ++seen;
        }
    };
    ImGuiIO& io = ImGui::GetIO();
    pressKey(ImGuiKey_A, body);
    EXPECT_EQ(seen, 0);

    io.AddKeyEvent(ImGuiMod_Ctrl, true);
    pressKey(ImGuiKey_A, body, /*shift=*/true);
    EXPECT_EQ(seen, 0);
    pressKey(ImGuiKey_A, body);
    io.AddKeyEvent(ImGuiMod_Ctrl, false);
    runFrame(body);
    EXPECT_EQ(seen, 1); // Once per press: it does not repeat on the next frame
}

/// A small scrolling table of selectable rows, keyboard-driven exactly as ProcessesPanel drives its
/// table: armed and polled in the panel's window, the move applied inside the table, the moved-to row
/// drawn even when clipped and scrolled into view.
struct TableHarness
{
    static constexpr std::size_t ROWS = 60;
    static constexpr float TABLE_HEIGHT = 200.0F;

    std::optional<std::size_t> selected;
    bool scrollPending = false;
    bool withFilterField = false;
    bool focusFilter = false;
    bool focusFirstRow = false; // Puts ImGui's own keyboard navigation on row 0
    std::string filter;
    float scrollY = 0.0F;
    unsigned int firstRowId = 0;

    void frame()
    {
        if (withFilterField)
        {
            if (focusFilter)
            {
                ImGui::SetKeyboardFocusHere();
                focusFilter = false;
            }
            ImGui::InputText("##filter", &filter);
        }
        auto command = NavCommand::None;
        if (KeyboardInput::tableNavigationArmed())
        {
            KeyboardInput::claimNavigationKeys(ImGui::GetID("##TableKeys"), /*treeView=*/false);
            command = KeyboardInput::pollNavigationCommand(/*treeView=*/false);
        }
        if (!ImGui::BeginTable("Rows", 1, ImGuiTableFlags_ScrollY, ImVec2(0.0F, TABLE_HEIGHT)))
        {
            return;
        }
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Name");
        ImGui::TableHeadersRow();
        if (command != NavCommand::None)
        {
            const float rowHeight = ImGui::GetTextLineHeight() + (ImGui::GetStyle().CellPadding.y * 2.0F);
            const std::size_t page = ProcessTableNavigation::pageStep(KeyboardInput::tableScrollViewHeight(), rowHeight);
            if (const auto next = ProcessTableNavigation::stepSelection(selected, command, ROWS, page); next.has_value())
            {
                selected = next;
                scrollPending = true;
            }
        }
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(ROWS));
        if (scrollPending && selected.has_value())
        {
            clipper.IncludeItemByIndex(static_cast<int>(*selected));
        }
        while (clipper.Step())
        {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
            {
                const auto row = static_cast<std::size_t>(i);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::PushID(i);
                const bool isSelected = selected == row;
                if (i == 0 && focusFirstRow)
                {
                    ImGui::SetKeyboardFocusHere();
                    focusFirstRow = false;
                }
                if (ImGui::Selectable("row", isSelected, ImGuiSelectableFlags_SpanAllColumns))
                {
                    selected = row;
                }
                if (i == 0)
                {
                    firstRowId = ImGui::GetItemID();
                }
                if (isSelected && scrollPending)
                {
                    KeyboardInput::scrollLastItemIntoView();
                    scrollPending = false;
                }
                ImGui::PopID();
            }
        }
        scrollPending = false;
        scrollY = ImGui::GetScrollY();
        ImGui::EndTable();
    }
};

TEST_F(KeyboardInputRenderTest, NavigationKeysMoveTheSelection)
{
    TableHarness t;
    const auto body = [&t]
    {
        t.frame();
    };
    hoverPanel(body);
    ASSERT_FALSE(t.selected.has_value());

    pressKey(ImGuiKey_DownArrow, body); // No selection: the first row
    EXPECT_EQ(t.selected, std::optional<std::size_t>{0});
    pressKey(ImGuiKey_DownArrow, body);
    EXPECT_EQ(t.selected, std::optional<std::size_t>{1});
    pressKey(ImGuiKey_J, body);
    EXPECT_EQ(t.selected, std::optional<std::size_t>{2});
    pressKey(ImGuiKey_K, body);
    EXPECT_EQ(t.selected, std::optional<std::size_t>{1});
    pressKey(ImGuiKey_UpArrow, body);
    EXPECT_EQ(t.selected, std::optional<std::size_t>{0});
    pressKey(ImGuiKey_UpArrow, body); // Clamped at the top
    EXPECT_EQ(t.selected, std::optional<std::size_t>{0});
    EXPECT_FLOAT_EQ(t.scrollY, 0.0F);

    // End (and G) go to the last row and scroll it into view; Home (and g) back to the top.
    pressKey(ImGuiKey_End, body);
    runFrame(body); // The scroll set while drawing the row is applied on the next frame
    EXPECT_EQ(t.selected, std::optional<std::size_t>{TableHarness::ROWS - 1});
    EXPECT_GT(t.scrollY, 0.0F);
    pressKey(ImGuiKey_Home, body);
    runFrame(body);
    EXPECT_EQ(t.selected, std::optional<std::size_t>{0});
    EXPECT_FLOAT_EQ(t.scrollY, 0.0F);
    pressKey(ImGuiKey_G, body, /*shift=*/true);
    EXPECT_EQ(t.selected, std::optional<std::size_t>{TableHarness::ROWS - 1});
    pressKey(ImGuiKey_G, body);
    EXPECT_EQ(t.selected, std::optional<std::size_t>{0});

    // Page Down moves by the rows that fit below the header, less one, so several but not all.
    pressKey(ImGuiKey_PageDown, body);
    const std::size_t afterPage = t.selected.value_or(0);
    EXPECT_GT(afterPage, 1U);
    EXPECT_LT(afterPage, TableHarness::ROWS - 1);
    pressKey(ImGuiKey_PageUp, body);
    EXPECT_EQ(t.selected, std::optional<std::size_t>{0});
}

TEST_F(KeyboardInputRenderTest, ImGuiNavigationDoesNotAlsoMove)
{
    TableHarness t;
    const auto body = [&t]
    {
        t.frame();
    };
    hoverPanel(body);
    // Put ImGui's own keyboard navigation on the first row, as a click or Tab would.
    t.focusFirstRow = true;
    runFrame(body);
    runFrame(body);
    const ImGuiID navBefore = ImGui::GetCurrentContext()->NavId;
    ASSERT_EQ(navBefore, t.firstRowId);

    pressKey(ImGuiKey_DownArrow, body);
    pressKey(ImGuiKey_DownArrow, body);
    runFrame(body);
    ASSERT_TRUE(t.selected.has_value());
    EXPECT_EQ(ImGui::GetCurrentContext()->NavId, navBefore); // The arrows went to the table only
}

TEST_F(KeyboardInputRenderTest, KeysWhileTypingDoNothing)
{
    TableHarness t;
    t.withFilterField = true;
    t.focusFilter = true;
    t.selected = 3;
    ShortcutAction seen = ShortcutAction::None;
    const auto body = [&t, &seen]
    {
        if (const ShortcutAction action = KeyboardInput::pollFunctionKeys(); action != ShortcutAction::None)
        {
            seen = action;
        }
        t.frame();
    };
    hoverPanel(body);
    runFrame(body);
    runFrame(body); // Focused, then active, then reported as wanting text
    ASSERT_TRUE(ImGui::GetIO().WantTextInput);

    for (const ImGuiKey key :
         {ImGuiKey_F9, ImGuiKey_F5, ImGuiKey_F10, ImGuiKey_J, ImGuiKey_K, ImGuiKey_G, ImGuiKey_End, ImGuiKey_Home, ImGuiKey_PageDown})
    {
        SCOPED_TRACE(ImGui::GetKeyName(key));
        pressKey(key, body);
        EXPECT_EQ(seen, ShortcutAction::None);
        EXPECT_EQ(t.selected, std::optional<std::size_t>{3});
        EXPECT_TRUE(ImGui::GetIO().WantTextInput); // Still typing: the key was the field's
    }
}

} // namespace
} // namespace App
