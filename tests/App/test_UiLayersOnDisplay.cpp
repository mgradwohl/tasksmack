/// @file test_UiLayersOnDisplay.cpp
/// @brief UI::UILayer and App::TitleBarLayer in a real Core::Application (#1395): the ImGui frame
/// UILayer brackets, the title bar TitleBarLayer draws into it, and each title-bar button's effect --
/// Settings, Help and About raise their events, Maximize and Restore change the window's state, and
/// Close stops the app. Also the minimum window size the bar sets from its contents; and the whole
/// shell as main.cpp composes it -- its real panels, each main tab, F1 and the config it saves on exit.
///
/// Needs a display with a GL 3.3 core context (Xvfb in Linux CI, where TASKSMACK_REQUIRE_DISPLAY=1
/// turns a missing one into a failure); skipped elsewhere, like the ApplicationTest suite.

#include "App/AboutLayer.h"
#include "App/ElevationNoticeLayer.h"
#include "App/HelpLayer.h"
#include "App/SettingsLayer.h"
#include "App/ShellLayer.h"
#include "App/TitleBarButtons.h"
#include "App/TitleBarLayer.h"
#include "App/UserConfig.h"
#include "Core/AnimationRequest.h"
#include "Core/Application.h"
#include "Core/ApplicationEvents.h"
#include "Core/ConfigDirOverride.h"
#include "Core/Event.h"
#include "Core/HeadlessVideoDriverTestUtils.h"
#include "Core/Layer.h"
#include "Core/Window.h"
#include "UI/IconsFontAwesome6.h"
#include "UI/RenderMetrics.h"
#include "UI/Theme.h"
#include "UI/UILayer.h"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_scancode.h>
#include <SDL3/SDL_video.h>
#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h> // FindWindowByName(), ImHashStr(), ActivateItemByID(): the bar and its buttons

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <optional>
#include <string_view>
#include <system_error>
#include <utility>

namespace
{

/// A display with a GL 3.3 core context: the real one when DISPLAY or WAYLAND_DISPLAY is set and
/// usable, else SDL's offscreen driver if Mesa EGL can back it.
bool hasDisplay()
{
#ifdef _WIN32
    // Windows CI runners are headless, as test_Application.cpp treats them; elsewhere probe for GL.
    char* ci = nullptr;
    std::size_t length = 0;
    _dupenv_s(&ci, &length, "CI");
    const bool onCi = ci != nullptr && std::string_view(ci) == "true";
    // NOLINTNEXTLINE(cppcoreguidelines-owning-memory, cppcoreguidelines-no-malloc) - _dupenv_s allocates with malloc
    free(ci);
    const bool available = !onCi && TestSupport::probeGLCapability();
#else
    // NOLINTBEGIN(concurrency-mt-unsafe) - read-only environment access during single-threaded test setup
    const char* display = std::getenv("DISPLAY");
    const char* waylandDisplay = std::getenv("WAYLAND_DISPLAY");
    // NOLINTEND(concurrency-mt-unsafe)
    const bool named = (display != nullptr && *display != '\0') || (waylandDisplay != nullptr && *waylandDisplay != '\0');
    const bool available =
        (named && TestSupport::probeGLCapability()) || (TestSupport::tryEnableOffscreenVideoDriver() && TestSupport::probeGLCapability());
#endif
    return TestSupport::enforceDisplayRequirement(available);
}

/// What the driver saw happen.
struct Observed
{
    int settings = 0;
    int help = 0;
    int about = 0;
    bool maximizedAfterMaximize = false;
    bool maximizedAfterRestore = true;
    int minimumWidth = 0;
    int minimumHeight = 0;
    int frames = 0;
    bool timedOut = false;
};

/// Pushed after UILayer and TitleBarLayer, so its onRender() runs inside the same ImGui frame, after
/// the bar is drawn: it presses one bar button every few frames (taking effect the next frame) and
/// records the outcome, then presses Close.
class TitleBarDriverLayer : public Core::Layer
{
  public:
    explicit TitleBarDriverLayer(Observed& observed) : Layer("TitleBarDriver"), m_Observed(observed)
    {}

    void onRender() override
    {
        Core::AnimationRequest::request(Core::AnimationRequest::FULL_RATE); // keep frames coming
        const int frame = ++m_Observed.frames;
        const Core::Window& window = Core::Application::get().getWindow();
        switch (frame)
        {
        case SETTLE:
            SDL_GetWindowMinimumSize(window.getHandle(), &m_Observed.minimumWidth, &m_Observed.minimumHeight);
            press(App::TitleBarButtons::SETTINGS_LABEL);
            break;
        case SETTLE + STEP:
            press(App::TitleBarButtons::HELP_LABEL);
            break;
        case SETTLE + (2 * STEP):
            press(App::TitleBarButtons::ABOUT_LABEL);
            break;
        case SETTLE + (3 * STEP):
            press(ICON_FA_WINDOW_MAXIMIZE "##Maximize");
            break;
        case SETTLE + (5 * STEP):
            m_Observed.maximizedAfterMaximize = window.isMaximized();
            press(ICON_FA_WINDOW_RESTORE "##Restore");
            break;
        case SETTLE + (7 * STEP):
            m_Observed.maximizedAfterRestore = window.isMaximized();
            // Unlabelled when the chrome icon font loaded, labelled when it fell back; one of them exists.
            press("##Close");
            press(ICON_FA_XMARK "##Close");
            break;
        default:
            if (frame > MAX_FRAMES)
            {
                m_Observed.timedOut = true;
                Core::Application::get().stop();
            }
            break;
        }
    }

    void onEvent(Core::Event& event) override
    {
        Core::EventDispatcher dispatcher(event);
        dispatcher.dispatch<Core::OpenSettingsEvent>(
            [this](Core::OpenSettingsEvent&)
            {
                ++m_Observed.settings;
                return false;
            });
        dispatcher.dispatch<Core::OpenHelpEvent>(
            [this](Core::OpenHelpEvent&)
            {
                ++m_Observed.help;
                return false;
            });
        dispatcher.dispatch<Core::OpenAboutEvent>(
            [this](Core::OpenAboutEvent&)
            {
                ++m_Observed.about;
                return false;
            });
    }

  private:
    static void press(const char* label)
    {
        const ImGuiWindow* bar = ImGui::FindWindowByName("##TitleBar");
        ASSERT_NE(bar, nullptr);
        ImGui::ActivateItemByID(ImHashStr(label, 0, bar->ID));
    }

    static constexpr int SETTLE = 10; // frames before the first press: fonts, layout and minimum size settle
    static constexpr int STEP = 10;   // frames between presses: maximize and restore may round-trip the WM
    static constexpr int MAX_FRAMES = 600;

    Observed& m_Observed;
};

TEST(UiLayersOnDisplayTest, TitleBarButtonsDoWhatTheySay)
{
    if (!hasDisplay())
    {
        GTEST_SKIP() << "No display available (headless environment)";
    }

    Core::ApplicationSpecification spec;
    spec.Name = "UiLayersOnDisplayTest";
    spec.VSync = false;
    Observed observed;
    try
    {
        Core::Application app(spec);
        app.pushLayer<UI::UILayer>();
        app.pushLayer<App::TitleBarLayer>();
        app.pushLayer<TitleBarDriverLayer>(observed);
        app.run();
    }
    catch (const std::exception& e)
    {
        FAIL() << "Application failed after the display probe passed: " << e.what();
    }

    EXPECT_FALSE(observed.timedOut) << "Close didn't stop the app";
    EXPECT_EQ(observed.settings, 1);
    EXPECT_EQ(observed.help, 1);
    EXPECT_EQ(observed.about, 1);
    EXPECT_TRUE(observed.maximizedAfterMaximize);
    EXPECT_FALSE(observed.maximizedAfterRestore);
    // The bar never lets the window get narrower than it has to show.
    EXPECT_GT(observed.minimumWidth, 0);
    EXPECT_GT(observed.minimumHeight, 0);
}

/// What the window-event driver saw.
struct WindowEventsObserved
{
    bool maximizedAfterDoubleClick = false;
    bool maximizedAfterSecondDoubleClick = true;
    bool positionKeptAfterClick = false;
    bool sizeKeptAfterEdgePress = false;
    bool systemMenuOpened = false;
    int frames = 0;
    bool timedOut = false;
};

/// Pushed after UILayer and TitleBarLayer: feeds TitleBarLayer::onSDLEvent() the SDL events a user
/// makes on the title bar -- pushed onto SDL's queue, so they arrive through Application's loop as real
/// ones do -- and records the outcome. A press that isn't followed by pointer movement must leave the
/// window where it is (the drag and resize begin, then end on release).
class WindowEventDriverLayer : public Core::Layer
{
  public:
    explicit WindowEventDriverLayer(WindowEventsObserved& observed) : Layer("WindowEventDriver"), m_Observed(observed)
    {}

    void onRender() override
    {
        Core::AnimationRequest::request(Core::AnimationRequest::FULL_RATE); // keep frames coming
        const int frame = ++m_Observed.frames;
        const Core::Window& window = Core::Application::get().getWindow();
        const auto [width, height] = window.getSize();
        const float titleX = static_cast<float>(width) * 0.5F; // empty title bar: the middle, clear of the buttons
        constexpr float TITLE_Y = 16.0F;
        switch (frame)
        {
        case SETTLE:
            pushButton(titleX, TITLE_Y, true, 2); // double-click: maximize
            pushButton(titleX, TITLE_Y, false, 2);
            break;
        case SETTLE + STEP:
            m_Observed.maximizedAfterDoubleClick = window.isMaximized();
            pushButton(titleX, TITLE_Y, true, 2); // and again: restore
            pushButton(titleX, TITLE_Y, false, 2);
            break;
        case SETTLE + (2 * STEP):
            m_Observed.maximizedAfterSecondDoubleClick = window.isMaximized();
            m_Position = window.getPosition();
            pushButton(titleX, TITLE_Y, true, 1); // a drag begins...
            break;
        case SETTLE + (2 * STEP) + 2:
            pushButton(titleX, TITLE_Y, false, 1); // ...and ends without moving
            break;
        case SETTLE + (3 * STEP):
            m_Observed.positionKeptAfterClick = window.getPosition() == m_Position;
            m_Size = window.getSize();
            pushButton(static_cast<float>(width) - 2.0F, static_cast<float>(height) * 0.5F, true, 1); // a right-edge resize begins...
            break;
        case SETTLE + (3 * STEP) + 2:
            pushButton(static_cast<float>(width) - 2.0F, static_cast<float>(height) * 0.5F, false, 1); // ...and ends
            break;
        case SETTLE + (4 * STEP):
            m_Observed.sizeKeptAfterEdgePress = window.getSize() == m_Size;
            pushKey(SDLK_SPACE, SDL_SCANCODE_SPACE, SDL_KMOD_LCTRL); // Ctrl+Space: the system menu
            break;
        case SETTLE + (5 * STEP):
            m_Observed.systemMenuOpened = !GImGui->OpenPopupStack.empty();
            Core::Application::get().stop();
            break;
        default:
            if (frame > MAX_FRAMES)
            {
                m_Observed.timedOut = true;
                Core::Application::get().stop();
            }
            break;
        }
    }

  private:
    static void pushButton(float x, float y, bool down, int clicks)
    {
        SDL_Event event{};
        event.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
        event.button.windowID = SDL_GetWindowID(Core::Application::get().getWindow().getHandle());
        event.button.button = SDL_BUTTON_LEFT;
        event.button.down = down;
        event.button.clicks = static_cast<Uint8>(clicks);
        event.button.x = x;
        event.button.y = y;
        static_cast<void>(SDL_PushEvent(&event));
    }

    static void pushKey(SDL_Keycode key, SDL_Scancode scancode, SDL_Keymod mod)
    {
        SDL_Event event{};
        event.type = SDL_EVENT_KEY_DOWN;
        event.key.windowID = SDL_GetWindowID(Core::Application::get().getWindow().getHandle());
        event.key.key = key;
        event.key.scancode = scancode; // as a real key event carries; ImGui's SDL backend expects one
        event.key.mod = mod;
        event.key.down = true;
        static_cast<void>(SDL_PushEvent(&event));
    }

    static constexpr int SETTLE = 10;
    static constexpr int STEP = 10;
    static constexpr int MAX_FRAMES = 600;

    WindowEventsObserved& m_Observed;
    std::pair<int, int> m_Position;
    std::pair<int, int> m_Size;
};

TEST(UiLayersOnDisplayTest, TitleBarMouseAndKeyEventsDoWhatTheySay)
{
    if (!hasDisplay())
    {
        GTEST_SKIP() << "No display available (headless environment)";
    }

    Core::ApplicationSpecification spec;
    spec.Name = "UiLayersOnDisplayEventsTest";
    spec.VSync = false;
    WindowEventsObserved observed;
    try
    {
        Core::Application app(spec);
        app.pushLayer<UI::UILayer>();
        app.pushLayer<App::TitleBarLayer>();
        app.pushLayer<WindowEventDriverLayer>(observed);
        app.run();
    }
    catch (const std::exception& e)
    {
        FAIL() << "Application failed after the display probe passed: " << e.what();
    }

    EXPECT_FALSE(observed.timedOut);
    EXPECT_TRUE(observed.maximizedAfterDoubleClick);        // double-click on the empty title bar maximizes
    EXPECT_FALSE(observed.maximizedAfterSecondDoubleClick); // and again restores
    EXPECT_TRUE(observed.positionKeptAfterClick);           // a press and release without motion moves nothing
    EXPECT_TRUE(observed.sizeKeptAfterEdgePress);           // likewise on the resize edge
    EXPECT_TRUE(observed.systemMenuOpened);                 // Ctrl+Space opens the window menu
}

/// What the UI-layer driver saw.
struct UiLayerObserved
{
    bool userThemeLoaded = false;
    int drawCalls = 0;
    int frames = 0;
};

/// Pushed after UILayer: a display-scale event (noted, then re-measured at the next frame boundary;
/// a headless display's scale doesn't change, so nothing is rebuilt), then what UILayer loaded and
/// counted.
class UiLayerDriverLayer : public Core::Layer
{
  public:
    explicit UiLayerDriverLayer(UiLayerObserved& observed) : Layer("UiLayerDriver"), m_Observed(observed)
    {}

    void onRender() override
    {
        Core::AnimationRequest::request(Core::AnimationRequest::FULL_RATE); // keep frames coming
        // Something to draw, so each frame has draw calls to count.
        ImGui::Begin("UiLayerDriver");
        ImGui::TextUnformatted("frame");
        ImGui::End();
        const int frame = ++m_Observed.frames;
        if (frame == 5)
        {
            SDL_Event event{};
            event.type = SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED;
            event.window.windowID = SDL_GetWindowID(Core::Application::get().getWindow().getHandle());
            static_cast<void>(SDL_PushEvent(&event));
        }
        else if (frame == 15)
        {
            const auto& themes = UI::Theme::get().discoveredThemes();
            m_Observed.userThemeLoaded =
                std::ranges::any_of(themes, [](const UI::DiscoveredTheme& theme) { return theme.id == "app-tests-user-theme"; });
            m_Observed.drawCalls = UI::RenderMetrics::get().drawCalls();
            Core::Application::get().stop();
        }
    }

  private:
    UiLayerObserved& m_Observed;
};

TEST(UiLayersOnDisplayTest, UiLayerLoadsUserThemesAndCountsDrawCalls)
{
    if (!hasDisplay())
    {
        GTEST_SKIP() << "No display available (headless environment)";
    }
    // A user theme beside config.toml (AppTestsMain.cpp's temporary directory): merged over the
    // built-ins by its file name (#1127).
    const std::optional<std::filesystem::path>& configDir = Core::ConfigDirOverride::active();
    ASSERT_TRUE(configDir.has_value());
    const std::filesystem::path userThemes = configDir.value_or(std::filesystem::path{}) / "themes";
    std::error_code ec;
    std::filesystem::create_directories(userThemes, ec);
    ASSERT_FALSE(ec);
    std::filesystem::copy_file(std::filesystem::path(TASKSMACK_SOURCE_THEMES_DIR) / "arctic-fire.toml",
                               userThemes / "app-tests-user-theme.toml",
                               std::filesystem::copy_options::overwrite_existing,
                               ec);
    ASSERT_FALSE(ec) << ec.message();
    UI::RenderMetrics::get().setEnabled(true);

    Core::ApplicationSpecification spec;
    spec.Name = "UiLayersOnDisplayUiLayerTest";
    spec.VSync = false;
    UiLayerObserved observed;
    try
    {
        Core::Application app(spec);
        app.pushLayer<UI::UILayer>();
        app.pushLayer<UiLayerDriverLayer>(observed);
        app.run();
    }
    catch (const std::exception& e)
    {
        UI::RenderMetrics::get().setEnabled(false);
        FAIL() << "Application failed after the display probe passed: " << e.what();
    }
    UI::RenderMetrics::get().setEnabled(false);
    std::filesystem::remove_all(userThemes, ec);

    EXPECT_TRUE(observed.userThemeLoaded);
    EXPECT_GT(observed.drawCalls, 0); // counted after each Render(), while the overlay's metrics are on
}

/// The tab IDs pressed, in order, then back to the first (TabLabel.h, ShellLayer.cpp's labels).
constexpr std::array<const char*, 7> TABS{
    "###ProcessesTab", "###ProcessDetailsTab", "###ServicesTab", "###StartupTab", "###SystemInfoTab", "###SystemTab", "###ProcessesTab"};

/// What the shell driver saw.
struct ShellObserved
{
    std::array<bool, TABS.size()> tabShown{};
    int help = 0;
    int frames = 0;
    bool timedOut = false;
};

/// Pushed last over the whole shell: presses each main tab in turn, checks the tab bar selected it, then
/// presses F1 and finally the title bar's Close.
class ShellDriverLayer : public Core::Layer
{
  public:
    explicit ShellDriverLayer(ShellObserved& observed) : Layer("ShellDriver"), m_Observed(observed)
    {}

    void onRender() override
    {
        Core::AnimationRequest::request(Core::AnimationRequest::FULL_RATE); // keep frames coming
        const int frame = ++m_Observed.frames;
        if (frame > MAX_FRAMES)
        {
            m_Observed.timedOut = true;
            Core::Application::get().stop();
            return;
        }
        if (frame < SETTLE || (frame - SETTLE) % STEP != 0)
        {
            return;
        }
        const auto step = static_cast<std::size_t>((frame - SETTLE) / STEP);
        if (step > 0 && step <= TABS.size())
        {
            m_Observed.tabShown[step - 1] = selectedTab() == tabId(TABS[step - 1]);
        }
        if (step < TABS.size())
        {
            ImGui::ActivateItemByID(tabId(TABS[step]));
        }
        else if (step == TABS.size())
        {
            ImGui::GetIO().AddKeyEvent(ImGuiKey_F1, true);
        }
        else if (step == TABS.size() + 1)
        {
            ImGui::GetIO().AddKeyEvent(ImGuiKey_F1, false);
            pressOnTitleBar("##Close");
            pressOnTitleBar(ICON_FA_XMARK "##Close");
        }
    }

    void onEvent(Core::Event& event) override
    {
        Core::EventDispatcher dispatcher(event);
        dispatcher.dispatch<Core::OpenHelpEvent>(
            [this](Core::OpenHelpEvent&)
            {
                ++m_Observed.help;
                return false;
            });
    }

  private:
    [[nodiscard]] static ImGuiID tabBarId()
    {
        const ImGuiWindow* main = ImGui::FindWindowByName("##MainWindow");
        return main != nullptr ? ImHashStr("##MainTabBar", 0, main->ID) : 0;
    }

    [[nodiscard]] static ImGuiID tabId(const char* label)
    {
        return ImHashStr(label, 0, tabBarId());
    }

    [[nodiscard]] static ImGuiID selectedTab()
    {
        const ImGuiTabBar* bar = GImGui->TabBars.GetByKey(tabBarId());
        return bar != nullptr ? bar->SelectedTabId : 0;
    }

    static void pressOnTitleBar(const char* label)
    {
        if (const ImGuiWindow* bar = ImGui::FindWindowByName("##TitleBar"); bar != nullptr)
        {
            ImGui::ActivateItemByID(ImHashStr(label, 0, bar->ID));
        }
    }

    static constexpr int SETTLE = 20; // the panels' first samples and layout
    static constexpr int STEP = 15;   // frames per tab: the press lands, the tab draws a few frames
    static constexpr int MAX_FRAMES = 1200;

    ShellObserved& m_Observed;
};

TEST(UiLayersOnDisplayTest, TheWholeShellShowsEachTabAndSavesItsConfigOnClose)
{
    if (!hasDisplay())
    {
        GTEST_SKIP() << "No display available (headless environment)";
    }
    // Never the user's own config: AppTestsMain.cpp points TASKSMACK_CONFIG_DIR at a temporary
    // directory before any test, and the save on close must land there.
    const std::optional<std::filesystem::path>& configDir = Core::ConfigDirOverride::active();
    ASSERT_TRUE(configDir.has_value());
    ASSERT_EQ(App::UserConfig::get().configPath().parent_path(), configDir.value_or(std::filesystem::path{}));

    Core::ApplicationSpecification spec;
    spec.Name = "UiLayersOnDisplayShellTest";
    spec.VSync = false;
    ShellObserved observed;
    try
    {
        Core::Application app(spec);
        app.pushLayer<UI::UILayer>();
        auto& titleBar = app.pushLayer<App::TitleBarLayer>();
        app.pushLayer<App::ShellLayer>().setTitleBar(&titleBar);
        app.pushLayer<App::HelpLayer>();
        app.pushLayer<App::AboutLayer>();
        app.pushLayer<App::SettingsLayer>();
        app.pushLayer<App::ElevationNoticeLayer>();
        app.pushLayer<ShellDriverLayer>(observed);
        app.run();
    }
    catch (const std::exception& e)
    {
        FAIL() << "Application failed after the display probe passed: " << e.what();
    }

    EXPECT_FALSE(observed.timedOut) << "Close didn't stop the app";
    for (std::size_t i = 0; i < TABS.size(); ++i)
    {
        EXPECT_TRUE(observed.tabShown[i]) << TABS[i];
    }
    EXPECT_EQ(observed.help, 1); // F1
    std::error_code ec;
    EXPECT_TRUE(
        std::filesystem::exists(configDir.value_or(std::filesystem::path{}) / "config.toml", ec)); // saved by ShellLayer::onDetach()
}

} // namespace
