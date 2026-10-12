/// @file test_SettingsLayerRender.cpp
/// @brief The Settings layer (#1395), run headless through its test seam: Core::OpenSettingsEvent opens
/// the dialog, Save writes only what the user picked, raises the refresh-rate and history events and
/// saves the config, and the Advanced buttons hand their paths to the opener or raise
/// Core::OpenAboutEvent. UserConfig is redirected to a temporary directory, never the user's config.

#include "App/SettingsDialog.h"
#include "App/SettingsLayer.h"
#include "App/UserConfig.h"
#include "Core/ApplicationEvents.h"
#include "Core/Event.h"
#include "UI/Theme.h"

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h> // GetTopMostPopupModal(), ImHashStr(), ActivateItemByID(): the modal and its controls

#include <filesystem>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace App
{
namespace
{

/// What the layer handed to its seams.
struct Seen
{
    std::vector<int> refreshIntervalsMs;
    std::vector<int> historySeconds;
    int aboutRequests = 0;
    std::vector<std::filesystem::path> opened;
};

class SettingsLayerRenderTest : public ::testing::Test
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

        m_OriginalPath = UserConfig::get().configPath();
        m_OriginalSettings = UserConfig::get().settings();
        m_OriginalFontSize = UI::Theme::get().currentFontSize();
        std::error_code ec;
        const std::filesystem::path base = std::filesystem::temp_directory_path(ec);
        ASSERT_FALSE(ec);
        std::random_device random;
        for (int attempt = 0; attempt < 100 && m_TempDir.empty(); ++attempt)
        {
            std::filesystem::path candidate = base / ("tasksmack_settings_layer_" + std::to_string(random()));
            if (std::filesystem::create_directory(candidate, ec) && !ec)
            {
                m_TempDir = std::move(candidate);
            }
        }
        ASSERT_FALSE(m_TempDir.empty());
        m_ConfigPath = m_TempDir / "config.toml";
        UserConfig::get().resetConfigPathForTesting(m_ConfigPath);
        ASSERT_EQ(UserConfig::get().configPath(), m_ConfigPath);

        m_Layer = std::make_unique<SettingsLayer>(
            [this](Core::Event& event)
            {
                Core::EventDispatcher dispatcher(event);
                dispatcher.dispatch<Core::RefreshRateChangedEvent>(
                    [this](Core::RefreshRateChangedEvent& e)
                    {
                        m_Seen.refreshIntervalsMs.push_back(e.getIntervalMs());
                        return false;
                    });
                dispatcher.dispatch<Core::HistoryDurationChangedEvent>(
                    [this](Core::HistoryDurationChangedEvent& e)
                    {
                        m_Seen.historySeconds.push_back(e.getSeconds());
                        return false;
                    });
                dispatcher.dispatch<Core::OpenAboutEvent>(
                    [this](Core::OpenAboutEvent&)
                    {
                        ++m_Seen.aboutRequests;
                        return false;
                    });
            },
            [this](const std::filesystem::path& path)
            {
                m_Seen.opened.push_back(path);
                return true;
            },
            [this] { return m_TempDir; });
    }

    void TearDown() override
    {
        m_Layer.reset();
        UI::Theme::get().setFontSize(m_OriginalFontSize);
        UserConfig::get().resetConfigPathForTesting(m_OriginalPath);
        UserConfig::get().settings() = m_OriginalSettings;
        std::error_code ec;
        std::filesystem::remove_all(m_TempDir, ec);
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
        for (int frame = 0; frame < SETTLE_FRAMES; ++frame)
        {
            open = runFrame();
        }
        return open;
    }

    /// Raises Core::OpenSettingsEvent at the layer, as the title bar's gear button does, and lets the
    /// dialog appear. False if the layer consumed the event (other layers may want it too).
    bool openDialog()
    {
        Core::OpenSettingsEvent event;
        m_Layer->onEvent(event);
        EXPECT_FALSE(event.isHandled());
        return settle();
    }

    /// The modal's scrolling body, where the combos and the Advanced section's buttons are.
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

    /// Presses an item in the scrolling body, then lets it take effect.
    void pressInBody(const char* label)
    {
        const ImGuiWindow* window = body();
        ASSERT_NE(window, nullptr);
        ImGui::ActivateItemByID(ImHashStr(label, 0, window->ID));
        static_cast<void>(runFrame());
        static_cast<void>(runFrame());
    }

    /// Opens the body's combo @p comboId and picks its entry @p entry, a frame or two apiece.
    void pickFromCombo(const char* comboId, const char* entry)
    {
        pressInBody(comboId);
        ASSERT_FALSE(GImGui->OpenPopupStack.empty());
        const ImGuiWindow* combo = GImGui->OpenPopupStack.back().Window;
        ASSERT_NE(combo, nullptr);
        ASSERT_TRUE(std::string_view(combo->Name).contains("Combo")) << combo->Name;
        ImGui::ActivateItemByID(ImHashStr(entry, 0, combo->ID));
        static_cast<void>(runFrame());
        static_cast<void>(runFrame());
    }

    /// Presses Save: UI::Widgets::filledButton's "##filled" under the label's ID, on the modal.
    void pressSave()
    {
        const ImGuiWindow* modal = ImGui::GetTopMostPopupModal();
        ASSERT_NE(modal, nullptr);
        ImGui::ActivateItemByID(ImHashStr("##filled", 0, ImHashStr(SettingsDialog::SAVE_LABEL, 0, modal->ID)));
        static_cast<void>(runFrame());
    }

    static constexpr int SETTLE_FRAMES = 6;

    Seen m_Seen;
    std::filesystem::path m_ConfigPath;
    std::filesystem::path m_TempDir;

  private:
    ImGuiContext* m_Context = nullptr;
    std::unique_ptr<SettingsLayer> m_Layer;
    std::filesystem::path m_OriginalPath;
    UserSettings m_OriginalSettings;
    UI::FontSize m_OriginalFontSize = UI::FontSize::Medium;
};

TEST_F(SettingsLayerRenderTest, StaysClosedUntilOpenSettingsIsRaised)
{
    EXPECT_FALSE(settle());
    EXPECT_TRUE(openDialog());
}

TEST_F(SettingsLayerRenderTest, SaveWritesThePickedChoicesRaisesTheirEventsAndSaves)
{
    UserSettings& settings = UserConfig::get().settings();
    settings.refreshIntervalMs = 1000;
    settings.maxHistorySeconds = 300;
    settings.fontSize = UI::FontSize::Medium;
    settings.showPrivilegeNotice = true;
    UI::Theme::get().setFontSize(UI::FontSize::Medium);
    ASSERT_TRUE(openDialog());

    pickFromCombo("##RefreshRate", "250 ms");
    pickFromCombo("##History", "10 minutes");
    pickFromCombo("##FontSize", "Large");
    pressInBody(SettingsDialog::PRIVILEGE_NOTICE_LABEL); // unticks it
    pressSave();

    EXPECT_FALSE(settle());
    EXPECT_EQ(settings.refreshIntervalMs, 250);
    EXPECT_EQ(settings.maxHistorySeconds, 600);
    EXPECT_EQ(settings.fontSize, UI::FontSize::Large);
    EXPECT_EQ(UI::Theme::get().currentFontSize(), UI::FontSize::Large);
    EXPECT_FALSE(settings.showPrivilegeNotice);
    EXPECT_EQ(m_Seen.refreshIntervalsMs, std::vector<int>{250});
    EXPECT_EQ(m_Seen.historySeconds, std::vector<int>{600});
    std::error_code ec;
    EXPECT_TRUE(std::filesystem::exists(m_ConfigPath, ec)); // saved to the redirected config

    // The saved file reads back as what was picked.
    UserConfig::get().settings() = UserSettings{};
    UserConfig::get().load();
    EXPECT_EQ(UserConfig::get().settings().refreshIntervalMs, 250);
    EXPECT_EQ(UserConfig::get().settings().maxHistorySeconds, 600);
}

TEST_F(SettingsLayerRenderTest, SaveWithNothingPickedRaisesNothing)
{
    UserSettings& settings = UserConfig::get().settings();
    settings.refreshIntervalMs = 500;
    settings.maxHistorySeconds = 120;
    ASSERT_TRUE(openDialog());
    pressSave();

    EXPECT_FALSE(settle());
    // Only the combos the user picked are written (#1120): the stored values stay.
    EXPECT_EQ(settings.refreshIntervalMs, 500);
    EXPECT_EQ(settings.maxHistorySeconds, 120);
    EXPECT_TRUE(m_Seen.refreshIntervalsMs.empty());
    EXPECT_TRUE(m_Seen.historySeconds.empty());
}

TEST_F(SettingsLayerRenderTest, PickingTheStoredValueAgainRaisesNothing)
{
    UserConfig::get().settings().refreshIntervalMs = 250;
    ASSERT_TRUE(openDialog());
    pickFromCombo("##RefreshRate", "250 ms");
    pressSave();
    EXPECT_TRUE(m_Seen.refreshIntervalsMs.empty());
}

TEST_F(SettingsLayerRenderTest, EditConfigOpensTheConfigFile)
{
    ASSERT_TRUE(openDialog());
    pressInBody(SettingsDialog::EDIT_CONFIG_LABEL);
    ASSERT_EQ(m_Seen.opened.size(), 1U);
    EXPECT_EQ(m_Seen.opened.front(), m_ConfigPath);
}

TEST_F(SettingsLayerRenderTest, OpenThemesFolderMakesAndOpensTheUserThemesDirectory)
{
    ASSERT_TRUE(openDialog());
    pressInBody(SettingsDialog::OPEN_THEMES_LABEL);
    ASSERT_EQ(m_Seen.opened.size(), 1U);
    EXPECT_EQ(m_Seen.opened.front(), m_TempDir / "themes");
    std::error_code ec;
    EXPECT_TRUE(std::filesystem::is_directory(m_TempDir / "themes", ec)); // created so the button always opens something
}

TEST_F(SettingsLayerRenderTest, AboutRaisesOpenAbout)
{
    ASSERT_TRUE(openDialog());
    pressInBody(SettingsDialog::ABOUT_LABEL);
    EXPECT_EQ(m_Seen.aboutRequests, 1);
}

} // namespace
} // namespace App
