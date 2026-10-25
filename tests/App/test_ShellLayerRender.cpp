/// @file test_ShellLayerRender.cpp
/// @brief The shell's frame, headless with a fake host (#1547): the main window, its tab bar and the
/// status bar drawn over the (unattached) panels; the main window below the custom title bar only when
/// there is one; F1, F2 and F10 raising Help, Settings and a close request; the status bar's Settings
/// button with native decorations; and the window's minimum size handed to the host from the panels'
/// measurements.

#include "App/ShellHost.h"
#include "App/ShellLayer.h"
#include "App/StatusBarView.h"
#include "App/UserConfig.h"
#include "App/WindowOverride.h"
#include "Core/Event.h"
#include "Core/WindowGeometry.h"

#include <SDL3/SDL_video.h>
#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h> // FindWindowByName(), ImHashStr(), ActivateItemByID()
#include <implot.h>

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace App
{
namespace
{

constexpr float DISPLAY_WIDTH = 1280.0F;
constexpr float DISPLAY_HEIGHT = 800.0F;
constexpr float TITLE_BAR_HEIGHT = 32.0F;

/// What the shell asked of its host, for the test to read.
struct HostLog
{
    std::vector<Core::EventType> events;
    int closeRequests = 0;
    std::string title; ///< The last setWindowTitle() (#2728)
    std::vector<std::pair<int, int>> minimumSizes;
    std::vector<Core::WindowGeometry::MinimumSizeChange> minimumSizeChanges;
    WindowOverride::CapturedWindow window; // what captureWindow() reports
};

class FakeShellHost final : public IShellHost
{
  public:
    FakeShellHost(HostLog& log, bool borderless) : m_Log(log), m_Borderless(borderless)
    {}

    void raiseEvent(Core::Event& event) override
    {
        m_Log.events.push_back(event.getEventType());
    }
    [[nodiscard]] double lastFrameIntervalSeconds() const override
    {
        return 1.0 / 60.0;
    }
    [[nodiscard]] bool isBorderless() const override
    {
        return m_Borderless;
    }
    [[nodiscard]] float titleBarHeight() const override
    {
        return TITLE_BAR_HEIGHT;
    }
    void requestClose() override
    {
        ++m_Log.closeRequests;
    }
    void setWindowTitle(const std::string& title) override
    {
        m_Log.title = title;
    }
    [[nodiscard]] SDL_DisplayID displayId() const override
    {
        return 1;
    }
    [[nodiscard]] std::optional<std::pair<int, int>> usableDisplaySize() const override
    {
        return std::pair{3840, 2160};
    }
    [[nodiscard]] bool hasWindow() const override
    {
        return true;
    }
    void setMinimumWindowSize(int width, int height, Core::WindowGeometry::MinimumSizeChange change) override
    {
        m_Log.minimumSizes.emplace_back(width, height);
        m_Log.minimumSizeChanges.push_back(change);
    }
    [[nodiscard]] WindowOverride::CapturedWindow captureWindow() const override
    {
        return m_Log.window;
    }

  private:
    HostLog& m_Log;
    bool m_Borderless;
};

class ShellLayerRenderTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Context = ImGui::CreateContext();
        m_PlotContext = ImPlot::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.DisplaySize = ImVec2(DISPLAY_WIDTH, DISPLAY_HEIGHT);
        io.DeltaTime = 1.0F / 60.0F;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard; // as UILayer runs it
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        io.Fonts->AddFontDefault();
    }

    void TearDown() override
    {
        m_Shell.reset(); // before the contexts its panels drew into
        ImPlot::DestroyContext(m_PlotContext);
        ImGui::DestroyContext(m_Context);
    }

    void makeShell(bool borderless)
    {
        m_Shell = std::make_unique<ShellLayer>(std::make_unique<FakeShellHost>(m_Log, borderless));
    }

    /// One frame of the shell, as UILayer's NewFrame()/Render() bracket it.
    void frame()
    {
        ImGui::NewFrame();
        m_Shell->onRender();
        ImGui::Render();
    }

    /// Presses and releases @p key over two frames.
    void pressKey(ImGuiKey key)
    {
        ImGuiIO& io = ImGui::GetIO();
        io.AddKeyEvent(key, true);
        frame();
        io.AddKeyEvent(key, false);
        frame();
    }

    [[nodiscard]] int count(Core::EventType type) const
    {
        int n = 0;
        for (const Core::EventType raised : m_Log.events)
        {
            n += raised == type ? 1 : 0;
        }
        return n;
    }

    HostLog m_Log;
    std::unique_ptr<ShellLayer> m_Shell;

  private:
    ImGuiContext* m_Context = nullptr;
    ImPlotContext* m_PlotContext = nullptr;
};

TEST_F(ShellLayerRenderTest, DrawsTheMainWindowTheTabBarAndTheStatusBar)
{
    makeShell(false);
    frame();
    frame();

    const ImGuiWindow* main = ImGui::FindWindowByName("##MainWindow");
    ASSERT_NE(main, nullptr);
    EXPECT_TRUE(main->WasActive);
    const ImGuiWindow* status = ImGui::FindWindowByName("##StatusBar");
    ASSERT_NE(status, nullptr);
    EXPECT_TRUE(status->WasActive);
    // The main window and the status bar share the display between them, without overlapping.
    EXPECT_FLOAT_EQ(main->Pos.y, 0.0F);
    EXPECT_LE(main->Pos.y + main->Size.y, status->Pos.y + 0.5F);
    EXPECT_GT(main->Size.y, DISPLAY_HEIGHT / 2.0F);
}

TEST_F(ShellLayerRenderTest, TheMainWindowStartsBelowTheCustomTitleBar)
{
    makeShell(true);
    frame();
    frame();

    const ImGuiWindow* main = ImGui::FindWindowByName("##MainWindow");
    ASSERT_NE(main, nullptr);
    EXPECT_FLOAT_EQ(main->Pos.y, TITLE_BAR_HEIGHT);
}

TEST_F(ShellLayerRenderTest, TheGlobalFunctionKeysRaiseHelpSettingsAndAClose)
{
    makeShell(false);
    frame();

    pressKey(ImGuiKey_F1);
    EXPECT_EQ(count(Core::EventType::OpenHelp), 1);
    pressKey(ImGuiKey_F2);
    EXPECT_EQ(count(Core::EventType::OpenSettings), 1);
    EXPECT_EQ(m_Log.closeRequests, 0);
    pressKey(ImGuiKey_F10);
    EXPECT_EQ(m_Log.closeRequests, 1); // a request, vetoable, not a close
}

TEST_F(ShellLayerRenderTest, TheStatusBarsSettingsButtonRaisesOpenSettingsWithNativeDecorations)
{
    makeShell(false);
    frame();

    const ImGuiWindow* status = ImGui::FindWindowByName("##StatusBar");
    ASSERT_NE(status, nullptr);
    ImGui::ActivateItemByID(ImHashStr(StatusBarView::SETTINGS_LABEL, 0, status->ID));
    frame();
    frame();
    EXPECT_EQ(count(Core::EventType::OpenSettings), 1);
}

TEST_F(ShellLayerRenderTest, TheWindowsMinimumSizeFollowsThePanelsMeasurements)
{
    makeShell(false);
    frame();
    frame();

    // No title bar to hand it to: the shell sets it on the window itself, once the panels have
    // measured their content, and not again while that is unchanged.
    ASSERT_FALSE(m_Log.minimumSizes.empty());
    const auto [width, height] = m_Log.minimumSizes.back();
    EXPECT_GT(width, 0);
    EXPECT_GT(height, 0);
    EXPECT_LE(width, 3840);
    EXPECT_LE(height, 2160);
    const std::size_t applied = m_Log.minimumSizes.size();
    frame();
    EXPECT_EQ(m_Log.minimumSizes.size(), applied);

    // At startup there is no size of the user's to keep: the first minimum may grow the window (#1945).
    ASSERT_FALSE(m_Log.minimumSizeChanges.empty());
    EXPECT_EQ(m_Log.minimumSizeChanges.front(), Core::WindowGeometry::MinimumSizeChange::AllowGrow);
}

/// onDetach() against a config file of the test's own: UserConfig is redirected to a fresh temporary
/// directory, never the user's config.toml.
class ShellLayerDetachTest : public ShellLayerRenderTest
{
  protected:
    void SetUp() override
    {
        ShellLayerRenderTest::SetUp();
        m_OriginalPath = UserConfig::get().configPath();
        m_Settings = UserConfig::get().settings();
        std::error_code ec;
        const std::filesystem::path base = std::filesystem::temp_directory_path(ec);
        ASSERT_FALSE(ec);
        std::random_device random;
        for (int attempt = 0; attempt < 100 && m_TempDir.empty(); ++attempt)
        {
            std::filesystem::path candidate = base / ("tasksmack_shell_detach_" + std::to_string(random()));
            if (std::filesystem::create_directory(candidate, ec) && !ec)
            {
                m_TempDir = std::move(candidate);
            }
        }
        ASSERT_FALSE(m_TempDir.empty());
        m_ConfigPath = m_TempDir / "config.toml";
        UserConfig::get().resetConfigPathForTesting(m_ConfigPath);
        ASSERT_EQ(UserConfig::get().configPath(), m_ConfigPath);
    }

    void TearDown() override
    {
        UserConfig::get().resetConfigPathForTesting(m_OriginalPath);
        UserConfig::get().settings() = m_Settings;
        std::error_code ec;
        std::filesystem::remove_all(m_TempDir, ec);
        ShellLayerRenderTest::TearDown();
    }

    std::filesystem::path m_ConfigPath;

  private:
    std::filesystem::path m_OriginalPath;
    std::filesystem::path m_TempDir;
    UserSettings m_Settings;
};

TEST_F(ShellLayerDetachTest, SavesTheWindowsNormalGeometryFromTheHost)
{
    // Maximized: what Restore returns to is saved, with the scale it was measured at (#1121, #1168).
    m_Log.window = {.normal = Core::WindowGeometry::Rect{.x = 120, .y = 80, .width = 1111, .height = 777},
                    .normalScale = 1.5F,
                    .canPosition = true,
                    .maximized = true};
    makeShell(false);
    frame();
    m_Shell->onDetach();

    const UserSettings& settings = UserConfig::get().settings();
    EXPECT_EQ(settings.windowWidth, 1111);
    EXPECT_EQ(settings.windowHeight, 777);
    EXPECT_EQ(settings.windowPosX, 120);
    EXPECT_EQ(settings.windowPosY, 80);
    EXPECT_EQ(settings.windowScale, std::optional<float>{1.5F});
    EXPECT_TRUE(settings.windowMaximized);

    // And written out: a fresh load reads the same back.
    ASSERT_TRUE(std::filesystem::exists(m_ConfigPath));
    UserConfig::get().settings() = UserSettings{};
    UserConfig::get().load();
    EXPECT_EQ(UserConfig::get().settings().windowWidth, 1111);
    EXPECT_EQ(UserConfig::get().settings().windowHeight, 777);
    EXPECT_TRUE(UserConfig::get().settings().windowMaximized);
}

TEST_F(ShellLayerDetachTest, AnUnknownNormalRectangleKeepsTheSavedSize)
{
    UserConfig::get().settings().windowWidth = 900;
    UserConfig::get().settings().windowHeight = 650;
    // Maximized by the compositor: no normal rectangle to save, so the loaded one stays.
    m_Log.window = {.normal = std::nullopt, .normalScale = 0.0F, .canPosition = false, .maximized = true};
    makeShell(false);
    m_Shell->onDetach();

    EXPECT_EQ(UserConfig::get().settings().windowWidth, 900);
    EXPECT_EQ(UserConfig::get().settings().windowHeight, 650);
    EXPECT_TRUE(UserConfig::get().settings().windowMaximized);
}

} // namespace
} // namespace App
