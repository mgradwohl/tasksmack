/// @file test_UiLayersOnDisplay.cpp
/// @brief UI::UILayer and App::TitleBarLayer in a real Core::Application (#1395): the ImGui frame
/// UILayer brackets, the title bar TitleBarLayer draws into it, and each title-bar button's effect --
/// Settings, Help and About raise their events, Maximize and Restore change the window's state, and
/// Close stops the app. Also the minimum window size the bar sets from its contents, the drags and
/// resizes the bar runs client-side, from every edge and corner (#2717); and the whole
/// shell as main.cpp composes it -- its real panels, each main tab, F1 and the config it saves on exit --
/// and a first launch from an empty config directory, whose saved config.toml is a golden file (#2727).
///
/// Needs a display with a GL 3.3 core context (Xvfb in Linux CI, where TASKSMACK_REQUIRE_DISPLAY=1
/// turns a missing one into a failure); skipped elsewhere, like the ApplicationTest suite.

#include "App/AboutDialog.h"
#include "App/AboutLayer.h"
#include "App/ElevationNoticeLayer.h"
#include "App/HelpLayer.h"
#include "App/ProcessColumnConfig.h"
#include "App/SettingsLayer.h"
#include "App/ShellLayer.h"
#include "App/TitleBarButtons.h"
#include "App/TitleBarGeometry.h"
#include "App/TitleBarLayer.h"
#include "App/UserConfig.h"
#include "App/WindowInteraction.h"
#include "Core/AnimationRequest.h"
#include "Core/Application.h"
#include "Core/ApplicationEvents.h"
#include "Core/ConfigDirOverride.h"
#include "Core/Event.h"
#include "Core/HeadlessVideoDriverTestUtils.h"
#include "Core/Layer.h"
#include "Core/Utf8Path.h"
#include "Core/VideoBackend.h"
#include "Core/Window.h"
#include "Core/WindowConstants.h"
#include "Core/WindowGeometry.h"
#include "UI/IconTextureCache.h"
#include "UI/IconsFontAwesome6.h"
#include "UI/RenderMetrics.h"
#include "UI/Theme.h"
#include "UI/UILayer.h"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_scancode.h>
#include <SDL3/SDL_video.h>
#include <glad/gl.h>
#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h> // FindWindowByName(), ImHashStr(), ActivateItemByID(): the bar and its buttons
#include <toml++/toml.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace
{

/// A display with a GL 3.3 core context: the real one when DISPLAY or WAYLAND_DISPLAY is set and
/// usable, else SDL's offscreen driver if Mesa EGL can back it.
bool hasDisplay()
{
#ifdef _WIN32
    // A GL 3.3 core context on the desktop, CI included (#2134).
    const bool available = TestSupport::detectWindowsDisplay();
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

/// For a driver that presses keys through ImGuiIO: a focus loss no longer drops its presses (#2268).
/// Under Xvfb there's no window manager, so SDL gives each window it shows the X input focus, and a test
/// app that ctest -j opens alongside this one takes it from ours. ImGui clears every key after the frame
/// that takes a focus loss in, so an F1 pressed on that frame was never seen and Help never opened. The
/// presses here come from the driver, not the keyboard, so they're the same whichever window has focus.
void keepKeyPressesThroughFocusLoss()
{
    ImGui::GetIO().ConfigDebugIgnoreFocusLoss = true;
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
#ifdef _WIN32
    LRESULT titleHitTest = HTNOWHERE; ///< What the window answered WM_NCHITTEST on the empty title bar...
    LRESULT edgeHitTest = HTNOWHERE;  ///< ...and on its right edge
#endif
    int frames = 0;
    bool timedOut = false;
};

/// Pushed after UILayer and TitleBarLayer, so its onRender() runs inside the same ImGui frame, after
/// the bar is drawn: it presses one bar button every few frames (taking effect the next frame) and
/// records the outcome, then presses Close. Maximize and Restore wait for the window's state, up to
/// WAIT_LIMIT frames: the window manager may apply either some frames later (#2054).
class TitleBarDriverLayer : public Core::Layer
{
  public:
    explicit TitleBarDriverLayer(Observed& observed) : Layer("TitleBarDriver"), m_Observed(observed)
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
        const auto next = [this, frame](Phase phase, int frames)
        {
            m_Phase = phase;
            m_NextFrame = frame + frames;
        };
        const auto waiting = [this, frame]
        {
            return frame < m_Deadline;
        };
        switch (m_Phase)
        {
        case Phase::Settings:
            SDL_GetWindowMinimumSize(window.getHandle(), &m_Observed.minimumWidth, &m_Observed.minimumHeight);
#ifdef _WIN32
            hitTest(window);
#endif
            press(App::TitleBarButtons::SETTINGS_LABEL);
            next(Phase::Help, STEP);
            break;
        case Phase::Help:
            press(App::TitleBarButtons::HELP_LABEL);
            next(Phase::About, STEP);
            break;
        case Phase::About:
            press(App::TitleBarButtons::ABOUT_LABEL);
            next(Phase::Maximize, STEP);
            break;
        case Phase::Maximize:
            press(ICON_FA_WINDOW_MAXIMIZE "##Maximize");
            m_Deadline = frame + WAIT_LIMIT;
            next(Phase::AwaitMaximized, 1);
            break;
        case Phase::AwaitMaximized:
            m_Observed.maximizedAfterMaximize = window.isMaximized();
            if (!m_Observed.maximizedAfterMaximize && waiting())
            {
                next(Phase::AwaitMaximized, 1);
                break;
            }
            press(ICON_FA_WINDOW_RESTORE "##Restore");
            m_Deadline = frame + WAIT_LIMIT;
            next(Phase::AwaitRestored, 1);
            break;
        case Phase::AwaitRestored:
            m_Observed.maximizedAfterRestore = window.isMaximized();
            if (m_Observed.maximizedAfterRestore && waiting())
            {
                next(Phase::AwaitRestored, 1);
                break;
            }
            // Unlabelled when the chrome icon font loaded, labelled when it fell back; one of them exists.
            // Pressed on separate frames: ActivateItemByID() keeps only the last request of a frame, so
            // pressing both at once only ever pressed the labelled one (#1954).
            press("##Close");
            next(Phase::CloseLabelled, 1);
            break;
        case Phase::CloseLabelled:
            press(ICON_FA_XMARK "##Close");
            next(Phase::Closing, MAX_FRAMES); // the app stops; MAX_FRAMES fails the test if it doesn't
            break;
        case Phase::Closing:
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

#ifdef _WIN32
    /// Asks the test's own window what lies under two of its points, as Windows does when the pointer
    /// moves: the window procedure hands WM_NCHITTEST to TitleBarLayer's hit-test callback. Sent to
    /// this window alone, and only a question: nothing is clicked or moved.
    void hitTest(const Core::Window& window)
    {
        const SDL_PropertiesID props = SDL_GetWindowProperties(window.getHandle());
        auto* hwnd = static_cast<HWND>(SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
        ASSERT_NE(hwnd, nullptr);
        RECT client{};
        ASSERT_TRUE(GetClientRect(hwnd, &client));
        const auto ask = [hwnd](LONG x, LONG y)
        {
            POINT point{.x = x, .y = y};
            ClientToScreen(hwnd, &point);
            return SendMessageW(hwnd, WM_NCHITTEST, 0, MAKELPARAM(point.x, point.y));
        };
        m_Observed.titleHitTest = ask(client.right / 2, TITLE_Y);          // the empty title bar: the OS's caption (#2716)
        m_Observed.edgeHitTest = ask(client.right - 2, client.bottom / 2); // the right edge: the app's own resize
    }
#endif

    /// The steps, in order; each waits m_NextFrame before running.
    enum class Phase : std::uint8_t
    {
        Settings,
        Help,
        About,
        Maximize,
        AwaitMaximized,
        AwaitRestored,
        CloseLabelled,
        Closing,
    };

    static constexpr int SETTLE = 10;      // frames before the first press: fonts, layout and minimum size settle
    static constexpr int STEP = 10;        // frames between presses
    static constexpr int WAIT_LIMIT = 300; // frames Maximize and Restore each wait for the window's state
    static constexpr int MAX_FRAMES = 900;
#ifdef _WIN32
    static constexpr LONG TITLE_Y = 16;
#endif

    Observed& m_Observed;
    Phase m_Phase = Phase::Settings;
    int m_NextFrame = SETTLE;
    int m_Deadline = 0;
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
#ifdef _WIN32
    // On Windows the empty title bar is the OS's caption (#2716): a press there runs Windows' own
    // move loop, which is where Aero Snap lives. The edges stay client area: TitleBarLayer resizes
    // the window itself, so Windows never enters its modal resize loop and the frames keep coming.
    EXPECT_EQ(observed.titleHitTest, HTCAPTION);
    EXPECT_EQ(observed.edgeHitTest, HTCLIENT);
#endif
}

/// What the display-scale driver saw.
struct DisplayScaleObserved
{
    float measured = 0.0F;
    float afterRebuild = 0.0F;
    std::uint64_t fontGenerationBefore = 0;
    std::uint64_t fontGenerationAfter = 0;
    int frames = 0;
    bool timedOut = false;
};

/// Pushed after UILayer: once the frame has settled, records the scale UILayer measured, moves the
/// Theme's scale off it and queues SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED, as dragging the window to a
/// differently scaled monitor does (#943). UILayer rebuilds the fonts and style at the next frame
/// boundary, back at the measured scale; then it stops the app.
class DisplayScaleDriverLayer : public Core::Layer
{
  public:
    explicit DisplayScaleDriverLayer(DisplayScaleObserved& observed) : Layer("DisplayScaleDriver"), m_Observed(observed)
    {}

    void onRender() override
    {
        Core::AnimationRequest::request(Core::AnimationRequest::FULL_RATE); // keep frames coming
        const int frame = ++m_Observed.frames;
        UI::Theme& theme = UI::Theme::get();
        if (frame == SETTLE)
        {
            m_Observed.measured = theme.displayScale();
            m_Observed.fontGenerationBefore = theme.fontGeneration();
            theme.setDisplayScale(m_Observed.measured + 0.5F);
            SDL_Event event{};
            event.type = SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED;
            event.window.windowID = SDL_GetWindowID(Core::Application::get().getWindow().getHandle());
            SDL_PushEvent(&event);
        }
        else if (frame == SETTLE + STEP)
        {
            m_Observed.afterRebuild = theme.displayScale();
            m_Observed.fontGenerationAfter = theme.fontGeneration();
            Core::Application::get().stop();
        }
        else if (frame >= MAX_FRAMES)
        {
            m_Observed.timedOut = true;
            Core::Application::get().stop();
        }
    }

  private:
    static constexpr int SETTLE = 10;
    static constexpr int STEP = 5;
    static constexpr int MAX_FRAMES = 200;

    DisplayScaleObserved& m_Observed;
};

TEST(UiLayersOnDisplayTest, ADisplayScaleChangeRebuildsTheFontsAtTheMeasuredScale)
{
    if (!hasDisplay())
    {
        GTEST_SKIP() << "No display available (headless environment)";
    }

    Core::ApplicationSpecification spec;
    spec.Name = "UiLayersOnDisplayScaleTest";
    spec.VSync = false;
    DisplayScaleObserved observed;
    try
    {
        Core::Application app(spec);
        app.pushLayer<UI::UILayer>();
        app.pushLayer<DisplayScaleDriverLayer>(observed);
        app.run();
    }
    catch (const std::exception& e)
    {
        FAIL() << "Application failed after the display probe passed: " << e.what();
    }

    EXPECT_FALSE(observed.timedOut);
    EXPECT_GT(observed.measured, 0.0F);
    EXPECT_FLOAT_EQ(observed.afterRebuild, observed.measured); // back at what the window reports
    EXPECT_GT(observed.fontGenerationAfter, observed.fontGenerationBefore) << "the fonts weren't rebuilt";
}

/// What the real OpenGL icon backend did inside a frame, with the app's context current.
struct IconTextureObserved
{
    bool ran = false;
    std::uint32_t texture = 0U;
    bool bindingRestored = false;
    bool deleted = false;
    std::uint32_t refusedSize = 1U;
};

/// Makes a 2x2 icon texture through UI::IconTextureBackend::openGL() on the first frame, with another
/// texture bound, then deletes it and stops the app.
class IconTextureDriverLayer : public Core::Layer
{
  public:
    explicit IconTextureDriverLayer(IconTextureObserved& observed) : Layer("IconTextureDriver"), m_Observed(observed)
    {}

    void onRender() override
    {
        if (m_Observed.ran)
        {
            return;
        }
        m_Observed.ran = true;
        const UI::IconTextureBackend gl = UI::IconTextureBackend::openGL();
        constexpr std::array<std::uint8_t, std::size_t{2} * 2U * 4U> PIXELS{};

        GLuint bound = 0U;
        glGenTextures(1, &bound);
        glBindTexture(GL_TEXTURE_2D, bound);
        m_Observed.texture = gl.create(2, 2, PIXELS);
        GLint after = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &after);
        m_Observed.bindingRestored = std::cmp_equal(after, bound);
        m_Observed.refusedSize = gl.create(3, 2, PIXELS); // 3x2 needs 24 bytes, not 16

        gl.destroy(m_Observed.texture);
        m_Observed.deleted = glIsTexture(m_Observed.texture) == GL_FALSE;
        glBindTexture(GL_TEXTURE_2D, 0U);
        glDeleteTextures(1, &bound);
        Core::Application::get().stop();
    }

  private:
    IconTextureObserved& m_Observed;
};

TEST(UiLayersOnDisplayTest, TheOpenGLIconBackendMakesAndDeletesATexture)
{
    if (!hasDisplay())
    {
        GTEST_SKIP() << "No display available (headless environment)";
    }

    Core::ApplicationSpecification spec;
    spec.Name = "UiLayersOnDisplayIconTest";
    spec.VSync = false;
    IconTextureObserved observed;
    try
    {
        Core::Application app(spec);
        app.pushLayer<IconTextureDriverLayer>(observed);
        app.run();
    }
    catch (const std::exception& e)
    {
        FAIL() << "Application failed after the display probe passed: " << e.what();
    }

    ASSERT_TRUE(observed.ran);
    EXPECT_NE(observed.texture, 0U);
    EXPECT_TRUE(observed.bindingRestored) << "the icon upload must leave the caller's texture bound";
    EXPECT_EQ(observed.refusedSize, 0U);
    EXPECT_TRUE(observed.deleted);
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

/// What the About driver saw.
struct AboutObserved
{
    bool openAfterRequest = false;
    bool largeFontLoaded = false;
    int frames = 0;
    bool timedOut = false;
};

/// Pushed after UILayer and AboutLayer, so its onRender() runs in the same frame after the dialog's:
/// raises Core::OpenAboutEvent, as Settings' "About TaskSmack" does, then checks the dialog is up
/// (drawn with the icon AboutLayer loaded and the theme's large title font) and stops the app.
class AboutDriverLayer : public Core::Layer
{
  public:
    explicit AboutDriverLayer(AboutObserved& observed) : Layer("AboutDriver"), m_Observed(observed)
    {}

    void onRender() override
    {
        Core::AnimationRequest::request(Core::AnimationRequest::FULL_RATE); // keep frames coming
        const int frame = ++m_Observed.frames;
        if (frame == SETTLE)
        {
            Core::OpenAboutEvent event;
            Core::Application::get().raiseEvent(event);
        }
        else if (frame == SETTLE + STEP)
        {
            m_Observed.openAfterRequest = ImGui::IsPopupOpen(App::AboutDialog::POPUP_ID);
            m_Observed.largeFontLoaded = UI::Theme::get().largeFont() != nullptr;
            Core::Application::get().stop();
        }
        else if (frame >= MAX_FRAMES)
        {
            m_Observed.timedOut = true;
            Core::Application::get().stop();
        }
    }

  private:
    static constexpr int SETTLE = 10;
    static constexpr int STEP = 5;
    static constexpr int MAX_FRAMES = 200;

    AboutObserved& m_Observed;
};

TEST(UiLayersOnDisplayTest, AboutOpensWithItsIconAndTitleFont)
{
    if (!hasDisplay())
    {
        GTEST_SKIP() << "No display available (headless environment)";
    }

    Core::ApplicationSpecification spec;
    spec.Name = "UiLayersOnDisplayAboutTest";
    spec.VSync = false;
    AboutObserved observed;
    try
    {
        Core::Application app(spec);
        app.pushLayer<UI::UILayer>();
        app.pushLayer<App::AboutLayer>();
        app.pushLayer<AboutDriverLayer>(observed);
        app.run();
    }
    catch (const std::exception& e)
    {
        FAIL() << "Application failed after the display probe passed: " << e.what();
    }

    EXPECT_FALSE(observed.timedOut);
    EXPECT_TRUE(observed.largeFontLoaded);
    EXPECT_TRUE(observed.openAfterRequest);
}

/// What the drag-and-resize driver saw.
struct WindowDragObserved
{
    /// False under SDL's offscreen driver, which keeps a window where it was made, and on a desktop
    /// whose window manager ignored a direct setPosition() after the drag didn't move the window (#2707).
    bool positionsApply = true;
    bool movedByDrag = false;
    bool widenedByResize = false;
    bool maximizedBeforeDrag = false;
    bool restoredByDrag = false;
    int frames = 0;
    int pressRetries = 0; ///< Presses made again because the one before had no effect (#2221)
    bool timedOut = false;
};

/// Pushed after UILayer and TitleBarLayer: holds the left button down and moves the pointer through
/// TitleBarLayer::setMouseStateQueryForTesting() -- a pushed event can't move SDL's own pointer -- so
/// the drag and resize the press begins step each frame (#1891): a drag on the title bar moves the
/// window, a drag from the right edge widens it, and a drag on a maximized window's title bar past
/// the threshold restores it. Each step waits for its effect (the window manager applies a move,
/// resize or restore some frames later on X11) up to WAIT_LIMIT frames, then lets go.
///
/// A press with no effect after RETRY_EVERY frames is made again (#2221). TitleBarLayer ends a drag or
/// resize on SDL_EVENT_WINDOW_FOCUS_LOST, as it should. Under `xvfb-run ctest -j`, every display test
/// shares one X server, and with no window manager focus follows the pointer. So another test's window
/// mapping over this one can take the focus between the press and the first step, and the step never
/// starts. The double-click that maximizes isn't repeated: it toggles, and it doesn't need the focus
/// after the event.
class WindowDragDriverLayer : public Core::Layer
{
  public:
    /// @p loseFocusAfterFirstPress: send the window a focus loss right after the first drag's press,
    /// as another window taking the focus would (#2221).
    WindowDragDriverLayer(WindowDragObserved& observed, App::TitleBarLayer& titleBar, bool loseFocusAfterFirstPress = false)
        : Layer("WindowDragDriver"), m_Observed(observed), m_TitleBar(titleBar), m_LoseFocusAfterFirstPress(loseFocusAfterFirstPress)
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
        const auto [x, y] = window.getPosition();
        const float titleX = static_cast<float>(width) * 0.5F; // empty title bar: the middle, clear of the buttons
        constexpr float TITLE_Y = 16.0F;
        const auto next = [this, frame](Phase phase, int frames)
        {
            m_Phase = phase;
            m_NextFrame = frame + frames;
        };
        const auto waiting = [this, frame]
        {
            return frame < m_Deadline;
        };
        // Press again from where the window is now (see the class comment).
        const auto retryDue = [this, frame]
        {
            if (frame - m_PressedAt < RETRY_EVERY)
            {
                return false;
            }
            m_PressedAt = frame;
            ++m_Observed.pressRetries;
            return true;
        };
        switch (m_Phase)
        {
        case Phase::BeginDrag:
        {
            const char* driver = SDL_GetCurrentVideoDriver();
            m_Observed.positionsApply = driver == nullptr || std::string_view(driver) != "offscreen";
            m_Position = {x, y};
            hold(x + static_cast<int>(titleX) + DRAG_X, y + static_cast<int>(TITLE_Y) + DRAG_Y);
            pushButton(titleX, TITLE_Y, true, 1);
            if (m_LoseFocusAfterFirstPress)
            {
                pushFocusLost();
            }
            m_PressedAt = frame;
            m_Deadline = frame + WAIT_LIMIT;
            next(Phase::AwaitMoved, 1);
            break;
        }
        case Phase::AwaitMoved:
            m_Observed.movedByDrag = std::pair{x, y} != m_Position;
            if (!m_Observed.movedByDrag && m_Observed.positionsApply && waiting())
            {
                if (retryDue())
                {
                    hold(x + static_cast<int>(titleX) + DRAG_X, y + static_cast<int>(TITLE_Y) + DRAG_Y);
                    pushButton(titleX, TITLE_Y, true, 1);
                }
                next(Phase::AwaitMoved, 1);
                break;
            }
            release();
            if (!m_Observed.movedByDrag && m_Observed.positionsApply && !TestSupport::displayRequired())
            {
                // The drag didn't move the window. WSLg's window manager (Weston's XWM) sometimes
                // ignores client moves altogether (#2707), as SetAndGetPositionRoundTrip skips for: ask
                // for a move directly, and only if that lands is the drag at fault. CI
                // (TASKSMACK_REQUIRE_DISPLAY=1, no window manager) always asserts the drag.
                m_Position = {x, y};
                Core::Application::get().getWindow().setPosition(x + DRAG_X, y + DRAG_Y);
                m_Deadline = frame + WAIT_LIMIT;
                next(Phase::AwaitProbeMove, 1);
                break;
            }
            next(Phase::BeginResize, STEP);
            break;
        case Phase::AwaitProbeMove:
            if (std::pair{x, y} == m_Position && waiting())
            {
                next(Phase::AwaitProbeMove, 1);
                break;
            }
            m_Observed.positionsApply = std::pair{x, y} != m_Position;
            next(Phase::BeginResize, STEP);
            break;
        case Phase::BeginResize:
        {
            m_Size = {width, height};
            const float edgeX = static_cast<float>(width) - 2.0F;
            const float edgeY = static_cast<float>(height) * 0.5F;
            hold(x + static_cast<int>(edgeX) + WIDEN_BY, y + static_cast<int>(edgeY));
            pushButton(edgeX, edgeY, true, 1);
            m_PressedAt = frame;
            m_Deadline = frame + WAIT_LIMIT;
            next(Phase::AwaitWidened, 1);
            break;
        }
        case Phase::AwaitWidened:
            m_Observed.widenedByResize = width > m_Size.first;
            if (!m_Observed.widenedByResize && waiting())
            {
                if (retryDue())
                {
                    const float edgeX = static_cast<float>(width) - 2.0F;
                    const float edgeY = static_cast<float>(height) * 0.5F;
                    hold(x + static_cast<int>(edgeX) + WIDEN_BY, y + static_cast<int>(edgeY));
                    pushButton(edgeX, edgeY, true, 1);
                }
                next(Phase::AwaitWidened, 1);
                break;
            }
            release(); // ends the resize, committing the size it last held back
            next(Phase::Maximize, STEP);
            break;
        case Phase::Maximize:
            pushButton(titleX, TITLE_Y, true, 2); // double-click: maximize
            pushButton(titleX, TITLE_Y, false, 2);
            m_Deadline = frame + WAIT_LIMIT;
            next(Phase::AwaitMaximized, 1);
            break;
        case Phase::AwaitMaximized:
            m_Observed.maximizedBeforeDrag = window.isMaximized();
            if (!m_Observed.maximizedBeforeDrag && waiting())
            {
                next(Phase::AwaitMaximized, 1);
                break;
            }
            hold(x + static_cast<int>(titleX) + DRAG_X, y + static_cast<int>(TITLE_Y) + DRAG_Y);
            pushButton(titleX, TITLE_Y, true, 1);
            m_PressedAt = frame;
            m_Deadline = frame + WAIT_LIMIT;
            next(Phase::AwaitRestored, 1);
            break;
        case Phase::AwaitRestored:
            m_Observed.restoredByDrag = !window.isMaximized();
            if (!m_Observed.restoredByDrag && waiting())
            {
                if (retryDue())
                {
                    hold(x + static_cast<int>(titleX) + DRAG_X, y + static_cast<int>(TITLE_Y) + DRAG_Y);
                    pushButton(titleX, TITLE_Y, true, 1);
                }
                next(Phase::AwaitRestored, 1);
                break;
            }
            release();
            next(Phase::Stop, STEP);
            break;
        case Phase::Stop:
            m_TitleBar.setMouseStateQueryForTesting({});
            Core::Application::get().stop();
            break;
        }
    }

  private:
    /// The pointer at (@p globalX, @p globalY) with the left button down, until release().
    void hold(int globalX, int globalY)
    {
        m_TitleBar.setMouseStateQueryForTesting([globalX, globalY]
                                                { return std::tuple<int, int, SDL_MouseButtonFlags>{globalX, globalY, SDL_BUTTON_LMASK}; });
    }

    /// The button up where it was: the next update sees no button held and ends the interaction.
    void release()
    {
        m_TitleBar.setMouseStateQueryForTesting([] { return std::tuple<int, int, SDL_MouseButtonFlags>{0, 0, 0U}; });
    }

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

    static void pushFocusLost()
    {
        SDL_Event event{};
        event.type = SDL_EVENT_WINDOW_FOCUS_LOST;
        event.window.windowID = SDL_GetWindowID(Core::Application::get().getWindow().getHandle());
        static_cast<void>(SDL_PushEvent(&event));
    }

    /// The steps, in order; each waits m_NextFrame before running.
    enum class Phase : std::uint8_t
    {
        BeginDrag,
        AwaitMoved,
        AwaitProbeMove,
        BeginResize,
        AwaitWidened,
        Maximize,
        AwaitMaximized,
        AwaitRestored,
        Stop,
    };

    static constexpr int SETTLE = 10;
    static constexpr int STEP = 10;
    static constexpr int WAIT_LIMIT = 300;  // frames to wait for an effect before going on
    static constexpr int RETRY_EVERY = 30;  // frames before a press with no effect is made again
    static constexpr int MAX_FRAMES = 1800; // room for every wait, the direct-move probe's included, to run out
    static constexpr int DRAG_X = 40;       // past DragTracker::DRAG_THRESHOLD_PX
    static constexpr int DRAG_Y = 30;
    static constexpr int WIDEN_BY = 60;

    WindowDragObserved& m_Observed;
    App::TitleBarLayer& m_TitleBar;
    bool m_LoseFocusAfterFirstPress = false;
    Phase m_Phase = Phase::BeginDrag;
    int m_NextFrame = SETTLE;
    int m_Deadline = 0;
    int m_PressedAt = 0;
    std::pair<int, int> m_Position;
    std::pair<int, int> m_Size;
};

TEST(UiLayersOnDisplayTest, TitleBarDragsAndResizesStepWhileTheButtonIsHeld)
{
    if (!hasDisplay())
    {
        GTEST_SKIP() << "No display available (headless environment)";
    }

    Core::ApplicationSpecification spec;
    spec.Name = "UiLayersOnDisplayDragTest";
    spec.VSync = false;
    WindowDragObserved observed;
    try
    {
        Core::Application app(spec);
        app.pushLayer<UI::UILayer>();
        auto& titleBar = app.pushLayer<App::TitleBarLayer>();
        app.pushLayer<WindowDragDriverLayer>(observed, titleBar);
        app.run();
    }
    catch (const std::exception& e)
    {
        FAIL() << "Application failed after the display probe passed: " << e.what();
    }

    RecordProperty("pressRetries", observed.pressRetries);
    EXPECT_FALSE(observed.timedOut);
    if (observed.positionsApply)
    {
        EXPECT_TRUE(observed.movedByDrag); // a title-bar drag moves the window
    }
    EXPECT_TRUE(observed.widenedByResize);     // a right-edge drag widens it
    EXPECT_TRUE(observed.maximizedBeforeDrag); // (the double-click maximized it)
    EXPECT_TRUE(observed.restoredByDrag);      // and a drag past the threshold restores it
}

// #2221: TitleBarLayer ends a drag on a focus loss, so a focus loss right after the press (another
// test's window mapping over this one, on the X server they share under `xvfb-run ctest -j`) left the
// drag step waiting out its deadline with the window unmoved. The driver presses again.
TEST(UiLayersOnDisplayTest, TitleBarDragStepSurvivesAFocusLossRightAfterThePress)
{
    if (!hasDisplay())
    {
        GTEST_SKIP() << "No display available (headless environment)";
    }

    Core::ApplicationSpecification spec;
    spec.Name = "UiLayersOnDisplayDragFocusTest";
    spec.VSync = false;
    WindowDragObserved observed;
    try
    {
        Core::Application app(spec);
        app.pushLayer<UI::UILayer>();
        auto& titleBar = app.pushLayer<App::TitleBarLayer>();
        app.pushLayer<WindowDragDriverLayer>(observed, titleBar, /*loseFocusAfterFirstPress=*/true);
        app.run();
    }
    catch (const std::exception& e)
    {
        FAIL() << "Application failed after the display probe passed: " << e.what();
    }

    EXPECT_FALSE(observed.timedOut);
    if (observed.positionsApply)
    {
        EXPECT_TRUE(observed.movedByDrag);
        EXPECT_GE(observed.pressRetries, 1) << "the lost drag was pressed again";
    }
}

/// What the drag-geometry driver saw (#2715).
struct DragGeometryObserved
{
    bool positionsApply = true; ///< False under SDL's offscreen driver, which keeps a window where it was made.
    int checks = 0;             ///< Pointer positions checked against where the window ended up
    std::vector<std::string> failures;
    bool widenedByResize = false;
    std::array<bool, 2> maximizedBeforeDrag{}; ///< For the press near the left end, then the right
    std::array<bool, 2> clickKeptMaximized{};  ///< A move inside the threshold leaves it maximized
    std::array<bool, 2> restoredByDrag{};
    bool maximizeUnavailable = false; ///< The window never maximized: the restore checks were skipped
    /// What the window's hit test answers where each press was made (#2716): the path a real press
    /// there takes. The driver's pushed events drive the client-side path whatever the answer.
    SDL_HitTestResult plainPressHitTest = SDL_HITTEST_NORMAL;
    SDL_HitTestResult resizePressHitTest = SDL_HITTEST_NORMAL;
    std::array<SDL_HitTestResult, 2> maximizedPressHitTest{};
    bool systemDrag = false;         ///< Core::VideoBackend::usesSystemDrag() while the app ran
    bool systemResize = false;       ///< Core::VideoBackend::usesSystemResize()
    bool clientSideMaximize = false; ///< Core::VideoBackend::supportsClientSideMaximize()
    int frames = 0;
    int pressRetries = 0; ///< Presses made again because the one before had no effect (#2221)
    bool timedOut = false;
};

/// Pushed after UILayer and TitleBarLayer: drives the title bar through its own test seams --
/// SDL_PushEvent() for the press and TitleBarLayer::setMouseStateQueryForTesting() for the held
/// pointer -- and checks where the window lands, never moving the OS pointer. In order:
/// - a plain drag: the window follows the pointer 1:1;
/// - a resize from the right edge: the window widens and its left and top edges stay put;
/// - twice, pressing near the title bar's left end and then near its right end: maximize, press,
///   move inside DRAG_THRESHOLD_PX (still maximized), then past it and on: the window is restored
///   with the pointer at the same fraction across it and the same distance below its top (#2715).
/// Each position waits for the window to reach it, up to WAIT_LIMIT frames (a window manager applies
/// moves some frames later on X11), then the miss is recorded.
class DragGeometryDriverLayer : public Core::Layer
{
  public:
    DragGeometryDriverLayer(DragGeometryObserved& observed, App::TitleBarLayer& titleBar)
        : Layer("DragGeometryDriver"), m_Observed(observed), m_TitleBar(titleBar)
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
        const auto [x, y] = window.getPosition();
        const bool maximized = window.isMaximized();
        const auto next = [this, frame](Phase phase, int frames)
        {
            m_Phase = phase;
            m_NextFrame = frame + frames;
        };
        const bool waiting = frame < m_Deadline;
        switch (m_Phase)
        {
        case Phase::BeginPlainDrag:
        {
            const char* driver = SDL_GetCurrentVideoDriver();
            m_Observed.positionsApply = driver == nullptr || std::string_view(driver) != "offscreen";
            m_Observed.systemDrag = Core::VideoBackend::usesSystemDrag();
            m_Observed.systemResize = Core::VideoBackend::usesSystemResize();
            m_Observed.clientSideMaximize = Core::VideoBackend::supportsClientSideMaximize();
            m_Start = {x, y};
            const int pressX = width / 2; // the press's own integer point, so the hit test is asked where it lands
            m_Observed.plainPressHitTest = m_TitleBar.hitTest(static_cast<float>(pressX), static_cast<float>(TITLE_Y));
            press(x, y, pressX, TITLE_Y, frame);
            m_Sample = 0;
            holdAt(PLAIN_OFFSETS[0], frame);
            next(Phase::AwaitPlainDrag, 1);
            break;
        }
        case Phase::AwaitPlainDrag:
        {
            // Still where the last position put it: the drag was lost (see retryDue()), so the press
            // is made again where the pointer was.
            const std::pair<int, int> previous = m_Sample == 0 ? std::pair{0, 0} : PLAIN_OFFSETS.at(m_Sample - 1);
            if (within(x - (m_Start.first + previous.first), y - (m_Start.second + previous.second)) && m_Observed.positionsApply &&
                waiting && retryDue(frame))
            {
                retryPress(x, y, width, maximized, previous, PLAIN_OFFSETS.at(m_Sample), frame);
            }
            const auto [dx, dy] = PLAIN_OFFSETS.at(m_Sample);
            const int errorX = x - (m_Start.first + dx);
            const int errorY = y - (m_Start.second + dy);
            if (!within(errorX, errorY) && m_Observed.positionsApply && waiting)
            {
                next(Phase::AwaitPlainDrag, 1);
                break;
            }
            check(within(errorX, errorY), std::format("plain drag by ({}, {}): window off by ({}, {})", dx, dy, errorX, errorY));
            if (++m_Sample < PLAIN_OFFSETS.size())
            {
                holdAt(PLAIN_OFFSETS.at(m_Sample), frame);
                next(Phase::AwaitPlainDrag, 1);
                break;
            }
            release();
            next(Phase::BeginResize, STEP);
            break;
        }
        case Phase::BeginResize:
            m_Start = {x, y};
            m_StartWidth = width;
            {
                const int pressY = height / 2;
                m_Observed.resizePressHitTest = m_TitleBar.hitTest(static_cast<float>(width - 2), static_cast<float>(pressY));
                press(x, y, width - 2, pressY, frame);
            }
            holdAt({WIDEN_BY, 0}, frame);
            next(Phase::AwaitResize, 1);
            break;
        case Phase::AwaitResize:
        {
            // Not widened at all: the press was lost (see retryDue()), so it is made again.
            if (width == m_StartWidth && waiting && retryDue(frame))
            {
                retryPress(x, y, width, maximized, {0, 0}, {WIDEN_BY, 0}, frame);
            }
            const bool widened = std::abs(width - (m_StartWidth + WIDEN_BY)) <= TOLERANCE_PX;
            if (!widened && waiting)
            {
                next(Phase::AwaitResize, 1);
                break;
            }
            m_Observed.widenedByResize = width > m_StartWidth;
            const int errorX = x - m_Start.first;
            const int errorY = y - m_Start.second;
            check(within(errorX, errorY), std::format("right-edge resize moved the left/top edges by ({}, {})", errorX, errorY));
            release();
            m_End = 0;
            next(Phase::Maximize, STEP);
            break;
        }
        case Phase::Maximize:
            // A double-click toggles, so it is only sent to a window that isn't maximized already.
            if (!maximized)
            {
                pushButton(static_cast<float>(width) * 0.5F, static_cast<float>(TITLE_Y), true, 2); // double-click
                pushButton(static_cast<float>(width) * 0.5F, static_cast<float>(TITLE_Y), false, 2);
            }
            m_Deadline = frame + WAIT_LIMIT;
            next(Phase::AwaitMaximized, 1);
            break;
        case Phase::AwaitMaximized:
            m_Observed.maximizedBeforeDrag.at(m_End) = maximized;
            if (!maximized && waiting)
            {
                next(Phase::AwaitMaximized, 1);
                break;
            }
            if (!maximized)
            {
                // This display can't maximize the window (the test skips this part, not the rest).
                m_Observed.maximizeUnavailable = true;
                next(Phase::Stop, STEP);
                break;
            }
            m_Geometry = {};
            m_StableFrames = 0;
            m_Deadline = frame + WAIT_LIMIT;
            next(Phase::AwaitMaximizedSettled, 1);
            break;
        case Phase::AwaitMaximizedSettled:
        {
            // The maximized flag can lead the window's new position and size by some frames (X11
            // reports the configure later), and a press read from the old geometry lands somewhere
            // else on the window: wait until the geometry has held for SETTLED_FRAMES.
            const std::array<int, 4> geometry{x, y, width, height};
            m_StableFrames = geometry == m_Geometry ? m_StableFrames + 1 : 0;
            m_Geometry = geometry;
            if (m_StableFrames < SETTLED_FRAMES && waiting)
            {
                next(Phase::AwaitMaximizedSettled, 1);
                break;
            }
            // Near one end of the title bar, clear of the icon and the buttons: near the top of the
            // title bar for the left end, three quarters of the way down it for the right (a
            // maximized window has no resize border to avoid).
            m_PressLocalY = m_End == 0 ? TITLE_Y_NEAR_TOP : static_cast<int>(App::TitleBarLayer::height() * 0.75F);
            m_PressLocalX = clearPressX(width, m_PressLocalY, m_End == 0);
            m_Fraction = static_cast<double>(m_PressLocalX) / static_cast<double>(width);
            m_Observed.maximizedPressHitTest.at(m_End) =
                m_TitleBar.hitTest(static_cast<float>(m_PressLocalX), static_cast<float>(m_PressLocalY));
            press(x, y, m_PressLocalX, m_PressLocalY, frame);
            holdAt({INSIDE_THRESHOLD, INSIDE_THRESHOLD}, frame);
            next(Phase::CheckStillMaximized, STEP);
            break;
        }
        case Phase::CheckStillMaximized:
            m_Observed.clickKeptMaximized.at(m_End) = maximized;
            m_Sample = 0;
            holdAt(RESTORE_OFFSETS[0], frame);
            next(Phase::AwaitRestoreSample, 1);
            break;
        case Phase::AwaitRestoreSample:
        {
            // Where the pointer is on the window, against where it was on the maximized one.
            const auto pointerError = [&](std::pair<int, int> offset)
            {
                const int pointerX = m_Press.first + offset.first;
                const int pointerY = m_Press.second + offset.second;
                return std::pair{static_cast<int>(std::lround(static_cast<double>(pointerX - x) - (m_Fraction * width))),
                                 (pointerY - y) - m_PressLocalY};
            };
            // Still maximized well after the pointer passed the threshold, or still where the last
            // position put it: the press or the drag was lost (see retryDue()), so the press is made
            // again where the pointer was.
            const std::pair<int, int> previous = m_Sample == 0 ? std::pair{0, 0} : RESTORE_OFFSETS.at(m_Sample - 1);
            const bool lost = m_Sample == 0 ? maximized : !maximized && within(pointerError(previous).first, pointerError(previous).second);
            if (lost && waiting && retryDue(frame))
            {
                retryPress(x, y, width, maximized, previous, RESTORE_OFFSETS.at(m_Sample), frame);
            }
            const auto [dx, dy] = RESTORE_OFFSETS.at(m_Sample);
            const auto [errorX, errorY] = pointerError({dx, dy});
            const bool placed = !maximized && within(errorX, errorY);
            if (!placed && (m_Observed.positionsApply || maximized) && waiting)
            {
                next(Phase::AwaitRestoreSample, 1);
                break;
            }
            if (m_Sample == 0)
            {
                m_Observed.restoredByDrag.at(m_End) = !maximized;
            }
            check(within(errorX, errorY),
                  std::format("{} end, pointer moved by ({}, {}): {:.3f} across a {}-wide window, {} px down; pointer off by ({}, {})",
                              m_End == 0 ? "left" : "right",
                              dx,
                              dy,
                              m_Fraction,
                              width,
                              m_PressLocalY,
                              errorX,
                              errorY));
            if (++m_Sample < RESTORE_OFFSETS.size())
            {
                holdAt(RESTORE_OFFSETS.at(m_Sample), frame);
                next(Phase::AwaitRestoreSample, 1);
                break;
            }
            release();
            if (m_End == 0)
            {
                m_End = 1;
                next(Phase::Maximize, STEP);
                break;
            }
            next(Phase::Stop, STEP);
            break;
        }
        case Phase::Stop:
            m_TitleBar.setMouseStateQueryForTesting({});
            Core::Application::get().stop();
            break;
        }
    }

  private:
    [[nodiscard]] static bool within(int errorX, int errorY) noexcept
    {
        return std::abs(errorX) <= TOLERANCE_PX && std::abs(errorY) <= TOLERANCE_PX;
    }

    /// One position checked; a miss is recorded only where SDL can place windows.
    void check(bool ok, std::string message)
    {
        if (!m_Observed.positionsApply)
        {
            return;
        }
        ++m_Observed.checks;
        if (!ok)
        {
            m_Observed.failures.push_back(std::move(message));
        }
    }

    /// The first x from one end at which a CLEAR_SPAN-wide run of the title bar at @p localY is clear
    /// of the icon and the buttons, plus half that run.
    [[nodiscard]] int clearPressX(int width, int localY, bool fromLeft) const
    {
        int clear = 0;
        for (int i = END_MARGIN; i < width / 2; ++i)
        {
            const int localX = fromLeft ? i : width - 1 - i;
            clear = m_TitleBar.isPointInControlArea(static_cast<float>(localX), static_cast<float>(localY)) ? 0 : clear + 1;
            if (clear == CLEAR_SPAN)
            {
                return fromLeft ? localX - (CLEAR_SPAN / 2) : localX + (CLEAR_SPAN / 2);
            }
        }
        return width / 2;
    }

    /// A left-button press at window-local (@p localX, @p localY) of the window at (@p windowX, @p windowY).
    void press(int windowX, int windowY, int localX, int localY, int frame)
    {
        m_Press = {windowX + localX, windowY + localY};
        m_PointerShift = {0, 0};
        pushButton(static_cast<float>(localX), static_cast<float>(localY), true, 1);
        m_HeldAt = frame;
    }

    /// Whether a press with no effect is due to be made again (#2221): TitleBarLayer ends a drag or
    /// resize on SDL_EVENT_WINDOW_FOCUS_LOST, as it should, and under ctest -j another test's window
    /// mapping, or the maximize itself, can take the focus between the press and any step.
    [[nodiscard]] bool retryDue(int frame) const noexcept
    {
        return frame - m_HeldAt >= RETRY_EVERY;
    }

    /// The press made again on the window now at (@p windowX, @p windowY), where the pointer was at
    /// @p previous from the original press, then the pointer moved on to @p offset, within the
    /// original deadline. A drag goes by the pointer's movement from its press, so the window ends up
    /// where the original drag would have put it. A drag that restored the window can leave the
    /// pointer over the icon, a button or the top resize border of the restored one, where a press
    /// doesn't drag: then it is made in the middle of the title bar instead, and the pointer held that
    /// far from where it was from then on. The checks still measure from the original press.
    void
    retryPress(int windowX, int windowY, int width, bool maximized, std::pair<int, int> previous, std::pair<int, int> offset, int frame)
    {
        const int unshiftedLocalX = m_Press.first + previous.first - windowX;
        const int unshiftedLocalY = m_Press.second + previous.second - windowY;
        int localX = unshiftedLocalX + m_PointerShift.first;
        int localY = unshiftedLocalY + m_PointerShift.second;
        if (!maximized && (localY < TITLE_Y || m_TitleBar.isPointInControlArea(static_cast<float>(localX), static_cast<float>(localY))))
        {
            localX = width / 2;
            localY = TITLE_Y;
        }
        m_PointerShift = {localX - unshiftedLocalX, localY - unshiftedLocalY};
        pushButton(static_cast<float>(localX), static_cast<float>(localY), true, 1);
        const int deadline = m_Deadline;
        holdAt(offset, frame);
        m_Deadline = deadline;
        ++m_Observed.pressRetries;
    }

    /// The pointer @p offset from the press with the left button down, until release(), waiting up to
    /// WAIT_LIMIT frames from @p frame for its effect.
    void holdAt(std::pair<int, int> offset, int frame)
    {
        m_HeldAt = frame;
        const int globalX = m_Press.first + m_PointerShift.first + offset.first;
        const int globalY = m_Press.second + m_PointerShift.second + offset.second;
        m_TitleBar.setMouseStateQueryForTesting([globalX, globalY]
                                                { return std::tuple<int, int, SDL_MouseButtonFlags>{globalX, globalY, SDL_BUTTON_LMASK}; });
        m_Deadline = frame + WAIT_LIMIT;
    }

    /// The button up: the next update sees no button held and ends the interaction.
    void release()
    {
        m_TitleBar.setMouseStateQueryForTesting([] { return std::tuple<int, int, SDL_MouseButtonFlags>{0, 0, 0U}; });
    }

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

    enum class Phase : std::uint8_t
    {
        BeginPlainDrag,
        AwaitPlainDrag,
        BeginResize,
        AwaitResize,
        Maximize,
        AwaitMaximized,
        AwaitMaximizedSettled,
        CheckStillMaximized,
        AwaitRestoreSample,
        Stop,
    };

    static constexpr int SETTLE = 10;
    static constexpr int STEP = 10;
    static constexpr int WAIT_LIMIT = 300;
    static constexpr int RETRY_EVERY = 60;    // frames before a press with no effect is made again
    static constexpr int SETTLED_FRAMES = 15; // frames a maximized window's geometry holds before the press
    static constexpr int MAX_FRAMES = 4000;
    static constexpr int TOLERANCE_PX = 2;
    static constexpr int TITLE_Y = 16;         // window-local y of a press: inside the title bar, below the resize border
    static constexpr int TITLE_Y_NEAR_TOP = 3; // a press near the top of a maximized window's title bar
    static constexpr int END_MARGIN = 4;       // px from the window's side where the search for a press starts
    static constexpr int CLEAR_SPAN = 16;
    static constexpr int WIDEN_BY = 60;
    static constexpr int INSIDE_THRESHOLD = App::DragTracker::DRAG_THRESHOLD_PX - 2;
    static constexpr std::array<std::pair<int, int>, 3> PLAIN_OFFSETS{{{40, 30}, {75, 55}, {-20, 90}}};
    static constexpr std::array<std::pair<int, int>, 4> RESTORE_OFFSETS{{{0, 40}, {15, 90}, {-30, 140}, {10, 180}}};

    DragGeometryObserved& m_Observed;
    App::TitleBarLayer& m_TitleBar;
    Phase m_Phase = Phase::BeginPlainDrag;
    int m_NextFrame = SETTLE;
    int m_Deadline = 0;
    std::size_t m_Sample = 0;
    std::size_t m_End = 0; ///< 0: the press near the left end; 1: near the right
    std::pair<int, int> m_Start;
    int m_StartWidth = 0;
    std::pair<int, int> m_Press;        ///< Global: where the pointer was pressed, which the checks measure from
    std::pair<int, int> m_PointerShift; ///< How far the held pointer is from m_Press + offset after a lower re-press
    int m_HeldAt = 0;                   ///< The frame the pointer was last pressed or moved
    int m_PressLocalX = 0;
    int m_PressLocalY = 0;
    double m_Fraction = 0.0;
    std::array<int, 4> m_Geometry{}; ///< x, y, width, height on the last frame, while waiting for them to settle
    int m_StableFrames = 0;
};

// #2715: a plain title-bar drag follows the pointer 1:1, a right-edge resize keeps the left edge, and
// a drag that restores a maximized window keeps the pointer where it pressed on the title bar, across
// and down, pressed near either end.
TEST(UiLayersOnDisplayTest, TitleBarDragGeometryKeepsThePointerOnTheWindow)
{
    if (!hasDisplay())
    {
        GTEST_SKIP() << "No display available (headless environment)";
    }

    Core::ApplicationSpecification spec;
    spec.Name = "UiLayersOnDisplayDragGeometryTest";
    spec.VSync = false;
    DragGeometryObserved observed;
    try
    {
        Core::Application app(spec);
        app.pushLayer<UI::UILayer>();
        auto& titleBar = app.pushLayer<App::TitleBarLayer>();
        app.pushLayer<DragGeometryDriverLayer>(observed, titleBar);
        app.run();
    }
    catch (const std::exception& e)
    {
        FAIL() << "Application failed after the display probe passed: " << e.what();
    }

    RecordProperty("pressRetries", observed.pressRetries);
    EXPECT_FALSE(observed.timedOut);
    EXPECT_TRUE(observed.widenedByResize);
    for (const std::string& failure : observed.failures)
    {
        ADD_FAILURE() << failure;
    }
    // #2716: the other path. The pushed presses above drove the client-side drag and resize whatever
    // the window's hit test says; this is what it says, so a real press at the same points takes the
    // OS's or the compositor's gesture where the backend runs it and the client-side one elsewhere.
    EXPECT_EQ(observed.plainPressHitTest, observed.systemDrag ? SDL_HITTEST_DRAGGABLE : SDL_HITTEST_NORMAL);
    EXPECT_EQ(observed.resizePressHitTest, observed.systemResize ? SDL_HITTEST_RESIZE_RIGHT : SDL_HITTEST_NORMAL);
#ifdef _WIN32
    EXPECT_EQ(observed.plainPressHitTest, SDL_HITTEST_DRAGGABLE); // Windows' own move loop: Aero Snap
    EXPECT_EQ(observed.resizePressHitTest, SDL_HITTEST_NORMAL);   // the resize stays the app's
#endif
    if (observed.maximizeUnavailable)
    {
        // The plain drag and the resize were checked above; only the restore part is skipped.
        GTEST_SKIP() << "The window never maximized on this display, so the drag-to-restore checks were skipped";
    }
    // A client-side maximized window (Windows) keeps the press for the restore under the pointer.
    const bool maximizedSystemDrag = App::computeSystemDragApplies(observed.systemDrag, observed.clientSideMaximize, /*isMaximized=*/true);
    for (std::size_t end = 0; end < 2; ++end)
    {
        SCOPED_TRACE(end == 0 ? "pressed near the left end" : "pressed near the right end");
        EXPECT_TRUE(observed.maximizedBeforeDrag.at(end));
        EXPECT_TRUE(observed.clickKeptMaximized.at(end)) << "a move inside the threshold restored the window";
        EXPECT_TRUE(observed.restoredByDrag.at(end));
        EXPECT_EQ(observed.maximizedPressHitTest.at(end), maximizedSystemDrag ? SDL_HITTEST_DRAGGABLE : SDL_HITTEST_NORMAL);
#ifdef _WIN32
        EXPECT_EQ(observed.maximizedPressHitTest.at(end), SDL_HITTEST_NORMAL);
#endif
    }
    if (observed.positionsApply)
    {
        EXPECT_EQ(observed.checks, 12); // 3 plain-drag positions, the resize, 4 restore positions per end
    }
}

// ---- The live resize from every edge and corner (#2717) ----

constexpr int RESIZE_EDGE_INSET = 2;   // px inside a window edge: within the resize border at any display scale
constexpr int RESIZE_TOLERANCE_PX = 2; // how far a landed rectangle may be off, as the drag geometry test allows

/// Which of the window's edges a resize from @p edge pulls: the one pressed or, for a corner, both.
[[nodiscard]] constexpr bool pullsLeftEdge(App::ResizeEdge edge) noexcept
{
    return edge == App::ResizeEdge::Left || edge == App::ResizeEdge::TopLeft || edge == App::ResizeEdge::BottomLeft;
}

[[nodiscard]] constexpr bool pullsRightEdge(App::ResizeEdge edge) noexcept
{
    return edge == App::ResizeEdge::Right || edge == App::ResizeEdge::TopRight || edge == App::ResizeEdge::BottomRight;
}

[[nodiscard]] constexpr bool pullsTopEdge(App::ResizeEdge edge) noexcept
{
    return edge == App::ResizeEdge::Top || edge == App::ResizeEdge::TopLeft || edge == App::ResizeEdge::TopRight;
}

[[nodiscard]] constexpr bool pullsBottomEdge(App::ResizeEdge edge) noexcept
{
    return edge == App::ResizeEdge::Bottom || edge == App::ResizeEdge::BottomLeft || edge == App::ResizeEdge::BottomRight;
}

/// Whether a resize from @p edge moves the window's origin, which only a display that applies moves can show.
[[nodiscard]] constexpr bool movesTheWindowOrigin(App::ResizeEdge edge) noexcept
{
    return pullsLeftEdge(edge) || pullsTopEdge(edge);
}

/// Where the window should end up after a resize from @p edge of @p start with the pointer moved by
/// @p offset: the edge or corner pressed follows the pointer, held to the @p minimum size, and the
/// opposite edge or edges stay where they were.
[[nodiscard]] App::WindowRect
expectedResizeRect(App::ResizeEdge edge, const App::WindowRect& start, std::pair<int, int> offset, std::pair<int, int> minimum) noexcept
{
    int left = start.x;
    int top = start.y;
    int right = start.x + start.width;
    int bottom = start.y + start.height;
    if (pullsLeftEdge(edge))
    {
        left = std::min(start.x + offset.first, right - minimum.first);
    }
    if (pullsRightEdge(edge))
    {
        right = std::max(right + offset.first, start.x + minimum.first);
    }
    if (pullsTopEdge(edge))
    {
        top = std::min(start.y + offset.second, bottom - minimum.second);
    }
    if (pullsBottomEdge(edge))
    {
        bottom = std::max(bottom + offset.second, start.y + minimum.second);
    }
    return {.x = left, .y = top, .width = right - left, .height = bottom - top};
}

/// The minimum a client-side resize holds the window to (TitleBarLayer::updateResize()): the one SDL
/// was handed, and never below Core::WINDOW_MIN_DIMENSION, as computeResizeGeometry() clamps it.
[[nodiscard]] std::pair<int, int> resizeMinimum(const Core::Window& window) noexcept
{
    const auto [width, height] = window.getMinimumSize();
    return {std::max(width, Core::WINDOW_MIN_DIMENSION), std::max(height, Core::WINDOW_MIN_DIMENSION)};
}

/// The window-local point to press for a resize from @p edge of a @p width x @p height window:
/// RESIZE_EDGE_INSET px inside the edge or corner, midway along an edge.
[[nodiscard]] std::pair<int, int> resizePressPoint(App::ResizeEdge edge, int width, int height) noexcept
{
    int x = width / 2;
    int y = height / 2;
    if (pullsLeftEdge(edge))
    {
        x = RESIZE_EDGE_INSET;
    }
    else if (pullsRightEdge(edge))
    {
        x = width - RESIZE_EDGE_INSET;
    }
    if (pullsTopEdge(edge))
    {
        y = RESIZE_EDGE_INSET;
    }
    else if (pullsBottomEdge(edge))
    {
        y = height - RESIZE_EDGE_INSET;
    }
    return {x, y};
}

[[nodiscard]] App::WindowRect currentRect(const Core::Window& window)
{
    const auto [width, height] = window.getSize();
    const auto [x, y] = window.getPosition();
    return {.x = x, .y = y, .width = width, .height = height};
}

[[nodiscard]] bool sameRect(const App::WindowRect& a, const App::WindowRect& b) noexcept
{
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}

[[nodiscard]] bool withinRect(const App::WindowRect& a, const App::WindowRect& b) noexcept
{
    return std::abs(a.x - b.x) <= RESIZE_TOLERANCE_PX && std::abs(a.y - b.y) <= RESIZE_TOLERANCE_PX &&
           std::abs(a.width - b.width) <= RESIZE_TOLERANCE_PX && std::abs(a.height - b.height) <= RESIZE_TOLERANCE_PX;
}

[[nodiscard]] std::string describeRect(const App::WindowRect& rect)
{
    return std::format("{}x{} at ({}, {})", rect.width, rect.height, rect.x, rect.y);
}

/// A left-button press at window-local (@p localX, @p localY) of the test window.
void pushLeftButtonDown(int localX, int localY)
{
    SDL_Event event{};
    event.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    event.button.windowID = SDL_GetWindowID(Core::Application::get().getWindow().getHandle());
    event.button.button = SDL_BUTTON_LEFT;
    event.button.down = true;
    event.button.clicks = static_cast<Uint8>(1);
    event.button.x = static_cast<float>(localX);
    event.button.y = static_cast<float>(localY);
    static_cast<void>(SDL_PushEvent(&event));
}

/// What the edge-resize driver saw.
struct EdgeResizeObserved
{
    /// False under SDL's offscreen driver, which keeps a window where it was made, and on a desktop
    /// whose window manager ignored a direct setPosition() (#2707). CI always asserts.
    bool positionsApply = true;
    int checks = 0;  ///< Cases checked against where the window ended up
    int skipped = 0; ///< Cases that move the window's origin, skipped where positions don't apply
    std::vector<std::string> failures;
    int minimumWidth = 0;  ///< The minimum the resize held the window to...
    int minimumHeight = 0; ///< ...in window coordinates
    int frames = 0;
    int pressRetries = 0; ///< Presses made again because the one before had no effect (#2221)
    bool timedOut = false;
};

/// Pushed after UILayer and TitleBarLayer: presses each window edge and corner in turn through the
/// title bar's test seams -- SDL_PushEvent() for the press, setMouseStateQueryForTesting() for the
/// held pointer -- and checks where the window lands (#2717): the edge or corner pressed follows the
/// pointer and the opposite edge or edges stay put; a pull past the minimum size stops there, with
/// the right or bottom edge still pinned when the left or top one was pulled. The top-right corner
/// included: the Close button's slot reaches it, but the corner square is the resize border's, not the
/// button's (#2882), so it resizes like the other three.
///
/// Each case waits for the window to reach its expected rectangle, up to WAIT_LIMIT frames (the size
/// commits at most every ResizeTracker::SIZE_COMMIT_INTERVAL_SECONDS, and an X11 window manager
/// applies a move some frames later), then records the miss. A resize whose window hasn't changed for
/// RETRY_EVERY frames was lost -- TitleBarLayer ends it on the focus loss another test's window
/// mapping over this one causes under `xvfb-run ctest -j` (#2221) -- so the press is made again at
/// the same edge of the window as it is now, the pointer still headed where it was: a resize goes by
/// the pointer's movement from the edge it pressed, so the window still ends up where expected.
class EdgeResizeDriverLayer : public Core::Layer
{
  public:
    EdgeResizeDriverLayer(EdgeResizeObserved& observed, App::TitleBarLayer& titleBar)
        : Layer("EdgeResizeDriver"), m_Observed(observed), m_TitleBar(titleBar)
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
        const App::WindowRect now = currentRect(window);
        const auto next = [this, frame](Phase phase, int frames)
        {
            m_Phase = phase;
            m_NextFrame = frame + frames;
        };
        const auto nextCase = [this, &next]
        {
            next(++m_Case < CASES.size() ? Phase::Press : Phase::Stop, STEP);
        };
        const bool waiting = frame < m_Deadline;
        switch (m_Phase)
        {
        case Phase::ProbeMove:
        {
            // SDL's offscreen driver keeps a window where it was made, and WSLg's window manager
            // sometimes ignores client moves (#2707): ask for a move directly, and if it never lands,
            // skip the cases that move the window's origin. CI (TASKSMACK_REQUIRE_DISPLAY=1, no window
            // manager) asserts them all.
            const char* driver = SDL_GetCurrentVideoDriver();
            m_Observed.positionsApply = driver == nullptr || std::string_view(driver) != "offscreen";
            if (!m_Observed.positionsApply || TestSupport::displayRequired())
            {
                next(Phase::Press, STEP);
                break;
            }
            m_Start = now;
            window.setPosition(now.x + PROBE_X, now.y + PROBE_Y);
            m_Deadline = frame + WAIT_LIMIT;
            next(Phase::AwaitProbeMove, 1);
            break;
        }
        case Phase::AwaitProbeMove:
            if (sameRect(now, m_Start) && waiting)
            {
                next(Phase::AwaitProbeMove, 1);
                break;
            }
            m_Observed.positionsApply = !sameRect(now, m_Start);
            next(Phase::Press, STEP);
            break;
        case Phase::Press:
        {
            const Case& resizeCase = CASES.at(m_Case);
            if (movesTheWindowOrigin(resizeCase.edge) && !m_Observed.positionsApply)
            {
                ++m_Observed.skipped;
                nextCase();
                break;
            }
            m_Start = now;
            m_Minimum = resizeMinimum(window);
            m_Offset = resizeCase.pastMinimum ? pastMinimumOffset(resizeCase.edge, now) : resizeCase.offset;
            m_Expected = expectedResizeRect(resizeCase.edge, now, m_Offset, m_Minimum);
            if (resizeCase.pastMinimum)
            {
                m_Observed.minimumWidth = m_Minimum.first;
                m_Observed.minimumHeight = m_Minimum.second;
            }
            const auto [localX, localY] = resizePressPoint(resizeCase.edge, now.width, now.height);
            // No press point is a control's: the top-right corner's is inside the Close button's slot,
            // which the corner square is taken back out of (#2882).
            if (m_TitleBar.isPointInControlArea(static_cast<float>(localX), static_cast<float>(localY)))
            {
                check(false, std::format("{}: the press point ({}, {}) is on a control", resizeCase.name, localX, localY));
                nextCase();
                break;
            }
            m_Target = {now.x + localX + m_Offset.first, now.y + localY + m_Offset.second};
            press(resizeCase.edge, now, frame);
            m_Deadline = frame + WAIT_LIMIT;
            next(Phase::AwaitCase, 1);
            break;
        }
        case Phase::AwaitCase:
        {
            const Case& resizeCase = CASES.at(m_Case);
            const bool landed = withinRect(now, m_Expected);
            if (waiting && !landed)
            {
                if (!sameRect(now, m_LastSeen))
                {
                    m_LastSeen = now;
                    m_StillSince = frame;
                }
                else if (frame - m_StillSince >= RETRY_EVERY)
                {
                    press(resizeCase.edge, now, frame);
                    ++m_Observed.pressRetries;
                }
                next(Phase::AwaitCase, 1);
                break;
            }
            check(landed,
                  std::format("{}: pressed the {} window, pointer moved by ({}, {}), minimum {}x{}: expected {}, got {}",
                              resizeCase.name,
                              describeRect(m_Start),
                              m_Offset.first,
                              m_Offset.second,
                              m_Minimum.first,
                              m_Minimum.second,
                              describeRect(m_Expected),
                              describeRect(now)));
            release();
            nextCase();
            break;
        }
        case Phase::Stop:
            m_TitleBar.setMouseStateQueryForTesting({});
            Core::Application::get().stop();
            break;
        }
    }

  private:
    struct Case
    {
        std::string_view name;
        App::ResizeEdge edge;
        bool pastMinimum;           ///< The pointer moves by the window's whole size: the resize stops at the minimum
        std::pair<int, int> offset; ///< The pointer's movement from the press, when !pastMinimum
    };

    /// The pointer moved far enough into the window from @p edge to shrink it past any minimum.
    [[nodiscard]] static std::pair<int, int> pastMinimumOffset(App::ResizeEdge edge, const App::WindowRect& window) noexcept
    {
        int dx = 0;
        int dy = 0;
        if (pullsLeftEdge(edge))
        {
            dx = window.width;
        }
        else if (pullsRightEdge(edge))
        {
            dx = -window.width;
        }
        if (pullsTopEdge(edge))
        {
            dy = window.height;
        }
        else if (pullsBottomEdge(edge))
        {
            dy = -window.height;
        }
        return {dx, dy};
    }

    /// One case checked.
    void check(bool ok, std::string message)
    {
        ++m_Observed.checks;
        if (!ok)
        {
            m_Observed.failures.push_back(std::move(message));
        }
    }

    /// The press on @p edge of the window as it is now, @p window, with the pointer held at m_Target.
    void press(App::ResizeEdge edge, const App::WindowRect& window, int frame)
    {
        const auto [localX, localY] = resizePressPoint(edge, window.width, window.height);
        pushLeftButtonDown(localX, localY);
        const int globalX = m_Target.first;
        const int globalY = m_Target.second;
        m_TitleBar.setMouseStateQueryForTesting([globalX, globalY]
                                                { return std::tuple<int, int, SDL_MouseButtonFlags>{globalX, globalY, SDL_BUTTON_LMASK}; });
        m_LastSeen = window;
        m_StillSince = frame;
    }

    /// The button up where it was: the next update sees no button held and ends the resize.
    void release()
    {
        m_TitleBar.setMouseStateQueryForTesting([] { return std::tuple<int, int, SDL_MouseButtonFlags>{0, 0, 0U}; });
    }

    enum class Phase : std::uint8_t
    {
        ProbeMove,
        AwaitProbeMove,
        Press,
        AwaitCase,
        Stop,
    };

    static constexpr int SETTLE = 10;
    static constexpr int STEP = 5;          // frames between cases: the release takes effect on the next one
    static constexpr int WAIT_LIMIT = 300;  // frames to wait for a rectangle before recording the miss
    static constexpr int RETRY_EVERY = 30;  // frames with no change before a press is made again
    static constexpr int MAX_FRAMES = 4000; // room for every wait to run out
    static constexpr int PROBE_X = 40;
    static constexpr int PROBE_Y = 30;
    static constexpr int RESIZE_BY = 60;
    // The pointer moves right and down by RESIZE_BY on every edge and corner: the left and top edges
    // shrink the window, the right and bottom ones grow it, and an edge ignores the movement across it.
    static constexpr std::array<Case, 11> CASES{{
        {.name = "left edge", .edge = App::ResizeEdge::Left, .pastMinimum = false, .offset = {RESIZE_BY, RESIZE_BY}},
        {.name = "right edge", .edge = App::ResizeEdge::Right, .pastMinimum = false, .offset = {RESIZE_BY, RESIZE_BY}},
        {.name = "top edge", .edge = App::ResizeEdge::Top, .pastMinimum = false, .offset = {RESIZE_BY, RESIZE_BY}},
        {.name = "bottom edge", .edge = App::ResizeEdge::Bottom, .pastMinimum = false, .offset = {RESIZE_BY, RESIZE_BY}},
        {.name = "top-left corner", .edge = App::ResizeEdge::TopLeft, .pastMinimum = false, .offset = {RESIZE_BY, RESIZE_BY}},
        {.name = "top-right corner", .edge = App::ResizeEdge::TopRight, .pastMinimum = false, .offset = {RESIZE_BY, RESIZE_BY}},
        {.name = "bottom-left corner", .edge = App::ResizeEdge::BottomLeft, .pastMinimum = false, .offset = {RESIZE_BY, RESIZE_BY}},
        {.name = "bottom-right corner", .edge = App::ResizeEdge::BottomRight, .pastMinimum = false, .offset = {RESIZE_BY, RESIZE_BY}},
        {.name = "left edge past the minimum", .edge = App::ResizeEdge::Left, .pastMinimum = true, .offset = {}},
        {.name = "top edge past the minimum", .edge = App::ResizeEdge::Top, .pastMinimum = true, .offset = {}},
        {.name = "bottom-right corner past the minimum", .edge = App::ResizeEdge::BottomRight, .pastMinimum = true, .offset = {}},
    }};

  public:
    static constexpr int CASE_COUNT = static_cast<int>(CASES.size());

  private:
    EdgeResizeObserved& m_Observed;
    App::TitleBarLayer& m_TitleBar;
    Phase m_Phase = Phase::ProbeMove;
    int m_NextFrame = SETTLE;
    int m_Deadline = 0;
    std::size_t m_Case = 0;
    App::WindowRect m_Start{};    ///< The window at the case's first press
    App::WindowRect m_Expected{}; ///< Where it should end up
    App::WindowRect m_LastSeen{}; ///< The window on the last frame, while waiting
    int m_StillSince = 0;         ///< The frame it last changed, or was pressed
    std::pair<int, int> m_Minimum;
    std::pair<int, int> m_Offset;
    std::pair<int, int> m_Target; ///< Global: where the pointer is held
};

// #2717: the resize the title bar runs client-side, from every edge and corner, live. The edge or
// corner pressed follows the pointer and the opposite edge or edges stay put; a pull past the
// minimum size stops there, the far edge still pinned. The top-right corner resizes like the other
// three although the Close button's slot reaches it: the corner square is the border's (#2882). The
// maths is unit-tested in ComputeResizeGeometryTest and ResizeTrackerTest; this checks the window on
// a display ends up where it says.
TEST(UiLayersOnDisplayTest, TitleBarResizesFromEveryEdgeAndCornerAndStopsAtTheMinimumSize)
{
    if (!hasDisplay())
    {
        GTEST_SKIP() << "No display available (headless environment)";
    }

    Core::ApplicationSpecification spec;
    spec.Name = "UiLayersOnDisplayEdgeResizeTest";
    spec.VSync = false;
    EdgeResizeObserved observed;
    try
    {
        Core::Application app(spec);
        app.pushLayer<UI::UILayer>();
        auto& titleBar = app.pushLayer<App::TitleBarLayer>();
        app.pushLayer<EdgeResizeDriverLayer>(observed, titleBar);
        app.run();
    }
    catch (const std::exception& e)
    {
        FAIL() << "Application failed after the display probe passed: " << e.what();
    }

    RecordProperty("pressRetries", observed.pressRetries);
    RecordProperty("minimumWidth", observed.minimumWidth);
    RecordProperty("minimumHeight", observed.minimumHeight);
    RecordProperty("frames", observed.frames);
    EXPECT_FALSE(observed.timedOut);
    for (const std::string& failure : observed.failures)
    {
        ADD_FAILURE() << failure;
    }
    EXPECT_GE(observed.minimumWidth, Core::WINDOW_MIN_DIMENSION);
    EXPECT_GE(observed.minimumHeight, Core::WINDOW_MIN_DIMENSION);
    EXPECT_EQ(observed.checks + observed.skipped, EdgeResizeDriverLayer::CASE_COUNT);
    if (observed.positionsApply)
    {
        EXPECT_EQ(observed.skipped, 0);
    }
}

/// What the fast left-edge resize driver saw.
struct FastResizeObserved
{
    bool positionsApply = true; ///< As EdgeResizeObserved::positionsApply; a left-edge resize moves the window
    int checks = 0;             ///< Rectangles checked once the pointer stopped
    std::vector<std::string> failures;
    int rightEdgeAtPress = 0;
    /// Mid-sweep, before the held-back size commits, the moved origin shows with the old width
    /// (ResizeTracker holds the size back for SIZE_COMMIT_INTERVAL_SECONDS): how far off the right
    /// edge was at most, and on how many frames. Recorded, not asserted.
    int maxRightEdgeErrorWhileMoving = 0;
    int framesRightEdgeOffWhileMoving = 0;
    int frames = 0;
    int pressRetries = 0; ///< Presses made again because the resize was lost (#2221)
    bool timedOut = false;
};

/// Pushed after UILayer and TitleBarLayer: a fast resize from the left edge (#2717). The pointer is
/// pressed on the left edge and stepped STEP_PX on every frame, STEPS_PER_LEG frames into the window
/// and then the same back out, and after each leg, once the window has caught up, the right edge must
/// be where it was at the press and the top and bottom edges untouched: a resize goes by the pointer's
/// total movement from its press, so no step is lost or counted twice however fast the pointer moves.
/// The same once the button is let go. A lost resize is pressed again as EdgeResizeDriverLayer does.
class FastLeftResizeDriverLayer : public Core::Layer
{
  public:
    FastLeftResizeDriverLayer(FastResizeObserved& observed, App::TitleBarLayer& titleBar)
        : Layer("FastLeftResizeDriver"), m_Observed(observed), m_TitleBar(titleBar)
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
        const App::WindowRect now = currentRect(window);
        const auto next = [this, frame](Phase phase, int frames)
        {
            m_Phase = phase;
            m_NextFrame = frame + frames;
        };
        const bool waiting = frame < m_Deadline;
        switch (m_Phase)
        {
        case Phase::ProbeMove:
        {
            // As EdgeResizeDriverLayer: where moves don't land, the left edge can't be checked.
            const char* driver = SDL_GetCurrentVideoDriver();
            m_Observed.positionsApply = driver == nullptr || std::string_view(driver) != "offscreen";
            if (!m_Observed.positionsApply || TestSupport::displayRequired())
            {
                next(Phase::Press, STEP);
                break;
            }
            m_Start = now;
            window.setPosition(now.x + PROBE_X, now.y + PROBE_Y);
            m_Deadline = frame + WAIT_LIMIT;
            next(Phase::AwaitProbeMove, 1);
            break;
        }
        case Phase::AwaitProbeMove:
            if (sameRect(now, m_Start) && waiting)
            {
                next(Phase::AwaitProbeMove, 1);
                break;
            }
            m_Observed.positionsApply = !sameRect(now, m_Start);
            next(Phase::Press, STEP);
            break;
        case Phase::Press:
        {
            if (!m_Observed.positionsApply)
            {
                next(Phase::Stop, 1);
                break;
            }
            m_Start = now;
            m_Minimum = resizeMinimum(window);
            m_Observed.rightEdgeAtPress = now.x + now.width;
            const auto [localX, localY] = resizePressPoint(App::ResizeEdge::Left, now.width, now.height);
            m_Press = {now.x + localX, now.y + localY};
            m_Offset = 0;
            m_Leg = 0;
            m_StepsInLeg = 0;
            press(now, frame);
            next(Phase::Sweep, 1);
            break;
        }
        case Phase::Sweep:
        {
            // What the last step left: the origin moved at once, the size at most every commit interval.
            const int rightEdgeError = (now.x + now.width) - m_Observed.rightEdgeAtPress;
            m_Observed.maxRightEdgeErrorWhileMoving = std::max(m_Observed.maxRightEdgeErrorWhileMoving, std::abs(rightEdgeError));
            if (std::abs(rightEdgeError) > RESIZE_TOLERANCE_PX)
            {
                ++m_Observed.framesRightEdgeOffWhileMoving;
            }
            m_Offset += LEG_DIRECTIONS.at(m_Leg) * STEP_PX;
            hold();
            if (++m_StepsInLeg < STEPS_PER_LEG)
            {
                next(Phase::Sweep, 1);
                break;
            }
            m_Expected = expectedResizeRect(App::ResizeEdge::Left, m_Start, {m_Offset, 0}, m_Minimum);
            m_Deadline = frame + WAIT_LIMIT;
            m_LastSeen = now;
            m_StillSince = frame;
            next(Phase::AwaitLeg, 1);
            break;
        }
        case Phase::AwaitLeg:
        {
            const bool landed = withinRect(now, m_Expected);
            if (!landed && waiting)
            {
                if (!sameRect(now, m_LastSeen))
                {
                    m_LastSeen = now;
                    m_StillSince = frame;
                }
                else if (frame - m_StillSince >= RETRY_EVERY)
                {
                    press(now, frame);
                    ++m_Observed.pressRetries;
                }
                next(Phase::AwaitLeg, 1);
                break;
            }
            check(landed, std::format("after leg {} (pointer {} px from the press)", m_Leg + 1, m_Offset), now);
            if (++m_Leg < LEG_DIRECTIONS.size())
            {
                m_StepsInLeg = 0;
                next(Phase::Sweep, 1);
                break;
            }
            release();
            next(Phase::AfterRelease, STEP);
            break;
        }
        case Phase::AfterRelease:
            // Nothing was held back when the button went up: the window stays where the last leg left it.
            check(withinRect(now, m_Expected), "after the release", now);
            next(Phase::Stop, 1);
            break;
        case Phase::Stop:
            m_TitleBar.setMouseStateQueryForTesting({});
            Core::Application::get().stop();
            break;
        }
    }

  private:
    /// One rectangle checked once the pointer stopped.
    void check(bool ok, std::string_view when, const App::WindowRect& now)
    {
        ++m_Observed.checks;
        if (!ok)
        {
            m_Observed.failures.push_back(
                std::format("{}: pressed the left edge of the {} window; expected {}, got {} (right edge {} for {})",
                            when,
                            describeRect(m_Start),
                            describeRect(m_Expected),
                            describeRect(now),
                            now.x + now.width,
                            m_Observed.rightEdgeAtPress));
        }
    }

    /// The press on the left edge of the window as it is now, @p window, the pointer held where it is.
    void press(const App::WindowRect& window, int frame)
    {
        const auto [localX, localY] = resizePressPoint(App::ResizeEdge::Left, window.width, window.height);
        pushLeftButtonDown(localX, localY);
        hold();
        m_LastSeen = window;
        m_StillSince = frame;
    }

    /// The pointer m_Offset to the right of the press with the left button down, until release().
    void hold()
    {
        const int globalX = m_Press.first + m_Offset;
        const int globalY = m_Press.second;
        m_TitleBar.setMouseStateQueryForTesting([globalX, globalY]
                                                { return std::tuple<int, int, SDL_MouseButtonFlags>{globalX, globalY, SDL_BUTTON_LMASK}; });
    }

    void release()
    {
        m_TitleBar.setMouseStateQueryForTesting([] { return std::tuple<int, int, SDL_MouseButtonFlags>{0, 0, 0U}; });
    }

    enum class Phase : std::uint8_t
    {
        ProbeMove,
        AwaitProbeMove,
        Press,
        Sweep,
        AwaitLeg,
        AfterRelease,
        Stop,
    };

    static constexpr int SETTLE = 10;
    static constexpr int STEP = 5; // frames after the release before the check: it takes effect on the next one
    static constexpr int WAIT_LIMIT = 300;
    static constexpr int RETRY_EVERY = 30;
    static constexpr int MAX_FRAMES = 1500; // room for every wait to run out
    static constexpr int PROBE_X = 40;
    static constexpr int PROBE_Y = 30;
    static constexpr int STEP_PX = 24;       // per frame: about 1,400 px/s at the loop's 60 fps
    static constexpr int STEPS_PER_LEG = 25; // 600 px into the window, then back out
    static constexpr std::array<int, 2> LEG_DIRECTIONS{1, -1};

    FastResizeObserved& m_Observed;
    App::TitleBarLayer& m_TitleBar;
    Phase m_Phase = Phase::ProbeMove;
    int m_NextFrame = SETTLE;
    int m_Deadline = 0;
    App::WindowRect m_Start{};    ///< The window at the press
    App::WindowRect m_Expected{}; ///< Where the leg should leave it
    App::WindowRect m_LastSeen{}; ///< The window on the last frame, while waiting
    int m_StillSince = 0;
    std::pair<int, int> m_Minimum;
    std::pair<int, int> m_Press; ///< Global: where the pointer pressed
    int m_Offset = 0;            ///< How far right of the press the pointer is held
    std::size_t m_Leg = 0;
    int m_StepsInLeg = 0;

  public:
    static constexpr int CHECK_COUNT = static_cast<int>(LEG_DIRECTIONS.size()) + 1; ///< Each leg, then the release
};

// #2717: a fast resize from the left edge -- the pointer stepped STEP_PX on every frame, 600 px in
// and back out -- leaves the right edge where it was and the top and bottom edges untouched, after
// each leg and after the release: no drift.
TEST(UiLayersOnDisplayTest, AFastLeftEdgeResizeKeepsTheRightEdgeWhereItWas)
{
    if (!hasDisplay())
    {
        GTEST_SKIP() << "No display available (headless environment)";
    }

    Core::ApplicationSpecification spec;
    spec.Name = "UiLayersOnDisplayFastResizeTest";
    spec.VSync = false;
    FastResizeObserved observed;
    try
    {
        Core::Application app(spec);
        app.pushLayer<UI::UILayer>();
        auto& titleBar = app.pushLayer<App::TitleBarLayer>();
        app.pushLayer<FastLeftResizeDriverLayer>(observed, titleBar);
        app.run();
    }
    catch (const std::exception& e)
    {
        FAIL() << "Application failed after the display probe passed: " << e.what();
    }

    RecordProperty("pressRetries", observed.pressRetries);
    RecordProperty("maxRightEdgeErrorWhileMoving", observed.maxRightEdgeErrorWhileMoving);
    RecordProperty("framesRightEdgeOffWhileMoving", observed.framesRightEdgeOffWhileMoving);
    RecordProperty("frames", observed.frames);
    EXPECT_FALSE(observed.timedOut);
    if (!observed.positionsApply)
    {
        GTEST_SKIP() << "This display doesn't apply window moves, so a left-edge resize can't be checked";
    }
    for (const std::string& failure : observed.failures)
    {
        ADD_FAILURE() << failure;
    }
    EXPECT_EQ(observed.checks, FastLeftResizeDriverLayer::CHECK_COUNT);
}

/// What the hit-test driver saw (#2716).
struct HitTestObserved
{
    int checks = 0; ///< Points asked
    std::vector<std::string> failures;
    SDL_HitTestResult title = SDL_HITTEST_NORMAL;          ///< The empty title bar
    SDL_HitTestResult rightEdge = SDL_HITTEST_NORMAL;      ///< The right border, below the bar
    SDL_HitTestResult maximizedTitle = SDL_HITTEST_NORMAL; ///< The empty title bar of the maximized window
    bool systemDrag = false;                               ///< Core::VideoBackend::usesSystemDrag() while the app ran
    bool systemResize = false;                             ///< Core::VideoBackend::usesSystemResize()
    bool clientSideMaximize = false;                       ///< Core::VideoBackend::supportsClientSideMaximize()
    bool maximized = false;                                ///< The window maximized for the second round
    bool maximizeUnavailable = false;                      ///< It never did: that round was skipped
    int frames = 0;
    bool timedOut = false;
};

/// Pushed after UILayer and TitleBarLayer: once the bar has drawn, asks the window's hit test --
/// TitleBarLayer::hitTest(), the function the callback installed on the real window forwards to --
/// what lies under the empty title bar, each button, the icon, every edge and corner and the content,
/// against what the backend's policy says it should answer; on Windows also asks the window itself
/// with WM_NCHITTEST, as Windows does when the pointer moves, so the installed callback and SDL's
/// mapping of its answers are covered too. Then maximizes the window and asks again: no resize
/// answers, and the title bar as the policy has it for a maximized window. Nothing is clicked or moved.
class HitTestDriverLayer : public Core::Layer
{
  public:
    HitTestDriverLayer(HitTestObserved& observed, App::TitleBarLayer& titleBar)
        : Layer("HitTestDriver"), m_Observed(observed), m_TitleBar(titleBar)
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
        Core::Window& window = Core::Application::get().getWindow();
        const auto [width, height] = window.getSize();
        const bool maximized = window.isMaximized();
        const auto next = [this, frame](Phase phase, int frames)
        {
            m_Phase = phase;
            m_NextFrame = frame + frames;
        };
        const bool waiting = frame < m_Deadline;
        switch (m_Phase)
        {
        case Phase::AskNormal:
            m_Observed.systemDrag = Core::VideoBackend::usesSystemDrag();
            m_Observed.systemResize = Core::VideoBackend::usesSystemResize();
            m_Observed.clientSideMaximize = Core::VideoBackend::supportsClientSideMaximize();
            askNormal(width, height);
            window.maximize();
            m_Deadline = frame + WAIT_LIMIT;
            next(Phase::AwaitMaximized, 1);
            break;
        case Phase::AwaitMaximized:
            if (!maximized && waiting)
            {
                next(Phase::AwaitMaximized, 1);
                break;
            }
            if (!maximized)
            {
                m_Observed.maximizeUnavailable = true;
                next(Phase::Stop, 1);
                break;
            }
            m_Geometry = {};
            m_StableFrames = 0;
            m_Deadline = frame + WAIT_LIMIT;
            next(Phase::AwaitMaximizedSettled, 1);
            break;
        case Phase::AwaitMaximizedSettled:
        {
            // The maximized flag can lead the window's new size by some frames (X11 reports the
            // configure later): wait until the geometry has held for SETTLED_FRAMES.
            const std::array<int, 2> geometry{width, height};
            m_StableFrames = geometry == m_Geometry ? m_StableFrames + 1 : 0;
            m_Geometry = geometry;
            if (m_StableFrames < SETTLED_FRAMES && waiting)
            {
                next(Phase::AwaitMaximizedSettled, 1);
                break;
            }
            m_Observed.maximized = true;
            askMaximized(width, height);
            window.restore();
            next(Phase::Stop, STEP);
            break;
        }
        case Phase::Stop:
            Core::Application::get().stop();
            break;
        }
    }

  private:
    /// One point asked: what the window answers for (@p x, @p y) against @p expected.
    SDL_HitTestResult check(const char* what, float x, float y, SDL_HitTestResult expected)
    {
        const SDL_HitTestResult got = m_TitleBar.hitTest(x, y);
        ++m_Observed.checks;
        if (got != expected)
        {
            m_Observed.failures.push_back(
                std::format("{} at ({}, {}): answered {}, expected {}", what, x, y, static_cast<int>(got), static_cast<int>(expected)));
        }
        return got;
    }

    void askNormal(int width, int height)
    {
        const App::WindowHitTestInput input = m_TitleBar.hitTestInput();
        const auto w = static_cast<float>(width);
        const auto h = static_cast<float>(height);
        const float barMiddleY = input.titleBarHeightPx * 0.5F;
        const SDL_HitTestResult drag = m_Observed.systemDrag ? SDL_HITTEST_DRAGGABLE : SDL_HITTEST_NORMAL;
        const auto resize = [this](SDL_HitTestResult edge)
        {
            return m_Observed.systemResize ? edge : SDL_HITTEST_NORMAL;
        };

        m_Observed.title = check("the empty title bar", w * 0.5F, barMiddleY, drag);
        check("the gap before the window controls", input.buttons.minimize.minX - 1.0F, barMiddleY, drag);
        const auto& b = input.buttons;
        const std::array<std::pair<const char*, const App::ButtonBounds*>, 6> buttons{{{"About", &b.about},
                                                                                       {"Help", &b.help},
                                                                                       {"Settings", &b.settings},
                                                                                       {"Minimize", &b.minimize},
                                                                                       {"Maximize", &b.maximize},
                                                                                       {"Close", &b.close}}};
        for (const auto& [name, bounds] : buttons)
        {
            check(name, (bounds->minX + bounds->maxX) * 0.5F, (bounds->minY + bounds->maxY) * 0.5F, SDL_HITTEST_NORMAL);
        }
        if (input.icon.maxX > input.icon.minX)
        {
            check("the icon", (input.icon.minX + input.icon.maxX) * 0.5F, (input.icon.minY + input.icon.maxY) * 0.5F, SDL_HITTEST_NORMAL);
        }
        check("the left edge", 1.0F, h * 0.5F, resize(SDL_HITTEST_RESIZE_LEFT));
        m_Observed.rightEdge = check("the right edge", w - 2.0F, h * 0.5F, resize(SDL_HITTEST_RESIZE_RIGHT));
        check("the top edge", w * 0.5F, 1.0F, resize(SDL_HITTEST_RESIZE_TOP));
        check("the bottom edge", w * 0.5F, h - 2.0F, resize(SDL_HITTEST_RESIZE_BOTTOM));
        check("the top-left corner", 1.0F, 1.0F, resize(SDL_HITTEST_RESIZE_TOPLEFT));
        check("the bottom-left corner", 1.0F, h - 2.0F, resize(SDL_HITTEST_RESIZE_BOTTOMLEFT));
        check("the bottom-right corner", w - 2.0F, h - 2.0F, resize(SDL_HITTEST_RESIZE_BOTTOMRIGHT));
        // The top-right corner is the border's although the Close button's slot reaches it (#2882); one
        // pixel past the corner square, left of it or below it, is the button.
        const float border = input.resizeBorderPx;
        check("the top-right corner", w - 2.0F, 1.0F, resize(SDL_HITTEST_RESIZE_TOPRIGHT));
        check("the top-right corner's innermost pixel", w - border, border - 1.0F, resize(SDL_HITTEST_RESIZE_TOPRIGHT));
        check("the close button left of the corner", w - border - 1.0F, border - 1.0F, SDL_HITTEST_NORMAL);
        check("the close button below the corner", w - 2.0F, border, SDL_HITTEST_NORMAL);
        check("the content", w * 0.5F, input.titleBarHeightPx + 50.0F, SDL_HITTEST_NORMAL);
#ifdef _WIN32
        askWindows(width, height, b.close, barMiddleY, drag, resize(SDL_HITTEST_RESIZE_RIGHT), resize(SDL_HITTEST_RESIZE_TOPRIGHT));
#endif
    }

    void askMaximized(int width, int height)
    {
        const App::WindowHitTestInput input = m_TitleBar.hitTestInput();
        const auto w = static_cast<float>(width);
        const auto h = static_cast<float>(height);
        const SDL_HitTestResult drag =
            App::computeSystemDragApplies(m_Observed.systemDrag, m_Observed.clientSideMaximize, /*isMaximized=*/true)
                ? SDL_HITTEST_DRAGGABLE
                : SDL_HITTEST_NORMAL;
        m_Observed.maximizedTitle = check("the maximized window's empty title bar", w * 0.5F, input.titleBarHeightPx * 0.5F, drag);
        check("the maximized window's top strip", w * 0.5F, 1.0F, drag); // no resize border: the bar
        check("the maximized window's right edge", w - 2.0F, h * 0.5F, SDL_HITTEST_NORMAL);
        check("the maximized window's bottom-left corner", 1.0F, h - 2.0F, SDL_HITTEST_NORMAL);
        check("the maximized window's close button", w - 2.0F, 1.0F, SDL_HITTEST_NORMAL);
    }

#ifdef _WIN32
    /// Asks the window itself, as Windows does when the pointer moves: the window procedure hands
    /// WM_NCHITTEST to the installed callback, and SDL maps its answer to a Win32 hit-test code --
    /// DRAGGABLE is HTCAPTION, which is what Aero Snap needs. Only a question: nothing is clicked.
    void askWindows(int width,
                    int height,
                    const App::ButtonBounds& close,
                    float barMiddleY,
                    SDL_HitTestResult title,
                    SDL_HitTestResult rightEdge,
                    SDL_HitTestResult topRightCorner)
    {
        const SDL_PropertiesID props = SDL_GetWindowProperties(Core::Application::get().getWindow().getHandle());
        auto* hwnd = static_cast<HWND>(SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
        if (hwnd == nullptr)
        {
            m_Observed.failures.emplace_back("no HWND for the window");
            return;
        }
        const auto ask = [hwnd](float x, float y)
        {
            POINT point{.x = static_cast<LONG>(x), .y = static_cast<LONG>(y)};
            ClientToScreen(hwnd, &point);
            return SendMessageW(hwnd, WM_NCHITTEST, 0, MAKELPARAM(point.x, point.y));
        };
        const auto code = [](SDL_HitTestResult result) -> LRESULT
        {
            switch (result)
            {
            case SDL_HITTEST_DRAGGABLE:
                return HTCAPTION;
            case SDL_HITTEST_RESIZE_RIGHT:
                return HTRIGHT;
            case SDL_HITTEST_RESIZE_TOPRIGHT:
                return HTTOPRIGHT;
            default:
                return HTCLIENT;
            }
        };
        const auto checkWindows = [this, &ask, &code](const char* what, float x, float y, SDL_HitTestResult expected)
        {
            const LRESULT got = ask(x, y);
            ++m_Observed.checks;
            if (got != code(expected))
            {
                m_Observed.failures.push_back(
                    std::format("WM_NCHITTEST on {} at ({}, {}): answered {}, expected {}", what, x, y, got, code(expected)));
            }
        };
        checkWindows("the empty title bar", static_cast<float>(width) * 0.5F, barMiddleY, title);
        checkWindows("the Close button", (close.minX + close.maxX) * 0.5F, (close.minY + close.maxY) * 0.5F, SDL_HITTEST_NORMAL);
        checkWindows("the right edge", static_cast<float>(width) - 2.0F, static_cast<float>(height) * 0.5F, rightEdge);
        // The corner the Close button's slot reaches (#2882): the border's, so on Windows HTCLIENT like
        // every border, and the press comes to the app's resize rather than the OS's modal one.
        checkWindows("the top-right corner", static_cast<float>(width) - 2.0F, 1.0F, topRightCorner);
    }
#endif

    enum class Phase : std::uint8_t
    {
        AskNormal,
        AwaitMaximized,
        AwaitMaximizedSettled,
        Stop,
    };

    static constexpr int SETTLE = 10; // frames before the first question: the bar has drawn and cached its layout
    static constexpr int STEP = 10;
    static constexpr int WAIT_LIMIT = 300;
    static constexpr int SETTLED_FRAMES = 15;
    static constexpr int MAX_FRAMES = 900;

    HitTestObserved& m_Observed;
    App::TitleBarLayer& m_TitleBar;
    Phase m_Phase = Phase::AskNormal;
    int m_NextFrame = SETTLE;
    int m_Deadline = 0;
    std::array<int, 2> m_Geometry{};
    int m_StableFrames = 0;
};

// #2716: the hit-test callback TitleBarLayer installs on the real window answers for the title bar, the
// buttons and the edges as the backend's policy says: on Windows the empty title bar is DRAGGABLE
// (HTCAPTION to Windows, so its own move loop and Aero Snap run the drag) and the edges stay the app's
// resize; on X11 the edges are the window manager's and the bar the app's; on native Wayland both are
// the compositor's. Maximized, nothing is a resize edge.
TEST(UiLayersOnDisplayTest, TheHitTestCallbackAnswersForTheRealWindow)
{
    if (!hasDisplay())
    {
        GTEST_SKIP() << "No display available (headless environment)";
    }

    Core::ApplicationSpecification spec;
    spec.Name = "UiLayersOnDisplayHitTestTest";
    spec.VSync = false;
    HitTestObserved observed;
    try
    {
        Core::Application app(spec);
        app.pushLayer<UI::UILayer>();
        auto& titleBar = app.pushLayer<App::TitleBarLayer>();
        app.pushLayer<HitTestDriverLayer>(observed, titleBar);
        app.run();
    }
    catch (const std::exception& e)
    {
        FAIL() << "Application failed after the display probe passed: " << e.what();
    }

    EXPECT_FALSE(observed.timedOut);
    for (const std::string& failure : observed.failures)
    {
        ADD_FAILURE() << failure;
    }
    EXPECT_GE(observed.checks, 17); // the bar, the gap, 6 buttons, 7 edges and corners, the close corner, the content
    EXPECT_EQ(observed.title, observed.systemDrag ? SDL_HITTEST_DRAGGABLE : SDL_HITTEST_NORMAL);
    EXPECT_EQ(observed.rightEdge, observed.systemResize ? SDL_HITTEST_RESIZE_RIGHT : SDL_HITTEST_NORMAL);
#ifdef _WIN32
    EXPECT_TRUE(observed.systemDrag);
    EXPECT_FALSE(observed.systemResize);
    EXPECT_EQ(observed.title, SDL_HITTEST_DRAGGABLE);
    EXPECT_EQ(observed.rightEdge, SDL_HITTEST_NORMAL);
#endif
    if (observed.maximizeUnavailable)
    {
        GTEST_SKIP() << "The window never maximized on this display, so the maximized round was skipped";
    }
    EXPECT_TRUE(observed.maximized);
    EXPECT_EQ(observed.maximizedTitle,
              App::computeSystemDragApplies(observed.systemDrag, observed.clientSideMaximize, /*isMaximized=*/true) ? SDL_HITTEST_DRAGGABLE
                                                                                                                    : SDL_HITTEST_NORMAL);
#ifdef _WIN32
    EXPECT_EQ(observed.maximizedTitle, SDL_HITTEST_NORMAL); // the restore under the pointer stays the app's (#2715)
#endif
}

/// What the UI-layer driver saw.
struct UiLayerObserved
{
    bool userThemeLoaded = false;
    int drawCalls = 0;
    int frames = 0;
    std::uint64_t fontGeneration = 0; ///< Theme's, while the layer was attached
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
            m_Observed.fontGeneration = UI::Theme::get().fontGeneration();
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
    // #2163: the fonts went with the ImGui context in onDetach(), so Theme keeps none of them, and a
    // cache keyed on (font, fontGeneration()) can't take a new font at a reused address for an old one.
    EXPECT_EQ(UI::Theme::get().regularFont(), nullptr);
    EXPECT_GT(UI::Theme::get().fontGeneration(), observed.fontGeneration);
}

/// What the shell-shortcuts driver saw.
struct ShellKeysObserved
{
    bool detailsTabShown = false;
    bool servicesTabShown = false; // #1886: a ShowServiceEvent brings the Services tab forward
    bool treeViewToggled = false;  // F5 on the Processes tab: list to tree or back (#1891)
    bool treeViewToggledBack = false;
    bool metricsOnAfterToggle = false;
    bool metricsOffAfterSecondToggle = true;
    bool fontGrew = false;
    bool fontShrankBack = false;
    int frames = 0;
    bool timedOut = false;
};

/// Pushed last over the whole shell: the row menu's Details for a process (as
/// ProcessesPanel raises it), F5 twice on the Processes tab, Ctrl+Shift+M twice, then Ctrl+= and Ctrl+-.
class ShellKeysDriverLayer : public Core::Layer
{
  public:
    ShellKeysDriverLayer(ShellKeysObserved& observed, const App::ShellLayer& shell)
        : Layer("ShellKeysDriver"), m_Observed(observed), m_Shell(shell)
    {}

    void onAttach() override
    {
        keepKeyPressesThroughFocusLoss();
    }

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
        // times (#1950): CI's xvfb dropped an injected key now and then (only a Ctrl+= was logged, never
        // the Ctrl+- after it), as #1845's button presses were. A focus loss on the press's frame drops
        // it like that, which onAttach() now prevents (#2268); the retries stay for any other cause. A
        // press taken twice still fails the checks that the state came back.
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
            startWait();
            next(Phase::AwaitProcessesTab, 1);
            break;
        case Phase::AwaitProcessesTab:
            // F5 goes to the tab drawn this frame: wait until that is Processes (#1891). A lost activation
            // is pressed again, as a lost key is.
            static_cast<void>(awaitThen(selectedTab() == tabId("###ProcessesTab"), Phase::PressF5, Phase::CheckServices));
            break;
        case Phase::PressF5:
        case Phase::PressF5Back:
            // No key repeat: these shortcuts are read with IsKeyPressed(), which repeats a key held past
            // KeyRepeatDelay, and one slow frame between a press and its release on a loaded runner did
            // that -- Ctrl+= grew the font two presets and Ctrl+- shrank it one (fontShrankBack failed).
            io.KeyRepeatDelay = 3600.0F;
            if (m_Phase == Phase::PressF5 && m_Retries == 0)
            {
                m_TreeBefore = m_Shell.processesTreeViewForTesting(); // not again on a retry
            }
            io.AddKeyEvent(ImGuiKey_F5, true); // the list/tree toggle, routed to the Processes tab
            next(m_Phase == Phase::PressF5 ? Phase::ReleaseF5 : Phase::ReleaseF5Back, 1);
            break;
        case Phase::ReleaseF5:
        case Phase::ReleaseF5Back:
            io.AddKeyEvent(ImGuiKey_F5, false);
            startWait();
            next(m_Phase == Phase::ReleaseF5 ? Phase::AwaitTree : Phase::AwaitTreeBack, 1);
            break;
        case Phase::AwaitTree:
            m_Observed.treeViewToggled = m_Shell.processesTreeViewForTesting() != m_TreeBefore;
            static_cast<void>(awaitThen(m_Observed.treeViewToggled, Phase::PressF5Back, Phase::PressF5));
            break;
        case Phase::AwaitTreeBack:
            m_Observed.treeViewToggledBack = m_Shell.processesTreeViewForTesting() == m_TreeBefore;
            static_cast<void>(awaitThen(m_Observed.treeViewToggledBack, Phase::PressMetricsOn, Phase::PressF5Back));
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
        AwaitProcessesTab,
        PressF5,
        ReleaseF5,
        AwaitTree,
        PressF5Back,
        ReleaseF5Back,
        AwaitTreeBack,
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
    static constexpr int MAX_FRAMES = 2400; // room for every wait to run out

    ShellKeysObserved& m_Observed;
    const App::ShellLayer& m_Shell;
    bool m_TreeBefore = false;
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
        auto& shell = app.pushLayer<App::ShellLayer>();
        shell.setTitleBar(&titleBar);
        app.pushLayer<ShellKeysDriverLayer>(observed, shell);
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
    EXPECT_TRUE(observed.treeViewToggled);              // F5 on the Processes tab switches list and tree...
    EXPECT_TRUE(observed.treeViewToggledBack);          // ...and back
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

    void onAttach() override
    {
        keepKeyPressesThroughFocusLoss();
    }

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

/// What the first-run driver saw (#2727).
struct FirstRunObserved
{
    // The window once the shell has settled, in window units (physical pixels on Windows and X11).
    bool windowChecked = false;
    int width = 0;
    int height = 0;
    int x = 0;
    int y = 0;
    float unitScale = 0.0F;
    bool maximized = true;
    std::optional<SDL_Rect> usable; // the usable bounds (work area) of the display the window is on
    bool canMoveWindows = false;    // the video driver can move a window (not SDL's offscreen one)
    // The shell on that frame.
    bool systemTabSelected = false; // the first tab, the machine's, is the default (tasksmack.md, ShellLayer)
    bool titleBarShown = false;
    bool statusBarShown = false;
    // The Processes tab once pressed, on the first frame it draws its table.
    bool processesTabSelected = false;
    bool processTableFound = false;
    std::vector<std::string> columnProblems; // each column that isn't at its default, and why
    int frames = 0;
    bool timedOut = false;
};

/// Pushed last over the whole shell as main.cpp composes it: lets the layout settle, records the window
/// and the shell as a first launch shows them, presses the Processes tab and records its table once it
/// is drawn, then presses Close.
class FirstRunDriverLayer : public Core::Layer
{
  public:
    explicit FirstRunDriverLayer(FirstRunObserved& observed) : Layer("FirstRunDriver"), m_Observed(observed)
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
        if (frame == SETTLE)
        {
            observeWindow();
            m_Observed.systemTabSelected = selectedTab() == tabId("###SystemTab");
            m_Observed.titleBarShown = shownThisFrame("##TitleBar");
            m_Observed.statusBarShown = shownThisFrame("##StatusBar");
            ImGui::ActivateItemByID(tabId("###ProcessesTab"));
        }
        // The table, once its first samples are in (they can take a while on a loaded runner), or at
        // WAIT_LIMIT without it, which then fails.
        else if (frame > SETTLE && m_ObservedFrame == 0 && (findProcessTable() != nullptr || frame >= SETTLE + WAIT_LIMIT))
        {
            m_Observed.processesTabSelected = selectedTab() == tabId("###ProcessesTab");
            observeProcessTable();
            m_ObservedFrame = frame;
        }
        // The unlabelled Close (chrome icon font) on one frame and the labelled one on the next: one
        // ActivateItemByID() request per frame is kept (#1954).
        else if (m_ObservedFrame != 0 && frame == m_ObservedFrame + 1)
        {
            pressOnTitleBar("##Close");
        }
        else if (m_ObservedFrame != 0 && frame == m_ObservedFrame + 2)
        {
            pressOnTitleBar(ICON_FA_XMARK "##Close");
        }
    }

  private:
    void observeWindow()
    {
        const Core::Window& window = Core::Application::get().getWindow();
        std::tie(m_Observed.width, m_Observed.height) = window.getSize();
        std::tie(m_Observed.x, m_Observed.y) = window.getPosition();
        m_Observed.unitScale = window.getUnitScale();
        m_Observed.maximized = window.isMaximized();
        if (SDL_Rect bounds{}; SDL_GetDisplayUsableBounds(SDL_GetDisplayForWindow(window.getHandle()), &bounds))
        {
            m_Observed.usable = bounds;
        }
        m_Observed.canMoveWindows = Core::Window::supportsPositioning() && Core::VideoBackend::driverName() != "offscreen";
        m_Observed.windowChecked = true;
    }

    void observeProcessTable()
    {
        const ImGuiTable* table = findProcessTable();
        m_Observed.processTableFound = table != nullptr;
        if (table == nullptr)
        {
            return;
        }
        for (const App::ProcessColumn col : App::allProcessColumns())
        {
            const auto index = static_cast<int>(App::toIndex(col));
            const ImGuiTableColumn& column = table->Columns[index];
            const App::ProcessColumnInfo info = App::getColumnInfo(col);
            const std::string key(info.configKey);
            // Shown: a default column, unless this system can't fill it (ProcessColumnAvailability,
            // #1210); the ones that can't be hidden always. Hidden: every other.
            if (column.IsEnabled && !info.defaultVisible)
            {
                m_Observed.columnProblems.push_back(key + " is shown but is not a default column");
            }
            if (!column.IsEnabled && !info.canHide)
            {
                m_Observed.columnProblems.push_back(key + " can't be hidden but is");
            }
            if (column.IsEnabled == ((column.Flags & ImGuiTableColumnFlags_DefaultHide) != 0))
            {
                m_Observed.columnProblems.push_back(key + " isn't shown as the panel's default columns say");
            }
            if (column.DisplayOrder != index)
            {
                m_Observed.columnProblems.push_back(key + " is at position " + std::to_string(column.DisplayOrder));
            }
            // Sorted by CPU %, highest first, and by nothing else.
            const bool sortColumn = col == App::ProcessColumn::CpuPercent;
            if (sortColumn && (column.SortOrder != 0 || column.SortDirection != ImGuiSortDirection_Descending))
            {
                m_Observed.columnProblems.push_back(key + " is not the descending sort");
            }
            if (!sortColumn && column.SortOrder != -1)
            {
                m_Observed.columnProblems.push_back(key + " is sorted on");
            }
        }
    }

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

    /// The Processes table: the one whose columns carry ProcessColumn's values as their user data, in
    /// order (ProcessesPanel's TableSetupColumn() calls), drawn this frame.
    [[nodiscard]] static const ImGuiTable* findProcessTable()
    {
        const int count = GImGui->Tables.GetMapSize();
        for (int i = 0; i < count; ++i)
        {
            const ImGuiTable* table = GImGui->Tables.TryGetMapData(i);
            if (table == nullptr || std::cmp_not_equal(table->ColumnsCount, App::processColumnCount()) ||
                table->LastFrameActive != ImGui::GetFrameCount())
            {
                continue;
            }
            bool ids = true;
            for (int column = 0; column < table->ColumnsCount; ++column)
            {
                ids = ids && std::cmp_equal(table->Columns[column].UserData, column);
            }
            if (ids)
            {
                return table;
            }
        }
        return nullptr;
    }

    [[nodiscard]] static bool shownThisFrame(const char* name)
    {
        const ImGuiWindow* window = ImGui::FindWindowByName(name);
        return window != nullptr && window->Active && !window->Hidden;
    }

    static void pressOnTitleBar(const char* label)
    {
        if (const ImGuiWindow* bar = ImGui::FindWindowByName("##TitleBar"); bar != nullptr)
        {
            ImGui::ActivateItemByID(ImHashStr(label, 0, bar->ID));
        }
    }

    static constexpr int SETTLE = 20;      // the layout and the minimum window size
    static constexpr int WAIT_LIMIT = 900; // frames to wait for the Processes table
    static constexpr int MAX_FRAMES = SETTLE + WAIT_LIMIT + 120;

    FirstRunObserved& m_Observed;
    int m_ObservedFrame = 0; // 0 until observed
};

/// The config.toml keys that differ from one machine to the next, as section and key (#2727). Their
/// values are replaced by "<masked>" before the file is compared with the golden one:
///  - [window] x, y: where the window manager placed it on this display;
///  - [window] width, height, scale: the default 1280x720 at 100 %, converted to the display's scale
///    (window units are physical pixels on Windows), and fitted to a display smaller than that;
///  - [process_table] layout: ImGui's own table settings, whose column widths are measured in the
///    current font at this display's scale, and whose shown columns follow what this system can fill.
/// The test checks each of them against the live window and table instead.
constexpr std::array<std::pair<std::string_view, std::string_view>, 6> MASKED_CONFIG_KEYS{{
    {"window", "x"},
    {"window", "y"},
    {"window", "width"},
    {"window", "height"},
    {"window", "scale"},
    {"process_table", "layout"},
}};

constexpr std::string_view MASK = "\"<masked>\"";

/// @p text with line endings made LF and each MASKED_CONFIG_KEYS value replaced by MASK, a
/// multi-line string value included.
std::string maskMachineDependentKeys(std::string_view text)
{
    std::string out;
    std::string section;
    std::string_view closing; // the delimiter that ends the masked multi-line string being skipped
    std::size_t start = 0;
    while (start < text.size())
    {
        const std::size_t end = std::min(text.find('\n', start), text.size());
        std::string_view line = text.substr(start, end - start);
        start = end + 1;
        if (line.ends_with('\r'))
        {
            line.remove_suffix(1);
        }
        if (!closing.empty())
        {
            if (line.contains(closing))
            {
                closing = {};
            }
            continue;
        }
        const std::size_t indent = std::min(line.find_first_not_of(" \t"), line.size());
        const std::string_view body = line.substr(indent);
        if (body.starts_with('['))
        {
            section = std::string(body.substr(1, body.find(']') - 1));
        }
        const auto masked = std::ranges::find_if(
            MASKED_CONFIG_KEYS,
            [&](const auto& key)
            { return key.first == section && body.starts_with(key.second) && body.substr(key.second.size()).starts_with(" = "); });
        if (masked == MASKED_CONFIG_KEYS.end())
        {
            out.append(line).push_back('\n');
            continue;
        }
        const std::string_view value = body.substr(masked->second.size() + 3);
        for (const std::string_view delimiter : {std::string_view("'''"), std::string_view(R"(""")")})
        {
            if (value.starts_with(delimiter) && value.find(delimiter, delimiter.size()) == std::string_view::npos)
            {
                closing = delimiter;
            }
        }
        out.append(line.substr(0, indent)).append(masked->second).append(" = ").append(MASK).push_back('\n');
    }
    return out;
}

/// The whole of @p path, or nullopt when it can't be read.
std::optional<std::string> readWholeFile(const std::filesystem::path& path)
{
    const std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        return std::nullopt;
    }
    std::ostringstream text;
    text << file.rdbuf();
    return std::move(text).str();
}

TEST(UiLayersOnDisplayTest, AFirstRunOpensAtTheDefaultsAndSavesTheGoldenConfig)
{
    if (!hasDisplay())
    {
        GTEST_SKIP() << "No display available (headless environment)";
    }
    // A first launch: an empty TASKSMACK_CONFIG_DIR (AppTestsMain.cpp's temporary directory, which the
    // tests before this one may have saved to) and UserConfig back at its defaults.
    const std::optional<std::filesystem::path>& configOverride = Core::ConfigDirOverride::active();
    ASSERT_TRUE(configOverride.has_value());
    const std::filesystem::path configDir = configOverride.value_or(std::filesystem::path{});
    std::error_code ec;
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(configDir, ec))
    {
        std::filesystem::remove_all(entry.path(), ec);
    }
    ASSERT_TRUE(std::filesystem::is_empty(configDir, ec)) << ec.message();
    App::UserConfig& config = App::UserConfig::get();
    config.resetConfigPathForTesting(configDir / "config.toml");

    // Composed as main.cpp's runApp() composes it: the size from the loaded settings, the saved
    // geometry applied, then the same layers in the same order.
    config.load();
    const App::UserSettings& settings = config.settings();
    Core::ApplicationSpecification spec;
    spec.Name = "UiLayersOnDisplayFirstRunTest";
    spec.Width = std::clamp(settings.windowWidth, Core::WINDOW_MIN_DIMENSION, Core::WINDOW_MAX_DIMENSION);
    spec.Height = std::clamp(settings.windowHeight, Core::WINDOW_MIN_DIMENSION, Core::WINDOW_MAX_DIMENSION);
    spec.VSync = false;
    spec.ForceNativeDecorationsOnWayland = settings.forceNativeWindowDecorationsOnWayland;
    ASSERT_FALSE(settings.windowPosX.has_value() || settings.windowPosY.has_value());
    FirstRunObserved observed;
    try
    {
        Core::Application app(spec);
        app.getWindow().applySavedGeometry(std::nullopt, settings.windowMaximized, settings.windowScale);
        app.pushLayer<UI::UILayer>();
        App::TitleBarLayer* titleBar = app.getWindow().isBorderless() ? &app.pushLayer<App::TitleBarLayer>() : nullptr;
        app.pushLayer<App::ShellLayer>().setTitleBar(titleBar);
        app.pushLayer<App::HelpLayer>();
        app.pushLayer<App::AboutLayer>();
        app.pushLayer<App::SettingsLayer>();
        app.pushLayer<App::ElevationNoticeLayer>();
        app.pushLayer<FirstRunDriverLayer>(observed);
        app.run();
    }
    catch (const std::exception& e)
    {
        FAIL() << "Application failed after the display probe passed: " << e.what();
    }
    EXPECT_FALSE(observed.timedOut) << "Close didn't stop the app";
    ASSERT_TRUE(observed.windowChecked);

    // 1. The window: the default size at this display's scale, wholly inside the display's usable
    // bounds, and not maximized. A display too small for it gets the default fitted to it (#1128).
    EXPECT_FALSE(observed.maximized);
    ASSERT_TRUE(observed.usable.has_value()) << SDL_GetError();
    const SDL_Rect usable = observed.usable.value_or(SDL_Rect{});
    // SDL's offscreen driver (Linux without a display, as in the ASan+UBSan job) can't move a window:
    // it has no SetWindowPosition, so SDL_SetWindowPosition() fails and the window stays where SDL
    // created it, centred at 1280 wide on its 1024-wide display. Only the size is checked there. Where
    // windows can be moved (Windows, X11 under Xvfb), the window must lie wholly inside the usable
    // bounds (#2735).
    if (observed.canMoveWindows)
    {
        EXPECT_GE(observed.x, usable.x);
        EXPECT_GE(observed.y, usable.y);
        EXPECT_LE(observed.x + observed.width, usable.x + usable.w);
        EXPECT_LE(observed.y + observed.height, usable.y + usable.h);
    }
    EXPECT_LE(observed.width, usable.w);
    EXPECT_LE(observed.height, usable.h);
    const auto [defaultWidth, defaultHeight] =
        Core::WindowGeometry::rescaleWindowSize(settings.windowWidth, settings.windowHeight, settings.windowScale, observed.unitScale);
    EXPECT_EQ(observed.width, std::min(defaultWidth, usable.w)) << "at scale " << observed.unitScale;
    EXPECT_EQ(observed.height, std::min(defaultHeight, usable.h)) << "at scale " << observed.unitScale;

    // 2. The shell: the first tab, the machine's, in front (the default tab: tasksmack.md's ShellLayer,
    // CONTRIBUTING.md's "overview"), the title bar and the status bar drawn; then, once pressed, the
    // Processes tab's table at the default columns, order and sort.
    EXPECT_TRUE(observed.systemTabSelected);
    EXPECT_TRUE(observed.titleBarShown); // the custom title bar: native decorations are opt-in (#745)
    EXPECT_TRUE(observed.statusBarShown);
    EXPECT_TRUE(observed.processesTabSelected);
    EXPECT_TRUE(observed.processTableFound);
    for (const std::string& problem : observed.columnProblems)
    {
        ADD_FAILURE() << "Processes table: " << problem;
    }

    // 4. The config saved on Close: the golden file, but for the machine-dependent values
    // (MASKED_CONFIG_KEYS), which are checked against the live window here instead.
    const std::filesystem::path savedPath = configDir / "config.toml";
    const std::optional<std::string> saved = readWholeFile(savedPath);
    ASSERT_TRUE(saved.has_value()) << "ShellLayer::onDetach() saved no " << Core::pathToUtf8(savedPath);
    const std::filesystem::path goldenPath = std::filesystem::path(TASKSMACK_APP_TEST_FIXTURES_DIR) / "config-first-run.toml";
    const std::optional<std::string> golden = readWholeFile(goldenPath);
    ASSERT_TRUE(golden.has_value()) << "can't read " << Core::pathToUtf8(goldenPath);
    const std::string masked = maskMachineDependentKeys(saved.value_or(std::string{}));
    EXPECT_EQ(masked, maskMachineDependentKeys(golden.value_or(std::string{})))
        << "The first-run config.toml is not " << Core::pathToUtf8(goldenPath)
        << ". If a default changed on purpose, replace the golden file with this (masked) text:\n"
        << masked;

    try
    {
        const toml::table document = toml::parse(saved.value_or(std::string{}));
        EXPECT_EQ(document["window"]["width"].value<int>(), observed.width);
        EXPECT_EQ(document["window"]["height"].value<int>(), observed.height);
        const std::optional<double> scale = document["window"]["scale"].value<double>();
        ASSERT_TRUE(scale.has_value());
        EXPECT_FLOAT_EQ(static_cast<float>(scale.value_or(0.0)), observed.unitScale);
        if (Core::Window::supportsPositioning())
        {
            EXPECT_EQ(document["window"]["x"].value<int>(), observed.x);
            EXPECT_EQ(document["window"]["y"].value<int>(), observed.y);
        }
        // The table's widths, order and sort, as ImGui saves them: not empty once the table was drawn.
        EXPECT_FALSE(document["process_table"]["layout"].value_or(std::string{}).empty());
    }
    catch (const toml::parse_error& e)
    {
        FAIL() << "The saved config.toml isn't TOML: " << e.what();
    }
}

} // namespace
