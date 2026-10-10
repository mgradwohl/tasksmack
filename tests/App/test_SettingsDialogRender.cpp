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

} // namespace
} // namespace App
