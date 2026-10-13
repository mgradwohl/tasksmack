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
#include <cstdint>
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
            // Pressed on separate frames: ActivateItemByID() keeps only the last request of a frame, so
            // pressing both at once only ever pressed the labelled one (#1954).
            press("##Close");
            break;
        case SETTLE + (7 * STEP) + 1:
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
        if (frame > MAX_FRAMES)
        {
            m_Observed.timedOut = true;
            Core::Application::get().stop();
            return;
        }
        if (frame < m_NextFrame)
        {
            return;
        }
        const Core::Window& window = Core::Application::get().getWindow();
        const auto [width, height] = window.getSize();
        const float titleX = static_cast<float>(width) * 0.5F; // empty title bar: the middle, clear of the buttons
        constexpr float TITLE_Y = 16.0F;
        const float edgeX = static_cast<float>(width) - 2.0F;
        const float edgeY = static_cast<float>(height) * 0.5F;
        const auto next = [this, frame](Phase phase, int frames)
        {
            m_Phase = phase;
            m_NextFrame = frame + frames;
        };
        switch (m_Phase)
        {
        case Phase::Maximize:
            pushButton(titleX, TITLE_Y, true, 2); // double-click: maximize
            pushButton(titleX, TITLE_Y, false, 2);
            next(Phase::Restore, STEP);
            break;
        case Phase::Restore:
            m_Observed.maximizedAfterDoubleClick = window.isMaximized();
            pushButton(titleX, TITLE_Y, true, 2); // and again: restore
            pushButton(titleX, TITLE_Y, false, 2);
            m_SettleDeadline = frame + SETTLE_LIMIT;
            next(Phase::AwaitRestored, STEP);
            break;
        case Phase::AwaitRestored:
        {
            // On X11 the restore is a request: the window manager un-maximizes the window and moves it
            // back some frames later, later still under load. Wait until it is restored and has kept its
            // position for STABLE_FRAMES frames, so the click below is the only thing that could move it
            // (#1907). A window that never settles goes on at SETTLE_LIMIT and fails the checks.
            m_Observed.maximizedAfterSecondDoubleClick = window.isMaximized();
            const auto position = window.getPosition();
            m_StableFrames = (!m_Observed.maximizedAfterSecondDoubleClick && position == m_Position) ? m_StableFrames + 1 : 0;
            m_Position = position;
            if (m_StableFrames < STABLE_FRAMES && frame < m_SettleDeadline)
            {
                next(Phase::AwaitRestored, 1);
                break;
            }
            pushButton(titleX, TITLE_Y, true, 1); // a drag begins...
            next(Phase::EndDrag, 2);
            break;
        }
        case Phase::EndDrag:
            pushButton(titleX, TITLE_Y, false, 1); // ...and ends without moving
            next(Phase::CheckPosition, STEP - 2);
            break;
        case Phase::CheckPosition:
            m_Observed.positionKeptAfterClick = window.getPosition() == m_Position;
            m_Size = window.getSize();
            pushButton(edgeX, edgeY, true, 1); // a right-edge resize begins...
            next(Phase::EndResize, 2);
            break;
        case Phase::EndResize:
            pushButton(edgeX, edgeY, false, 1); // ...and ends
            next(Phase::CheckSize, STEP - 2);
            break;
        case Phase::CheckSize:
            m_Observed.sizeKeptAfterEdgePress = window.getSize() == m_Size;
            pushKey(SDLK_SPACE, SDL_SCANCODE_SPACE, SDL_KMOD_LCTRL); // Ctrl+Space: the system menu
            next(Phase::CheckMenu, STEP);
            break;
        case Phase::CheckMenu:
            m_Observed.systemMenuOpened = !GImGui->OpenPopupStack.empty();
            Core::Application::get().stop();
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

    /// The steps, in order; each waits m_NextFrame before running.
    enum class Phase : std::uint8_t
    {
        Maximize,
        Restore,
        AwaitRestored,
        EndDrag,
        CheckPosition,
        EndResize,
        CheckSize,
        CheckMenu,
    };

    static constexpr int SETTLE = 10;
    static constexpr int STEP = 10;
    static constexpr int STABLE_FRAMES = 5;  // frames the restored window must hold still
    static constexpr int SETTLE_LIMIT = 300; // frames to wait for that before going on
    static constexpr int MAX_FRAMES = 900;

    WindowEventsObserved& m_Observed;
    Phase m_Phase = Phase::Maximize;
    int m_NextFrame = SETTLE;
    int m_SettleDeadline = 0;
    int m_StableFrames = 0;
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

/// What the shell-shortcuts driver saw.
struct ShellKeysObserved
{
    bool detailsTabShown = false;
    bool servicesTabShown = false; // #1886: a ShowServiceEvent brings the Services tab forward
    bool metricsOnAfterToggle = false;
    bool metricsOffAfterSecondToggle = true;
    bool fontGrew = false;
    bool fontShrankBack = false;
    int frames = 0;
    bool timedOut = false;
};

/// Pushed last over the whole shell: the row menu's Details for a process (as
/// ProcessesPanel raises it), F5 on the Processes tab, Ctrl+Shift+M twice, then Ctrl+= and Ctrl+-.
class ShellKeysDriverLayer : public Core::Layer
{
  public:
    explicit ShellKeysDriverLayer(ShellKeysObserved& observed) : Layer("ShellKeysDriver"), m_Observed(observed)
    {}

    void onRender() override
    {
        Core::AnimationRequest::request(Core::AnimationRequest::FULL_RATE); // keep frames coming
        ImGuiIO& io = ImGui::GetIO();
        const int frame = ++m_Observed.frames;
        if (frame > MAX_FRAMES)
        {
            m_Observed.timedOut = true;
            Core::Application::get().stop();
            return;
        }
        if (frame < m_NextFrame)
        {
            return;
        }
        const auto next = [this, frame](Phase phase, int frames)
        {
            m_Phase = phase;
            m_NextFrame = frame + frames;
        };
        // After a key's release, wait for its effect rather than a fixed number of frames: on a loaded
        // runner the effect can take longer to show (#1917). A press that is lost still fails, at
        // WAIT_LIMIT; one taken twice fails the checks that the state came back.
        // A press whose effect hasn't shown after RETRY_AFTER frames is pressed again, up to MAX_RETRIES
        // times (#1950): CI's xvfb drops an injected key now and then (only a Ctrl+= was logged, never
        // the Ctrl+- after it), as #1845's button presses were. A press taken twice still fails the
        // checks that the state came back.
        const auto awaitThen = [&](bool done, Phase then, Phase pressAgain)
        {
            if (!done && frame >= m_RetryFrame && m_Retries < MAX_RETRIES)
            {
                ++m_Retries;
                next(pressAgain, 1);
                return false;
            }
            if (!done && frame < m_WaitDeadline)
            {
                next(m_Phase, 1);
                return false;
            }
            m_Retries = 0;
            next(then, STEP);
            return true;
        };
        const auto startWait = [&]
        {
            m_WaitDeadline = frame + WAIT_LIMIT;
            m_RetryFrame = frame + RETRY_AFTER;
        };
        const auto pressCtrl = [&io](ImGuiKey key, bool shift, bool down)
        {
            io.AddKeyEvent(ImGuiMod_Ctrl, down);
            if (shift)
            {
                io.AddKeyEvent(ImGuiMod_Shift, down);
            }
            io.AddKeyEvent(key, down);
        };
        switch (m_Phase)
        {
        case Phase::ShowDetails:
        {
            // As the Processes table's row menu does it (#1209): the selection first, then Details. PID 1
            // is init on Linux; elsewhere it isn't listed, which Process Details says, and the tab comes
            // forward either way.
            Core::ProcessSelectedEvent selected(1, 0);
            Core::Application::get().raiseEvent(selected);
            Core::ShowProcessDetailsEvent details;
            Core::Application::get().raiseEvent(details);
            next(Phase::CheckDetails, 15);
            break;
        }
        case Phase::CheckDetails:
        {
            m_Observed.detailsTabShown = selectedTab() == tabId("###ProcessDetailsTab");
            // Process Details' Services section asks for a service as this does (#1886, #1891).
            Core::ShowServiceEvent show("cron.service");
            Core::Application::get().raiseEvent(show);
            next(Phase::CheckServices, 15);
            break;
        }
        case Phase::CheckServices:
            m_Observed.servicesTabShown = selectedTab() == tabId("###ServicesTab");
            ImGui::ActivateItemByID(tabId("###ProcessesTab"));
            next(Phase::PressF5, STEP);
            break;
        case Phase::PressF5:
            // No key repeat: these shortcuts are read with IsKeyPressed(), which repeats a key held past
            // KeyRepeatDelay, and one slow frame between a press and its release on a loaded runner did
            // that -- Ctrl+= grew the font two presets and Ctrl+- shrank it one (fontShrankBack failed).
            io.KeyRepeatDelay = 3600.0F;
            io.AddKeyEvent(ImGuiKey_F5, true); // the list/tree toggle, routed to the Processes tab
            next(Phase::ReleaseF5, 1);
            break;
        case Phase::ReleaseF5:
            io.AddKeyEvent(ImGuiKey_F5, false);
            next(Phase::PressMetricsOn, STEP);
            break;
        case Phase::PressMetricsOn:
        case Phase::PressMetricsOff:
            pressCtrl(ImGuiKey_M, true, true);
            next(m_Phase == Phase::PressMetricsOn ? Phase::ReleaseMetricsOn : Phase::ReleaseMetricsOff, 1);
            break;
        case Phase::ReleaseMetricsOn:
        case Phase::ReleaseMetricsOff:
            pressCtrl(ImGuiKey_M, true, false);
            startWait();
            next(m_Phase == Phase::ReleaseMetricsOn ? Phase::AwaitMetricsOn : Phase::AwaitMetricsOff, 1);
            break;
        case Phase::AwaitMetricsOn:
            m_Observed.metricsOnAfterToggle = UI::RenderMetrics::get().enabled();
            static_cast<void>(awaitThen(m_Observed.metricsOnAfterToggle, Phase::PressMetricsOff, Phase::PressMetricsOn));
            break;
        case Phase::AwaitMetricsOff:
            m_Observed.metricsOffAfterSecondToggle = UI::RenderMetrics::get().enabled();
            static_cast<void>(awaitThen(!m_Observed.metricsOffAfterSecondToggle, Phase::PressGrow, Phase::PressMetricsOff));
            break;
        case Phase::PressGrow:
            if (m_Retries == 0)
            {
                m_FontBefore = UI::Theme::get().currentFontSize(); // not again on a retry
            }
            pressCtrl(ImGuiKey_Equal, false, true);
            next(Phase::ReleaseGrow, 1);
            break;
        case Phase::ReleaseGrow:
            pressCtrl(ImGuiKey_Equal, false, false);
            startWait();
            next(Phase::AwaitGrow, 1);
            break;
        case Phase::AwaitGrow:
            m_Observed.fontGrew = UI::Theme::get().currentFontSize() != m_FontBefore;
            static_cast<void>(awaitThen(m_Observed.fontGrew, Phase::PressShrink, Phase::PressGrow));
            break;
        case Phase::PressShrink:
            pressCtrl(ImGuiKey_Minus, false, true);
            next(Phase::ReleaseShrink, 1);
            break;
        case Phase::ReleaseShrink:
            pressCtrl(ImGuiKey_Minus, false, false);
            startWait();
            next(Phase::AwaitShrink, 1);
            break;
        case Phase::AwaitShrink:
            m_Observed.fontShrankBack = UI::Theme::get().currentFontSize() == m_FontBefore;
            if (awaitThen(m_Observed.fontShrankBack, Phase::Done, Phase::PressShrink))
            {
                Core::Application::get().stop();
            }
            break;
        case Phase::Done:
            break;
        }
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

    /// The steps, in order; each waits for m_NextFrame.
    enum class Phase : std::uint8_t
    {
        ShowDetails,
        CheckDetails,
        CheckServices,
        PressF5,
        ReleaseF5,
        PressMetricsOn,
        ReleaseMetricsOn,
        AwaitMetricsOn,
        PressMetricsOff,
        ReleaseMetricsOff,
        AwaitMetricsOff,
        PressGrow,
        ReleaseGrow,
        AwaitGrow,
        PressShrink,
        ReleaseShrink,
        AwaitShrink,
        Done,
    };

    static constexpr int STEP = 10;
    static constexpr int WAIT_LIMIT = 120; // frames to wait for a shortcut's effect
    static constexpr int RETRY_AFTER = 40; // frames without it before the key is pressed again
    static constexpr int MAX_RETRIES = 3;
    static constexpr int MAX_FRAMES = 1500;

    ShellKeysObserved& m_Observed;
    Phase m_Phase = Phase::ShowDetails;
    int m_NextFrame = 20;
    int m_WaitDeadline = 0;
    int m_RetryFrame = 0;
    int m_Retries = 0;
    UI::FontSize m_FontBefore = UI::FontSize::Medium;
};

TEST(UiLayersOnDisplayTest, TheShellsShortcutsAndTheDetailsRequestDoWhatTheySay)
{
    if (!hasDisplay())
    {
        GTEST_SKIP() << "No display available (headless environment)";
    }
    Core::ApplicationSpecification spec;
    spec.Name = "UiLayersOnDisplayShellKeysTest";
    spec.VSync = false;
    ShellKeysObserved observed;
    try
    {
        Core::Application app(spec);
        app.pushLayer<UI::UILayer>();
        auto& titleBar = app.pushLayer<App::TitleBarLayer>();
        app.pushLayer<App::ShellLayer>().setTitleBar(&titleBar);
        app.pushLayer<ShellKeysDriverLayer>(observed);
        app.run();
    }
    catch (const std::exception& e)
    {
        FAIL() << "Application failed after the display probe passed: " << e.what();
    }
    UI::RenderMetrics::get().setEnabled(false);

    EXPECT_FALSE(observed.timedOut);
    EXPECT_TRUE(observed.detailsTabShown);              // Details brings the Process Details tab forward
    EXPECT_TRUE(observed.servicesTabShown);             // Show in Services brings the Services tab forward
    EXPECT_TRUE(observed.metricsOnAfterToggle);         // Ctrl+Shift+M turns the render metrics on...
    EXPECT_FALSE(observed.metricsOffAfterSecondToggle); // ...and off again
    EXPECT_TRUE(observed.fontGrew);                     // Ctrl+= one preset larger
    EXPECT_TRUE(observed.fontShrankBack);               // Ctrl+- back
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
        afterTabs(frame);
        if (frame < SETTLE || (frame - SETTLE) % STEP != 0 || m_F1ReleaseFrame != 0)
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
            m_F1ReleaseFrame = frame + 1;
        }
    }

    /// F1 is released on the frame after its press, then Close waits for its effect (the help request)
    /// rather than a fixed number of frames, up to WAIT_LIMIT: on a loaded runner the shell can take
    /// longer to answer (#1917). A press that is lost still fails the help count.
    void afterTabs(int frame)
    {
        if (m_F1ReleaseFrame == 0 || frame < m_F1ReleaseFrame)
        {
            return;
        }
        if (frame == m_F1ReleaseFrame)
        {
            ImGui::GetIO().AddKeyEvent(ImGuiKey_F1, false);
            return;
        }
        if (m_Observed.help == 0 && frame < m_F1ReleaseFrame + WAIT_LIMIT)
        {
            return;
        }
        // The unlabelled Close (chrome icon font) on one frame and the labelled one on the next: one
        // ActivateItemByID() request per frame is kept (#1954).
        if (m_ClosePresses == 0)
        {
            pressOnTitleBar("##Close");
            ++m_ClosePresses;
        }
        else if (m_ClosePresses == 1)
        {
            pressOnTitleBar(ICON_FA_XMARK "##Close");
            ++m_ClosePresses;
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
    static constexpr int WAIT_LIMIT = 120; // frames Close waits for F1's help request

    ShellObserved& m_Observed;
    int m_F1ReleaseFrame = 0; // 0 until F1 is pressed
    int m_ClosePresses = 0;
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
