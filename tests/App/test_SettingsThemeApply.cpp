/// @file test_SettingsThemeApply.cpp
/// @brief #2713 (a #2700 manual check): a theme picked in Settings applies on Save, at the next frame's style
/// update, not when it is picked, and Cancel leaves the old one. Headless (no window), but with the real
/// UI::Theme and its bundled themes, which TaskSmackTests' theme stub can't list; the config goes to the
/// temporary directory AppTestsMain.cpp gives every test in this binary.

#include "App/SettingsDialog.h"
#include "App/SettingsLayer.h"
#include "App/UserConfig.h"
#include "Core/ApplicationEvents.h"
#include "Core/Event.h"
#include "UI/Theme.h"

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h> // GetTopMostPopupModal(), ImHashStr(), ActivateItemByID(): the modal and its controls
#include <implot.h>

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace App
{
namespace
{

class SettingsThemeApplyTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Context = ImGui::CreateContext();
        m_Plot = ImPlot::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.DisplaySize = ImVec2(1600.0F, 900.0F);
        io.DeltaTime = 1.0F / 60.0F;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        io.Fonts->AddFontDefault();
        UI::Theme::get().loadThemes(std::filesystem::path(TASKSMACK_SOURCE_THEMES_DIR));
        m_OriginalTheme = UI::Theme::get().currentThemeId();
        m_OriginalSettings = UserConfig::get().settings();
        m_Layer = std::make_unique<SettingsLayer>(
            [](Core::Event&) {}, [](const std::filesystem::path&) { return true; }, [] { return std::filesystem::temp_directory_path(); });
    }

    void TearDown() override
    {
        m_Layer.reset();
        UI::Theme::get().setThemeById(m_OriginalTheme);
        static_cast<void>(UI::Theme::get().applyPendingStyleChanges());
        UserConfig::get().settings() = m_OriginalSettings;
        ImPlot::DestroyContext(m_Plot);
        ImGui::DestroyContext(m_Context);
    }

    /// One frame of the layer over a full-window main window; whether the dialog is open after it.
    bool runFrame()
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        ImGui::Begin("Main", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
        ImGui::End();
        m_Layer->onRender();
        const bool open = ImGui::IsPopupOpen(SettingsDialog::POPUP_ID);
        ImGui::Render();
        return open;
    }

    bool settle()
    {
        bool open = false;
        for (int frame = 0; frame < 6; ++frame)
        {
            open = runFrame();
        }
        return open;
    }

    void openDialog()
    {
        Core::OpenSettingsEvent event;
        m_Layer->onEvent(event);
        ASSERT_TRUE(settle());
    }

    /// The modal's scrolling body, where the combos are.
    [[nodiscard]] static const ImGuiWindow* body()
    {
        const ImGuiWindow* modal = ImGui::GetTopMostPopupModal();
        for (const ImGuiWindow* window : GImGui->Windows)
        {
            if (modal != nullptr && window->ParentWindow == modal && std::string_view{window->Name}.contains("##SettingsBody"))
            {
                return window;
            }
        }
        return nullptr;
    }

    /// Picks the theme @p id, named @p name, from the Theme combo: its entries are scoped by theme id.
    void pickTheme(const std::string& id, const std::string& name)
    {
        const ImGuiWindow* window = body();
        ASSERT_NE(window, nullptr);
        ImGui::ActivateItemByID(ImHashStr("##Theme", 0, window->ID));
        static_cast<void>(settle());
        ASSERT_FALSE(GImGui->OpenPopupStack.empty());
        const ImGuiWindow* combo = GImGui->OpenPopupStack.back().Window;
        ASSERT_NE(combo, nullptr);
        ImGui::ActivateItemByID(ImHashStr(name.c_str(), 0, ImHashStr(id.c_str(), 0, combo->ID)));
        static_cast<void>(runFrame());
        static_cast<void>(runFrame());
    }

    /// Presses a footer button: Cancel plain, Save filled ("##filled" under its label). Whether the dialog is
    /// still open after it.
    bool pressFooter(const char* label, bool filled)
    {
        const ImGuiWindow* modal = ImGui::GetTopMostPopupModal();
        if (modal == nullptr)
        {
            ADD_FAILURE() << "no dialog to press " << label << " on";
            return false;
        }
        const ImGuiID button = ImHashStr(label, 0, modal->ID);
        ImGui::ActivateItemByID(filled ? ImHashStr("##filled", 0, button) : button);
        return settle();
    }

    std::string m_OriginalTheme;

  private:
    ImGuiContext* m_Context = nullptr;
    ImPlotContext* m_Plot = nullptr;
    std::unique_ptr<SettingsLayer> m_Layer;
    UserSettings m_OriginalSettings;
};

TEST_F(SettingsThemeApplyTest, APickedThemeAppliesOnSaveNotBefore)
{
    UI::Theme& theme = UI::Theme::get();
    ASSERT_FALSE(theme.discoveredThemes().empty());
    const std::string target = m_OriginalTheme == "dracula" ? "mocha" : "dracula";
    const std::string targetName = target == "dracula" ? "Dracula" : "Mocha";

    openDialog();
    pickTheme(target, targetName);
    EXPECT_FALSE(theme.applyPendingStyleChanges()) << "picking queues nothing";
    EXPECT_EQ(theme.currentThemeId(), m_OriginalTheme);
    EXPECT_FALSE(pressFooter(SettingsDialog::CANCEL_LABEL, false)) << "Cancel closes it";
    EXPECT_FALSE(theme.applyPendingStyleChanges()) << "Cancel queues nothing";
    EXPECT_EQ(theme.currentThemeId(), m_OriginalTheme);

    openDialog();
    pickTheme(target, targetName);
    EXPECT_FALSE(pressFooter(SettingsDialog::SAVE_LABEL, true)) << "Save closes it";
    EXPECT_EQ(theme.currentThemeId(), m_OriginalTheme) << "queued, not applied mid-frame";
    EXPECT_TRUE(theme.applyPendingStyleChanges()) << "the next frame applies it";
    EXPECT_EQ(theme.currentThemeId(), target);
    EXPECT_EQ(UserConfig::get().settings().themeId, target);
}

} // namespace
} // namespace App
