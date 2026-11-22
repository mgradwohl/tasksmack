/// @file test_ShellLayerRender.cpp
/// @brief The shell's frame, headless with a fake host (#1547): the main window, its tab bar and the
/// status bar drawn over the (unattached) panels; the main window below the custom title bar only when
/// there is one; F1, F2 and F10 raising Help, Settings and a close request; the status bar's Settings
/// button with native decorations; and the window's minimum size handed to the host from the panels'
/// measurements.

#include "App/BackgroundOpen.h"
#include "App/PlatformOpen.h"
#include "App/SelectOverride.h"
#include "App/ShellHost.h"
#include "App/ShellLayer.h"
#include "App/StatusBarView.h"
#include "App/UserConfig.h"
#include "App/WindowOverride.h"
#include "App/WithheldOpenNotice.h"
#include "Core/ApplicationEvents.h"
#include "Core/Event.h"
#include "Core/WindowGeometry.h"
#include "Mocks/FakeClipboard.h"
#include "Platform/IHttpsClient.h"

#include <SDL3/SDL_video.h>
#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h> // FindWindowByName(), ImHashStr(), ActivateItemByID()
#include <implot.h>

#include <chrono>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <random>
#include <stop_token>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace App
{
namespace
{

constexpr float SHELL_DISPLAY_WIDTH = 1280.0F;
constexpr float SHELL_DISPLAY_HEIGHT = 800.0F;
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
    std::vector<std::string> windowCalls;  ///< "capture" and "hide", in the order onDetach() made them (#4590)
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
        m_Log.windowCalls.emplace_back("capture");
        return m_Log.window;
    }
    void hideWindowForExit() override
    {
        m_Log.windowCalls.emplace_back("hide");
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
        io.DisplaySize = ImVec2(SHELL_DISPLAY_WIDTH, SHELL_DISPLAY_HEIGHT);
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
    EXPECT_GT(main->Size.y, SHELL_DISPLAY_HEIGHT / 2.0F);
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

// #3221: the status bar's Help button raises Help, as F1 does.
TEST_F(ShellLayerRenderTest, TheStatusBarsHelpButtonRaisesOpenHelp)
{
    makeShell(false);
    frame();

    const ImGuiWindow* status = ImGui::FindWindowByName("##StatusBar");
    ASSERT_NE(status, nullptr);
    ImGui::ActivateItemByID(ImHashStr(StatusBarView::HELP_LABEL, 0, status->ID));
    frame();
    frame();
    EXPECT_EQ(count(Core::EventType::OpenHelp), 1);
}

// #3221: F6 to F9 on the Processes tab and F9 on Process Details go to the panel showing it, and
// nothing else is raised: each opens that panel's own menu or confirm, never acts at once. The panels
// here aren't attached, so each request is dropped at the frame's end.
TEST_F(ShellLayerRenderTest, TheTabShortcutsGoToThePanelShowingThem)
{
    makeShell(false);
    frame();
    for (const ImGuiKey key : {ImGuiKey_F6, ImGuiKey_F7, ImGuiKey_F8, ImGuiKey_F9})
    {
        pressKey(key);
    }
    ASSERT_TRUE(m_Shell->requestMainTab("ProcessDetails"));
    frame();
    frame();
    pressKey(ImGuiKey_F9);
    EXPECT_EQ(count(Core::EventType::OpenHelp), 0);
    EXPECT_EQ(count(Core::EventType::OpenSettings), 0);
    EXPECT_EQ(m_Log.closeRequests, 0);
}

// #3221: a tab no name matches isn't requested. The selection, tab and section requests a demo script
// makes (#2827) are handed to their panels, unattached here, and the frames after draw with them pending;
// none of them is the startup selection the screenshot hook waits for.
TEST_F(ShellLayerRenderTest, RequestsForATabSelectionOrSectionReachTheirPanels)
{
    makeShell(false);
    frame();
    EXPECT_FALSE(m_Shell->requestMainTab("No Such Tab"));
    m_Shell->requestSelection(SelectOverride::Target{.pid = 4242, .name = {}});
    m_Shell->requestDetailsTab(SelectOverride::DetailsTab::Gpu);
    m_Shell->requestMachineTab(SelectOverride::MachineTab::Cores);
    m_Shell->requestDetailsSection(SelectOverride::DetailsSection::Modules);
    frame();
    frame();
    EXPECT_FALSE(m_Shell->startupSelectionPending()) << "the demo's requests aren't the startup selection the screenshot waits for";
}

// #3717: the tabs take their panels by position in ShellPanels::inTabOrder(), so each tab's event name must still
// name the panel it shows; a reordered inTabOrder() would put a panel under another tab's name.
TEST_F(ShellLayerRenderTest, EachTabShowsThePanelItIsNamedFor)
{
    makeShell(false);
    const std::vector<std::pair<std::string, std::string>> expected{
        {"SystemOverview", "System"},
        {"Processes", "Processes"},
        {"ProcessDetails", "Process Details"},
        {"Services", "Services"},
        {"Startup", "Startup"},
        {"Connections", "Connections"},
        {"SystemInfo", "System Information"},
    };
    EXPECT_EQ(m_Shell->tabPanelsForTesting(), expected);
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
    m_Log.window = {
        .normal = Core::WindowGeometry::Rect{.x = 120, .y = 80, .width = 1111, .height = 777},
        .normalScale = 1.5F,
        .canPosition = true,
        .maximized = true,
    };
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

// #4590: the window is hidden on exit once its geometry is read, and before the panels stop, which joins their
// workers: closing with X no longer leaves a window on screen that Windows marks Not Responding.
TEST_F(ShellLayerDetachTest, TheWindowIsHiddenAfterItsGeometryIsReadAndBeforeThePanelsStop)
{
    m_Log.window = {
        .normal = Core::WindowGeometry::Rect{.x = 10, .y = 20, .width = 1000, .height = 700},
        .normalScale = 1.0F,
        .canPosition = true,
        .maximized = false,
    };
    makeShell(false);
    frame();
    m_Log.windowCalls.clear();
    m_Shell->onDetach();

    EXPECT_EQ(m_Log.windowCalls, (std::vector<std::string>{"capture", "hide"})) << "read first, hidden once, after it";
    EXPECT_EQ(UserConfig::get().settings().windowWidth, 1000) << "the geometry read before the hide is the one saved";
}

// #4590: Application::detachAllLayers() raises AppExitingEvent before any layer detaches. The shell saves what it
// reads from the window and hides it then, passes the event on (unhandled, for the panels), and its onDetach()
// later neither reads the hidden window again nor hides it twice.
TEST_F(ShellLayerDetachTest, AppExitingHidesTheWindowOnceBeforeTheDetach)
{
    m_Log.window = {
        .normal = Core::WindowGeometry::Rect{.x = 5, .y = 6, .width = 1234, .height = 567},
        .normalScale = 1.0F,
        .canPosition = true,
        .maximized = false,
    };
    makeShell(false);
    frame();
    m_Log.windowCalls.clear();

    Core::AppExitingEvent exiting;
    m_Shell->onEvent(exiting);
    EXPECT_FALSE(exiting.isHandled()) << "every layer and panel hears it";
    EXPECT_EQ(m_Log.windowCalls, (std::vector<std::string>{"capture", "hide"}));

    m_Shell->onDetach();
    EXPECT_EQ(m_Log.windowCalls, (std::vector<std::string>{"capture", "hide"})) << "nothing read from or done to it again";
    EXPECT_EQ(UserConfig::get().settings().windowWidth, 1234) << "the geometry read at the event is the one saved";
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

/// The update check's request answered here, never by the network (#3093): a newer release.
class NewerReleaseClient final : public Platform::IHttpsClient
{
  public:
    [[nodiscard]] std::expected<Platform::HttpsResponse, std::string> get(const Platform::HttpsRequest& /*request*/,
                                                                          const std::stop_token& /*stop*/) override
    {
        return Platform::HttpsResponse{
            .body = R"({"tag_name":"v99.0.0","html_url":"https://github.com/mgradwohl/tasksmack/releases/tag/v99.0.0"})",
            .status = 200,
        };
    }
};

/// One update and one frame of the shell, and the text the frame drew.
std::string updateAndCapture(ShellLayer& shell)
{
    shell.onUpdate(1.0F / 60.0F);
    ImGui::NewFrame();
    ImGui::Begin("##Capture", nullptr, ImGuiWindowFlags_NoSavedSettings);
    ImGui::LogToBuffer(); // logs what the shell's windows draw while this one is open, as the status bar's test does
    shell.onRender();
    std::string captured = GImGui->LogBuffer.c_str();
    ImGui::LogFinish();
    ImGui::End();
    ImGui::Render();
    return captured;
}

// #3093: with "Check for updates daily" on, the shell's first update starts the check, the status bar
// then offers the newer release, Settings is told what the check is doing, and the notice's X dismisses
// that version for good.
TEST_F(ShellLayerDetachTest, TheUpdateNoticeShowsAfterACheckAndCanBeDismissed)
{
    UserConfig::get().settings().updates.enabled = true;
    const auto now = std::chrono::system_clock::time_point(std::chrono::seconds(1'761'652'800));
    m_Shell = std::make_unique<ShellLayer>(
        std::make_unique<FakeShellHost>(m_Log, false), std::make_shared<NewerReleaseClient>(), [now] { return now; });

    EXPECT_FALSE(updateAndCapture(*m_Shell).contains("is available"));
    EXPECT_GE(count(Core::EventType::UpdateCheckStatus), 1); // offered, and checking
    m_Shell->updateCheckerForTesting().waitForCheck();
    static_cast<void>(updateAndCapture(*m_Shell));
    const std::string text = updateAndCapture(*m_Shell);
    EXPECT_TRUE(text.contains("TaskSmack v99.0.0 is available")) << text;
    EXPECT_EQ(UserConfig::get().settings().updates.latestVersion, "99.0.0");
    EXPECT_GE(count(Core::EventType::UpdateCheckStatus), 2); // and done
    // The title bar's update button (#3373) is lit with the same version.
    EXPECT_TRUE(m_Shell->updateButtonForTesting().active);
    EXPECT_EQ(m_Shell->updateButtonForTesting().tooltip, "TaskSmack v99.0.0 is available");

    const ImGuiWindow* status = ImGui::FindWindowByName("##StatusBar");
    ASSERT_NE(status, nullptr);
    ImGui::ActivateItemByID(ImHashStr(StatusBarView::DISMISS_UPDATE_LABEL, 0, status->ID));
    static_cast<void>(updateAndCapture(*m_Shell));
    static_cast<void>(updateAndCapture(*m_Shell));
    EXPECT_EQ(UserConfig::get().settings().updates.dismissedVersion, "99.0.0");
    EXPECT_FALSE(updateAndCapture(*m_Shell).contains("is available"));
    EXPECT_TRUE(m_Shell->updateButtonForTesting().active); // the notice is dismissed, not the news

    // Check now, from Settings: the next update starts another check.
    Core::CheckForUpdatesEvent checkNow;
    m_Shell->onEvent(checkNow);
    EXPECT_TRUE(m_Shell->updateCheckerForTesting().checking());
    static_cast<void>(updateAndCapture(*m_Shell));
    m_Shell->updateCheckerForTesting().waitForCheck();
    static_cast<void>(updateAndCapture(*m_Shell));
    EXPECT_FALSE(m_Shell->updateCheckerForTesting().checking());
}

// #3917: an elevated TaskSmack's title-bar update button whose open Windows' shell couldn't make as the user. With the
// status bar's notice dismissed (the button stays lit), the withheld release page is still named, under the title bar,
// with the Copy every withheld open has, until Close.
TEST_F(ShellLayerDetachTest, AWithheldOpenFromTheTitleBarsUpdateButtonNamesTheLinkWithACopy)
{
    TestMocks::FakeClipboard clipboard; // never the machine's own clipboard (#1890)
    clipboard.install();
    UserConfig::get().settings().updates.enabled = true;
    const auto now = std::chrono::system_clock::time_point(std::chrono::seconds(1'761'652'800));
    m_Shell = std::make_unique<ShellLayer>(
        std::make_unique<FakeShellHost>(m_Log, true), std::make_shared<NewerReleaseClient>(), [now] { return now; });
    std::vector<std::string> opened;
    m_Shell->setUpdateLinkOpenerForTesting(
        [&opened](const std::string& target)
        {
            opened.push_back(target);
            return BackgroundOpen::Job([] { return PlatformOpen::OpenResult::Withheld; }); // no browser is started
        });
    static_cast<void>(updateAndCapture(*m_Shell));
    m_Shell->updateCheckerForTesting().waitForCheck();
    static_cast<void>(updateAndCapture(*m_Shell));
    static_cast<void>(updateAndCapture(*m_Shell));
    ASSERT_TRUE(m_Shell->updateButtonForTesting().active);

    const ImGuiWindow* status = ImGui::FindWindowByName("##StatusBar");
    ASSERT_NE(status, nullptr);
    ImGui::ActivateItemByID(ImHashStr(StatusBarView::DISMISS_UPDATE_LABEL, 0, status->ID));
    static_cast<void>(updateAndCapture(*m_Shell));
    static_cast<void>(updateAndCapture(*m_Shell));
    ASSERT_FALSE(updateAndCapture(*m_Shell).contains("is available")) << "the notice is dismissed";
    EXPECT_EQ(ImGui::FindWindowByName(ShellLayer::UPDATE_WITHHELD_WINDOW), nullptr) << "nothing withheld yet";

    Core::OpenUpdatePageEvent click; // as the title bar raises it
    m_Shell->onEvent(click);
    m_Shell->waitForUpdateLinkForTesting();
    const std::string url = "https://github.com/mgradwohl/tasksmack/releases/tag/v99.0.0";
    EXPECT_EQ(opened, (std::vector<std::string>{url}));
    const std::string text = updateAndCapture(*m_Shell);
    EXPECT_TRUE(text.contains("Not opened, so that it doesn't run as administrator")) << text;
    EXPECT_TRUE(text.contains("Open it yourself: " + url)) << text;
    const ImGuiWindow* notice = ImGui::FindWindowByName(ShellLayer::UPDATE_WITHHELD_WINDOW);
    ASSERT_NE(notice, nullptr);

    ImGui::ActivateItemByID(ImHashStr(WithheldOpenNotice::COPY_LABEL, 0, notice->ID));
    static_cast<void>(updateAndCapture(*m_Shell));
    static_cast<void>(updateAndCapture(*m_Shell));
    EXPECT_EQ(clipboard.text(), url);
    EXPECT_TRUE(updateAndCapture(*m_Shell).contains("Open it yourself")) << "Copy leaves the notice up";

    ImGui::ActivateItemByID(ImHashStr(ShellLayer::UPDATE_WITHHELD_CLOSE_LABEL, 0, notice->ID));
    static_cast<void>(updateAndCapture(*m_Shell));
    static_cast<void>(updateAndCapture(*m_Shell));
    EXPECT_FALSE(updateAndCapture(*m_Shell).contains("Open it yourself")) << "Close takes it down";
}

// #3093: without a client nothing is offered: no notice, whatever config.toml says it saw.
TEST_F(ShellLayerDetachTest, WithoutAClientNoUpdateNoticeShows)
{
    UserConfig::get().settings().updates.enabled = true;
    UserConfig::get().settings().updates.latestVersion = "99.0.0";
    UserConfig::get().settings().updates.latestUrl = "https://github.com/mgradwohl/tasksmack/releases/tag/v99.0.0";
    makeShell(false);
    EXPECT_FALSE(m_Shell->updateCheckerForTesting().offered());
    EXPECT_FALSE(updateAndCapture(*m_Shell).contains("is available"));
    EXPECT_FALSE(m_Shell->updateButtonForTesting().shown); // no title-bar button either (#3373)
}

} // namespace
} // namespace App
