/// @file test_SettingsDialogRender.cpp
/// @brief The Settings dialog, headless (#1547): loading the stored settings into the controls,
/// Reset to defaults, and the dialog itself -- centred and inside the window at any size, Cancel and
/// Escape closing it without saving, Save, About and the two folder buttons reported to the layer,
/// which carries them out.

#include "App/SettingsDialog.h"
#include "App/SettingsLayerDetail.h"
#include "App/UserConfig.h"
#include "UI/DialogMetrics.h"
#include "UI/Theme.h"

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h> // GetTopMostPopupModal(), ImHashStr(), ActivateItemByID(): the modal and its controls

#include <cstddef>
#include <optional>
#include <string_view>
#include <vector>

namespace App
{
namespace
{

using SettingsDialog::Action;

std::vector<UI::DiscoveredTheme> themes()
{
    return {
        UI::DiscoveredTheme{.id = "arctic-fire", .name = "Arctic Fire", .description = {}, .path = {}},
        UI::DiscoveredTheme{.id = "cyberpunk", .name = "Cyberpunk", .description = {}, .path = {}},
    };
}

TEST(SettingsDialogStateTest, LoadPicksTheStoredValuesWithoutTouchingThem)
{
    UserSettings settings;
    settings.themeId = "cyberpunk";
    settings.fontSize = UI::FontSize::Large;
    settings.refreshIntervalMs = 500;
    settings.maxHistorySeconds = 120;
    settings.showPrivilegeNotice = false;
    SettingsDialog::State state;
    SettingsDialog::load(state, settings, themes());
    EXPECT_EQ(state.themes.size(), 2U);
    EXPECT_EQ(state.themeChoice.index, std::optional<std::size_t>(1));
    EXPECT_EQ(state.fontSizeChoice.index, std::optional<std::size_t>(2));
    EXPECT_EQ(state.refreshRateChoice.index, std::optional<std::size_t>(2));
    EXPECT_EQ(state.historyChoice.index, std::optional<std::size_t>(1));
    EXPECT_FALSE(state.themeChoice.touched); // Nothing is written until the user picks it (#1120)
    EXPECT_FALSE(state.fontSizeChoice.touched);
    EXPECT_FALSE(state.showPrivilegeNotice);
}

TEST(SettingsDialogStateTest, LoadOffersACustomPreviewForAValueThatIsNoOption)
{
    UserSettings settings;
    settings.themeId = "deleted-theme";
    settings.refreshIntervalMs = 750;
    SettingsDialog::State state;
    SettingsDialog::load(state, settings, themes());
    EXPECT_FALSE(state.themeChoice.index.has_value());
    EXPECT_FALSE(state.refreshRateChoice.index.has_value());
    EXPECT_EQ(state.customThemePreview, "Custom (deleted-theme)");
    EXPECT_FALSE(state.customRefreshPreview.empty());
}

TEST(SettingsDialogStateTest, ResetToDefaultsPicksEveryDefault)
{
    UserSettings settings;
    settings.themeId = "cyberpunk";
    settings.fontSize = UI::FontSize::Huge;
    settings.showPrivilegeNotice = false;
    SettingsDialog::State state;
    SettingsDialog::load(state, settings, themes());
    SettingsDialog::resetToDefaults(state);
    const UserSettings defaults;
    EXPECT_EQ(state.themeChoice.index, std::optional<std::size_t>(0)); // arctic-fire
    EXPECT_TRUE(state.themeChoice.touched);                            // Save writes it
    EXPECT_EQ(state.fontSizeChoice.index,
              Detail::optionIndexOf(Detail::FONT_SIZE_OPTIONS, defaults.fontSize, &Detail::FontSizeOption::value));
    EXPECT_TRUE(state.fontSizeChoice.touched);
    EXPECT_TRUE(state.refreshRateChoice.touched);
    EXPECT_TRUE(state.historyChoice.touched);
    EXPECT_EQ(state.showPrivilegeNotice, defaults.showPrivilegeNotice);
}

TEST(SettingsDialogStateTest, ResetLeavesTheThemeWhenTheDefaultThemeIsMissing)
{
    SettingsDialog::State state;
    UserSettings settings;
    settings.themeId = "cyberpunk";
    SettingsDialog::load(state, settings, {UI::DiscoveredTheme{.id = "cyberpunk", .name = "Cyberpunk", .description = {}, .path = {}}});
    SettingsDialog::resetToDefaults(state);
    EXPECT_EQ(state.themeChoice.index, std::optional<std::size_t>(0));
    EXPECT_FALSE(state.themeChoice.touched);
}

/// The open modal's geometry at the end of a frame, and what render() returned.
struct Measured
{
    bool open = false;
    Action action = Action::None;
    ImVec2 pos;
    ImVec2 size;
};

class SettingsDialogRenderTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Context = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.DisplaySize = ImVec2(1600.0F, 900.0F);
        io.DeltaTime = 1.0F / 60.0F;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // ImGui owns the font atlas
        io.Fonts->AddFontDefault();
        SettingsDialog::load(m_State, UserSettings{}, themes());
    }

    void TearDown() override
    {
        ImGui::DestroyContext(m_Context);
    }

    Measured runFrame()
    {
        Measured measured;
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        ImGui::Begin("Main", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
        ImGui::End();
        measured.action = SettingsDialog::render(m_State);
        measured.open = ImGui::IsPopupOpen(SettingsDialog::POPUP_ID);
        if (const ImGuiWindow* modal = ImGui::GetTopMostPopupModal(); modal != nullptr)
        {
            measured.pos = modal->Pos;
            measured.size = modal->Size;
        }
        ImGui::Render();
        return measured;
    }

    Measured settle()
    {
        Measured measured;
        for (int frame = 0; frame < SETTLE_FRAMES; ++frame)
        {
            measured = runFrame();
        }
        return measured;
    }

    Measured openAndSettle()
    {
        m_State.openRequested = true;
        return settle();
    }

    /// The modal's scrolling body, where the Advanced section's buttons are.
    static const ImGuiWindow* body()
    {
        const ImGuiWindow* modal = ImGui::GetTopMostPopupModal();
        if (modal == nullptr)
        {
            return nullptr;
        }
        for (const ImGuiWindow* window : GImGui->Windows)
        {
            if (window->ParentWindow == modal && std::string_view{window->Name}.contains("##SettingsBody"))
            {
                return window;
            }
        }
        return nullptr;
    }

    /// Presses a footer button (on the modal itself) on the next frame.
    static void pressFooter(const char* label)
    {
        const ImGuiWindow* modal = ImGui::GetTopMostPopupModal();
        ASSERT_NE(modal, nullptr);
        ImGui::ActivateItemByID(ImHashStr(label, 0, modal->ID));
    }

    /// Presses the filled (primary) footer button on the next frame: UI::Widgets::filledButton's
    /// "##filled" under the label's ID.
    static void pressFilledFooter(const char* label)
    {
        const ImGuiWindow* modal = ImGui::GetTopMostPopupModal();
        ASSERT_NE(modal, nullptr);
        ImGui::ActivateItemByID(ImHashStr("##filled", 0, ImHashStr(label, 0, modal->ID)));
    }

    /// Presses an item in the scrolling body on the next frame.
    static void pressInBody(const char* label)
    {
        const ImGuiWindow* window = body();
        ASSERT_NE(window, nullptr);
        ImGui::ActivateItemByID(ImHashStr(label, 0, window->ID));
    }

    /// Opens the body's combo @p comboId and picks its entry @p entry (under @p entryScope's ID, when
    /// the entries are scoped), a frame or two apiece, as a click on each would.
    void pickFromCombo(const char* comboId, const char* entry, const char* entryScope = nullptr)
    {
        pressInBody(comboId);
        static_cast<void>(runFrame());
        static_cast<void>(runFrame());
        ASSERT_FALSE(GImGui->OpenPopupStack.empty());
        const ImGuiWindow* combo = GImGui->OpenPopupStack.back().Window;
        ASSERT_NE(combo, nullptr);
        ASSERT_TRUE(std::string_view(combo->Name).contains("Combo")) << combo->Name;
        const ImGuiID scope = entryScope != nullptr ? ImHashStr(entryScope, 0, combo->ID) : combo->ID;
        ImGui::ActivateItemByID(ImHashStr(entry, 0, scope));
        static_cast<void>(runFrame());
        static_cast<void>(runFrame());
    }

    static void expectInsideDisplay(const Measured& measured)
    {
        const ImVec2 display = ImGui::GetIO().DisplaySize;
        EXPECT_GE(measured.pos.x, 0.0F);
        EXPECT_GE(measured.pos.y, 0.0F);
        EXPECT_LE(measured.pos.x + measured.size.x, display.x);
        EXPECT_LE(measured.pos.y + measured.size.y, display.y);
    }

    static constexpr int SETTLE_FRAMES = 6;

    SettingsDialog::State m_State;

  private:
    ImGuiContext* m_Context = nullptr;
};

TEST_F(SettingsDialogRenderTest, EachComboPicksAnEntryAndMarksItTouched)
{
    static_cast<void>(openAndSettle());
    ASSERT_FALSE(m_State.themeChoice.touched);

    // The theme's entries are scoped by theme id: two themes may share a display name.
    pickFromCombo("##Theme", "Cyberpunk", "cyberpunk");
    EXPECT_EQ(m_State.themeChoice.index, std::optional<std::size_t>(1));
    EXPECT_TRUE(m_State.themeChoice.touched);

    pickFromCombo("##FontSize", "Large");
    EXPECT_EQ(m_State.fontSizeChoice.index, std::optional<std::size_t>(2));
    EXPECT_TRUE(m_State.fontSizeChoice.touched);

    pickFromCombo("##RefreshRate", "250 ms");
    EXPECT_EQ(m_State.refreshRateChoice.index, std::optional<std::size_t>(1));
    EXPECT_TRUE(m_State.refreshRateChoice.touched);

    pickFromCombo("##History", "10 minutes");
    EXPECT_EQ(m_State.historyChoice.index, std::optional<std::size_t>(3));
    EXPECT_TRUE(m_State.historyChoice.touched);

    // Picking only edits the dialog's state: still open, nothing saved.
    EXPECT_TRUE(runFrame().open);
}

// The Escape that closes an open combo must not also close the dialog (SettingsDialog's
// comboOpenLastFrame guard): with a combo open, the dialog stays, unsaved and unchanged.
TEST_F(SettingsDialogRenderTest, EscapeWithAComboOpenKeepsTheDialog)
{
    static_cast<void>(openAndSettle());
    pressInBody("##RefreshRate");
    static_cast<void>(runFrame());
    static_cast<void>(runFrame());
    ASSERT_GE(GImGui->OpenPopupStack.Size, 2); // the dialog, and the combo over it

    ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, true);
    static_cast<void>(runFrame());
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, false);
    const Measured after = settle();
    EXPECT_TRUE(after.open);
    EXPECT_FALSE(m_State.refreshRateChoice.touched);
}

TEST_F(SettingsDialogRenderTest, StaysClosedUntilRequested)
{
    const Measured measured = settle();
    EXPECT_FALSE(measured.open);
    EXPECT_EQ(measured.action, Action::None);
}

TEST_F(SettingsDialogRenderTest, OpensCentredInsideTheWindow)
{
    const Measured measured = openAndSettle();
    ASSERT_TRUE(measured.open);
    EXPECT_FALSE(m_State.openRequested);
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    EXPECT_NEAR(measured.pos.x + (measured.size.x * 0.5F), display.x * 0.5F, 1.0F);
    EXPECT_NEAR(measured.pos.y + (measured.size.y * 0.5F), display.y * 0.5F, 1.0F);
    expectInsideDisplay(measured);
    EXPECT_NE(body(), nullptr);
}

// #1891: a refresh rate and history length that are no option show as "Custom (...)" previews, and the
// combos are sized for them too, so the dialog is at least as wide and still inside the window.
TEST_F(SettingsDialogRenderTest, CustomValuesAreMeasuredForTheCombos)
{
    const Measured standard = openAndSettle();
    ASSERT_TRUE(standard.open);
    pressFooter(SettingsDialog::CANCEL_LABEL);
    ASSERT_FALSE(settle().open);

    UserSettings custom;
    custom.refreshIntervalMs = 750; // no option
    custom.maxHistorySeconds = 777; // no option
    SettingsDialog::load(m_State, custom, themes());
    ASSERT_FALSE(m_State.customRefreshPreview.empty());
    ASSERT_FALSE(m_State.customHistoryPreview.empty());
    const Measured measured = openAndSettle();
    ASSERT_TRUE(measured.open);
    EXPECT_GE(measured.size.x, standard.size.x);
    expectInsideDisplay(measured);
}

TEST_F(SettingsDialogRenderTest, StaysInsideASmallWindowAtALargeFont)
{
    ImGui::GetIO().DisplaySize = ImVec2(480.0F, 320.0F);
    ImGui::GetStyle().FontScaleMain = 2.0F;
    const Measured measured = openAndSettle();
    ASSERT_TRUE(measured.open);
    EXPECT_LE(measured.size.x, UI::DialogMetrics::computeDialogMaxExtent(480.0F) + 1.0F);
    EXPECT_LE(measured.size.y, UI::DialogMetrics::computeDialogMaxExtent(320.0F) + 1.0F);
    expectInsideDisplay(measured);
}

TEST_F(SettingsDialogRenderTest, CancelClosesWithoutSaving)
{
    ASSERT_TRUE(openAndSettle().open);
    pressFooter(SettingsDialog::CANCEL_LABEL);
    EXPECT_EQ(runFrame().action, Action::None);
    EXPECT_FALSE(settle().open);
}

TEST_F(SettingsDialogRenderTest, EscapeClosesWithoutSaving)
{
    ASSERT_TRUE(openAndSettle().open);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, true);
    EXPECT_EQ(runFrame().action, Action::None);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, false);
    EXPECT_FALSE(settle().open);
}

// #2827: a close request (a CloseDialogEvent, TASKSMACK_DEMO's close=dialog) closes the dialog as Cancel
// does and is spent by it; one made while the dialog is closed is dropped, not kept for the next opening.
TEST_F(SettingsDialogRenderTest, ACloseRequestClosesWithoutSaving)
{
    ASSERT_TRUE(openAndSettle().open);
    m_State.closeRequested = true;
    EXPECT_EQ(runFrame().action, Action::None);
    EXPECT_FALSE(settle().open);
    EXPECT_FALSE(m_State.closeRequested);
}

TEST_F(SettingsDialogRenderTest, ACloseRequestWhileClosedIsDropped)
{
    m_State.closeRequested = true;
    EXPECT_FALSE(settle().open);
    EXPECT_FALSE(m_State.closeRequested);
    EXPECT_TRUE(openAndSettle().open);
}

TEST_F(SettingsDialogRenderTest, SaveIsReportedAndCloses)
{
    ASSERT_TRUE(openAndSettle().open);
    pressFilledFooter(SettingsDialog::SAVE_LABEL);
    EXPECT_EQ(runFrame().action, Action::Save);
    EXPECT_FALSE(settle().open);
}

TEST_F(SettingsDialogRenderTest, ResetToDefaultsChangesTheControlsAndStaysOpen)
{
    UserSettings settings;
    settings.themeId = "cyberpunk";
    SettingsDialog::load(m_State, settings, themes());
    ASSERT_TRUE(openAndSettle().open);
    pressFooter(SettingsDialog::RESET_LABEL);
    EXPECT_EQ(runFrame().action, Action::None);
    EXPECT_EQ(m_State.themeChoice.index, std::optional<std::size_t>(0));
    EXPECT_TRUE(m_State.themeChoice.touched);
    EXPECT_TRUE(settle().open);
}

TEST_F(SettingsDialogRenderTest, TheFolderButtonsAreReportedAndKeepItOpen)
{
    ASSERT_TRUE(openAndSettle().open);
    pressInBody(SettingsDialog::EDIT_CONFIG_LABEL);
    EXPECT_EQ(runFrame().action, Action::EditConfig);
    pressInBody(SettingsDialog::OPEN_THEMES_LABEL);
    EXPECT_EQ(runFrame().action, Action::OpenThemesFolder);
    EXPECT_TRUE(settle().open);
}

TEST_F(SettingsDialogRenderTest, AboutIsReportedAndClosesSettings)
{
    ASSERT_TRUE(openAndSettle().open);
    pressInBody(SettingsDialog::ABOUT_LABEL);
    EXPECT_EQ(runFrame().action, Action::OpenAbout);
    EXPECT_FALSE(settle().open);
}

TEST_F(SettingsDialogRenderTest, TheLimitedDataNoticeCheckboxEditsTheState)
{
    ASSERT_TRUE(m_State.showPrivilegeNotice);
    ASSERT_TRUE(openAndSettle().open);
    pressInBody(SettingsDialog::PRIVILEGE_NOTICE_LABEL);
    (void) runFrame();
    EXPECT_FALSE(m_State.showPrivilegeNotice);
}

#ifdef _WIN32
// #2040: Windows only (#1492). The checkbox edits the state, which Save hands the layer; the
// stored value is loaded into it, and Reset to defaults puts it back off.
TEST_F(SettingsDialogRenderTest, TheUdpCheckboxEditsTheState)
{
    ASSERT_FALSE(m_State.udpEventTrace);
    ASSERT_TRUE(openAndSettle().open);
    pressInBody(SettingsDialog::UDP_EVENT_TRACE_LABEL);
    (void) runFrame();
    EXPECT_TRUE(m_State.udpEventTrace);
    pressInBody(SettingsDialog::UDP_EVENT_TRACE_LABEL);
    (void) runFrame();
    EXPECT_FALSE(m_State.udpEventTrace);
}
#endif

TEST(SettingsDialogStateTest, TheUdpTraceIsLoadedAndResetToOff)
{
    UserSettings settings;
    settings.udpEventTrace = true;
    SettingsDialog::State state;
    SettingsDialog::load(state, settings, themes());
    EXPECT_TRUE(state.udpEventTrace);
    SettingsDialog::resetToDefaults(state);
    EXPECT_EQ(state.udpEventTrace, UserSettings{}.udpEventTrace);
}

} // namespace
} // namespace App
