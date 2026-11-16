/// @file test_UiLayersOnDisplay.cpp
/// @brief UI::UILayer and App::TitleBarLayer in a real Core::Application (#1395): the title bar's buttons, the UI scale after a display
/// change and a start minimized, the Font size setting, the window icon, window events, ignored events and About. Split from
/// test_UiLayersOnDisplay.cpp by feature (#3850); the shared checks and fixture are in UiLayersOnDisplayFixture.h.

#include "App/AboutDialog.h"
#include "App/AboutLayer.h"
#include "App/ElevationNoticeLayer.h"
#include "App/HelpLayer.h"
#include "App/Panels/ProcessDetailsPanel.h"
#include "App/ProcessColumnConfig.h"
#include "App/SettingsDialog.h"
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
#include "Domain/SamplingConfig.h"
#include "ScratchConfigDir.h"
#include "UI/DpiScale.h"
#include "UI/FontPointSize.h"
#include "UI/IconTextureCache.h"
#include "UI/IconsFontAwesome6.h"
#include "UI/RenderMetrics.h"
#include "UI/Theme.h"
#include "UI/UILayer.h"
#include "UiLayersOnDisplayFixture.h"

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
#include <atomic>
#include <chrono>
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
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#ifdef _WIN32
// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dwmapi.h> // DwmSetWindowAttribute(DWMWA_CLOAK) (#3152)
// clang-format on

#include <cstring> // sendDisplayState()
#include <span>    // sendDisplayState()
#else
#include <unistd.h> // getpid(): the hidden-shell test selects this process (#3156)
#endif

// NOLINTNEXTLINE(google-build-using-namespace) - the shared helpers, as the one file had them
using namespace UiLayersOnDisplayTesting;

namespace
{

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

TEST_F(UiLayersOnDisplayTest, TitleBarButtonsDoWhatTheySay)
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
    // On native Wayland, Maximize asks the compositor, which may decline (sway, a tiling compositor, does).
    if (!onNativeWayland() || compositorMaximizes())
    {
        EXPECT_TRUE(observed.maximizedAfterMaximize);
        EXPECT_FALSE(observed.maximizedAfterRestore);
    }
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

TEST_F(UiLayersOnDisplayTest, ADisplayScaleChangeRebuildsTheFontsAtTheMeasuredScale)
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

/// What FontSizeSettingDriverLayer saw (#3375).
struct FontSizeSettingObserved
{
    int builtBefore = 0;                ///< The size the fonts were first built at
    float emBefore = 0.0F;              ///< ImGui::GetFontSize() then
    std::uint64_t generationBefore = 0; ///< Theme::fontGeneration() then
    int settingAfter = 0;               ///< Theme's Font size setting after Save
    int builtAfter = 0;                 ///< The size the fonts were rebuilt at
    float emAfter = 0.0F;               ///< ImGui::GetFontSize() a frame after the rebuild
    float regularPxAfter = 0.0F;        ///< The body font's own size then
    float chartPxAfter = 0.0F;          ///< The chart font's own size then
    float styleScaleAfter = 0.0F;       ///< Theme::styleScale() then
    float displayScale = 0.0F;          ///< Theme::displayScale() throughout
    std::uint64_t generationAfter = 0;  ///< Theme::fontGeneration() then
    int savedPointSize = 0;             ///< UserConfig's setting after Save
    int frames = 0;
    bool timedOut = false;
};

/// Pushed after UILayer and SettingsLayer: once the frame has settled, opens
/// Settings, picks "12 pt" from the Font size combo and presses Save, as a user
/// does (#3375). UILayer then rebuilds the font atlas at 12 pt at the next
/// frame boundary; the layer records what the frames draw at, before and after,
/// and stops the app.
class FontSizeSettingDriverLayer : public Core::Layer
{
  public:
    explicit FontSizeSettingDriverLayer(FontSizeSettingObserved& observed) : Layer("FontSizeSettingDriver"), m_Observed(observed)
    {}

    void onRender() override
    {
        Core::AnimationRequest::request(Core::AnimationRequest::FULL_RATE); // keep frames coming
        const int frame = ++m_Observed.frames;
        if (frame >= MAX_FRAMES)
        {
            m_Observed.timedOut = true;
            Core::Application::get().stop();
            return;
        }
        if (frame < SETTLE)
        {
            return;
        }
        const UI::Theme& theme = UI::Theme::get();
        const bool settingsOpen = ImGui::IsPopupOpen(App::SettingsDialog::POPUP_ID);
        const ImGuiWindow* modal = ImGui::GetTopMostPopupModal();
        const ImGuiWindow* top = GImGui->OpenPopupStack.empty() ? nullptr : GImGui->OpenPopupStack.back().Window;
        const bool comboOpen = settingsOpen && top != nullptr && top != modal;
        const bool retry = frame % RETRY_FRAMES == 0;
        switch (m_Phase)
        {
        case Phase::Open:
            if (frame == SETTLE)
            {
                m_Observed.builtBefore = theme.fontsBuiltPointSize();
                m_Observed.emBefore = ImGui::GetFontSize();
                m_Observed.generationBefore = theme.fontGeneration();
                m_Observed.displayScale = theme.displayScale();
            }
            if (settingsOpen)
            {
                m_Phase = Phase::OpenCombo;
            }
            else if (retry || frame == SETTLE)
            {
                Core::OpenSettingsEvent event;
                Core::Application::get().raiseEvent(event);
            }
            break;
        case Phase::OpenCombo:
            if (comboOpen)
            {
                m_Phase = Phase::Pick;
            }
            else if (retry && modal != nullptr)
            {
                for (const ImGuiWindow* window : GImGui->Windows)
                {
                    if (window->ParentWindow == modal && std::string_view{window->Name}.contains("##SettingsBody"))
                    {
                        ImGui::ActivateItemByID(ImHashStr("##FontSize", 0, window->ID));
                    }
                }
            }
            break;
        case Phase::Pick:
            if (!comboOpen)
            {
                m_Phase = Phase::Save;
            }
            else if (retry)
            {
                ImGui::ActivateItemByID(ImHashStr(TARGET_LABEL, 0, top->ID));
            }
            break;
        case Phase::Save:
            if (!settingsOpen)
            {
                m_Observed.settingAfter = theme.fontPointSize();
                m_Observed.savedPointSize = App::UserConfig::get().settings().fontPointSize;
                m_Phase = Phase::AwaitRebuild;
            }
            else if (retry && modal != nullptr)
            {
                // Save: a filled button, "##filled" under its label.
                ImGui::ActivateItemByID(ImHashStr("##filled", 0, ImHashStr(App::SettingsDialog::SAVE_LABEL, 0, modal->ID)));
            }
            break;
        case Phase::AwaitRebuild:
            // This frame began after the rebuild when the fonts are at the setting's
            // size: what it draws at is the new size's.
            if (theme.fontsBuiltPointSize() == TARGET_POINTS)
            {
                m_Observed.builtAfter = theme.fontsBuiltPointSize();
                m_Observed.emAfter = ImGui::GetFontSize();
                m_Observed.regularPxAfter = theme.regularFont() != nullptr ? theme.regularFont()->LegacySize : 0.0F;
                m_Observed.chartPxAfter = theme.chartFont() != nullptr ? theme.chartFont()->LegacySize : 0.0F;
                m_Observed.styleScaleAfter = theme.styleScale();
                m_Observed.generationAfter = theme.fontGeneration();
                Core::Application::get().stop();
            }
            break;
        }
    }

    static constexpr int TARGET_POINTS = 12;
    static constexpr const char* TARGET_LABEL = "12 pt";

  private:
    enum class Phase : std::uint8_t
    {
        Open,
        OpenCombo,
        Pick,
        Save,
        AwaitRebuild,
    };

    static constexpr int SETTLE = 10;
    static constexpr int RETRY_FRAMES = 10; // a press that didn't take is made again
    static constexpr int MAX_FRAMES = 600;

    FontSizeSettingObserved& m_Observed;
    Phase m_Phase = Phase::Open;
};

// #3375: picking a point size in Settings and pressing Save rebuilds the fonts
// at that size, at a frame boundary, in a real UILayer: the text, the chart
// text and the chrome around it all grow, and the size is saved.
TEST_F(UiLayersOnDisplayTest, PickingAFontSizeInSettingsRebuildsTheFontsAtIt)
{
    if (!hasDisplay())
    {
        GTEST_SKIP() << "No display available (headless environment)";
    }

    UI::Theme& theme = UI::Theme::get();
    const int originalSize = theme.fontPointSize();
    theme.setFontPointSize(UI::FONT_POINT_SIZE_DEFAULT); // the scratch config's default, as a first
                                                         // run starts
    Core::ApplicationSpecification spec;
    spec.Name = "UiLayersOnDisplayFontSizeTest";
    spec.VSync = false;
    FontSizeSettingObserved observed;
    try
    {
        Core::Application app(spec);
        app.pushLayer<UI::UILayer>();
        app.pushLayer<App::SettingsLayer>();
        app.pushLayer<FontSizeSettingDriverLayer>(observed);
        app.run();
    }
    catch (const std::exception& e)
    {
        theme.setFontPointSize(originalSize);
        FAIL() << "Application failed after the display probe passed: " << e.what();
    }
    App::UserConfig::get().flushPendingSave(); // Save is written by the settings worker (#2916)
    theme.setFontPointSize(originalSize);

    constexpr int TARGET = FontSizeSettingDriverLayer::TARGET_POINTS;
    EXPECT_FALSE(observed.timedOut);
    EXPECT_EQ(observed.builtBefore, UI::FONT_POINT_SIZE_DEFAULT);
    EXPECT_EQ(observed.settingAfter, TARGET);
    EXPECT_EQ(observed.savedPointSize, TARGET);
    EXPECT_EQ(observed.builtAfter, TARGET);
    EXPECT_GT(observed.generationAfter, observed.generationBefore) << "the fonts weren't rebuilt";
    ASSERT_GT(observed.displayScale, 0.0F);
    // ImGui rounds a font's size to a whole pixel.
    EXPECT_NEAR(observed.emBefore, UI::computePointsToPixels(static_cast<float>(UI::FONT_POINT_SIZE_DEFAULT), observed.displayScale), 1.0F);
    EXPECT_NEAR(observed.regularPxAfter, UI::computePointsToPixels(static_cast<float>(TARGET), observed.displayScale), 0.01F);
    EXPECT_NEAR(observed.emAfter, observed.regularPxAfter, 1.0F);
    EXPECT_NEAR(
        observed.chartPxAfter, UI::computePointsToPixels(static_cast<float>(UI::chartFontPointSize(TARGET)), observed.displayScale), 0.01F);
    EXPECT_FLOAT_EQ(observed.styleScaleAfter, (static_cast<float>(TARGET) / 8.0F) * observed.displayScale);

    // Saved as a point size, with no named size beside it.
    const std::filesystem::path savedPath = App::UserConfig::get().configPath();
    toml::table saved;
    try
    {
        saved = toml::parse_file(Core::pathToUtf8(savedPath));
    }
    catch (const toml::parse_error& e)
    {
        FAIL() << "can't read " << Core::pathToUtf8(savedPath) << ": " << e.what();
    }
    EXPECT_EQ(saved["font"]["point_size"].value<std::int64_t>(), std::optional<std::int64_t>{TARGET});
    EXPECT_FALSE(saved["font"]["size"]);
}

/// What MinimizedStartDriverLayer saw (#3150).
struct MinimizedStartObserved
{
    bool minimizedAtFirstFrame = false;
    float scaleWhileMinimized = 0.0F; ///< Theme's scale after a display change arrived while minimized
    float afterRestore = 0.0F;        ///< Theme's scale a few frames after the restore
    float restoredWindowScale = 0.0F; ///< What the restored window measures (Window::getUnitScale())
    int frames = 0;
    bool timedOut = false;
};

/// Pushed after UILayer, in an app whose window was minimized before UILayer attached: queues the
/// SDL_EVENT_WINDOW_DISPLAY_CHANGED SDL sends at startup while the window is still minimized, which
/// measured the minimized window's pixel density and rebuilt the fonts at about 5x (#3150), then
/// restores the window and records the scale UILayer settles on. The loop draws the first frame of
/// a window started minimized, but none after it until the restore (#3156), so the restore is made
/// when the display change reaches the layers, after UILayer has noted it, not on a later frame.
class MinimizedStartDriverLayer : public Core::Layer
{
  public:
    explicit MinimizedStartDriverLayer(MinimizedStartObserved& observed) : Layer("MinimizedStartDriver"), m_Observed(observed)
    {}

    void onSDLEvent(SDL_Event* event) override
    {
        if (m_PushedDisplayChange && !m_RestoreRequested && event->type == SDL_EVENT_WINDOW_DISPLAY_CHANGED)
        {
            m_Observed.scaleWhileMinimized = UI::Theme::get().displayScale();
            Core::Application::get().getWindow().restore();
            m_RestoreRequested = true;
        }
    }

    void onRender() override
    {
        Core::AnimationRequest::request(Core::AnimationRequest::FULL_RATE); // keep frames coming
        const int frame = ++m_Observed.frames;
        const Core::Window& window = Core::Application::get().getWindow();
        if (frame == 1)
        {
            m_Observed.minimizedAtFirstFrame = window.isMinimized();
            SDL_Event event{};
            event.type = SDL_EVENT_WINDOW_DISPLAY_CHANGED;
            event.window.windowID = SDL_GetWindowID(window.getHandle());
            SDL_PushEvent(&event);
            m_PushedDisplayChange = true;
        }
        else if (m_RestoreRequested && m_RestoreFrame == 0)
        {
            m_RestoreFrame = frame;
        }
        else if (m_RestoreFrame != 0 && !window.isMinimized())
        {
            // Waits on the state, not a frame count (#2901): the restore event, and the measurement it
            // asks for, can land a few frames later under load.
            m_Observed.afterRestore = UI::Theme::get().displayScale();
            m_Observed.restoredWindowScale = window.getUnitScale();
            if (frame >= m_RestoreFrame + STEP && m_Observed.restoredWindowScale > 0.0F &&
                std::abs(m_Observed.afterRestore - m_Observed.restoredWindowScale) < UI::DISPLAY_SCALE_EPSILON)
            {
                Core::Application::get().stop();
            }
        }
        if (frame >= MAX_FRAMES)
        {
            m_Observed.timedOut = true;
            Core::Application::get().stop();
        }
    }

  private:
    static constexpr int STEP = 3;         // frames after the restore for the rebuild to land
    static constexpr int MAX_FRAMES = 300; // the restore and the rebuild normally land within a few frames

    MinimizedStartObserved& m_Observed;
    int m_RestoreFrame = 0; // the first frame after the restore was asked for
    bool m_PushedDisplayChange = false;
    bool m_RestoreRequested = false;
};

TEST_F(UiLayersOnDisplayTest, AWindowMinimizedBeforeItsFirstFrameGetsTheDisplayScaleBackOnRestore)
{
    if (!hasDisplay())
    {
        GTEST_SKIP() << "No display available (headless environment)";
    }
    if (onNativeWayland())
    {
        GTEST_SKIP() << "native Wayland: a client can't bring its own window back from minimized";
    }

    Core::ApplicationSpecification spec;
    spec.Name = "UiLayersOnDisplayMinimizedStartTest";
    spec.VSync = false;
    MinimizedStartObserved observed;
    try
    {
        Core::Application app(spec);
        // Minimized before UILayer measures anything, as "Run: Minimized" or ShowWindow(SW_SHOWMINNOACTIVE)
        // from a launcher does (#3150).
        app.getWindow().minimize();
        SDL_SyncWindow(app.getWindow().getHandle());
        app.pushLayer<UI::UILayer>();
        app.pushLayer<MinimizedStartDriverLayer>(observed);
        app.run();
    }
    catch (const std::exception& e)
    {
        FAIL() << "Application failed after the display probe passed: " << e.what();
    }

    if (!observed.minimizedAtFirstFrame)
    {
        GTEST_SKIP() << "The window didn't minimize on this display (no window manager), so there was nothing to check";
    }
    ASSERT_GT(observed.restoredWindowScale, 0.0F) << "the window never came back from minimized";
    // The scale the restored window measures, not one taken from the minimized window (8.88 at 175 %).
    EXPECT_FLOAT_EQ(observed.afterRestore, observed.restoredWindowScale)
        << "while minimized the UI scale was " << observed.scaleWhileMinimized;
    EXPECT_FALSE(observed.timedOut);
#ifdef _WIN32
    // A real window's pixel density is 1 on Windows, so the minimized window's one (about 0.2) is
    // ignored and the fonts were never built at 5x, even before the restore.
    EXPECT_FLOAT_EQ(observed.scaleWhileMinimized, observed.restoredWindowScale);
#endif
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

TEST_F(UiLayersOnDisplayTest, TheOpenGLIconBackendMakesAndDeletesATexture)
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

TEST_F(UiLayersOnDisplayTest, TitleBarMouseAndKeyEventsDoWhatTheySay)
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
    // On native Wayland the title bar's and the edges' presses are the compositor's (DRAGGABLE, RESIZE_*):
    // its double-click and its click are its own (#744). The keyboard's window menu is still TaskSmack's.
    if (!onNativeWayland())
    {
        EXPECT_TRUE(observed.maximizedAfterDoubleClick);        // double-click on the empty title bar maximizes
        EXPECT_FALSE(observed.maximizedAfterSecondDoubleClick); // and again restores
        EXPECT_TRUE(observed.positionKeptAfterClick);           // a press and release without motion moves nothing
        EXPECT_TRUE(observed.sizeKeptAfterEdgePress);           // likewise on the resize edge
    }
    EXPECT_TRUE(observed.systemMenuOpened); // Ctrl+Space opens the window menu
}

/// What the ignored-events driver saw (#3221).
struct IgnoredEventsObserved
{
    bool maximizedAfterIgnoredDoubleClicks = true;
    bool positionKeptAfterOtherWindowsPress = false;
    bool systemMenuOpenedByAltSpace = false;
    int frames = 0;
    bool timedOut = false;
};

/// Pushed after UILayer and TitleBarLayer: pushes the title bar the events it must leave alone -- a
/// double-click for another window, one below the bar and one on the window's top edge, and a press for
/// another window -- then Alt+Space, which opens the window menu (#3221).
class IgnoredEventsDriverLayer : public Core::Layer
{
  public:
    explicit IgnoredEventsDriverLayer(IgnoredEventsObserved& observed) : Layer("IgnoredEventsDriver"), m_Observed(observed)
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
        const SDL_WindowID thisWindow = SDL_GetWindowID(window.getHandle());
        const float middleX = static_cast<float>(width) * 0.5F; // clear of the buttons and the icon
        constexpr float TITLE_Y = 16.0F;
        constexpr float BELOW_BAR_Y = 120.0F; // the content, well below the title bar
        constexpr float TOP_EDGE_Y = 1.0F;    // the top resize border, inside the bar's height
        switch (m_Phase)
        {
        case Phase::DoubleClicks:
            for (const auto& [windowId, y] :
                 {std::pair{thisWindow + 1000, TITLE_Y}, std::pair{thisWindow, BELOW_BAR_Y}, std::pair{thisWindow, TOP_EDGE_Y}})
            {
                pushButton(windowId, middleX, y, true, 2);
                pushButton(windowId, middleX, y, false, 2);
            }
            m_Phase = Phase::OtherWindowsPress;
            m_NextFrame = frame + STEP;
            break;
        case Phase::OtherWindowsPress:
            m_Observed.maximizedAfterIgnoredDoubleClicks = window.isMaximized();
            m_Position = window.getPosition();
            pushButton(thisWindow + 1000, middleX, TITLE_Y, true, 1); // no drag begins for another window's press
            m_Phase = Phase::Release;
            m_NextFrame = frame + 2;
            break;
        case Phase::Release:
            pushButton(thisWindow + 1000, middleX, TITLE_Y, false, 1);
            m_Phase = Phase::AltSpace;
            m_NextFrame = frame + STEP;
            break;
        case Phase::AltSpace:
            m_Observed.positionKeptAfterOtherWindowsPress = window.getPosition() == m_Position;
            pushKey(thisWindow, SDLK_SPACE, SDL_SCANCODE_SPACE, SDL_KMOD_LALT); // Alt+Space: the window menu
            m_Phase = Phase::CheckMenu;
            m_NextFrame = frame + STEP;
            break;
        case Phase::CheckMenu:
            m_Observed.systemMenuOpenedByAltSpace = !GImGui->OpenPopupStack.empty();
            Core::Application::get().stop();
            break;
        }
    }

  private:
    static void pushButton(SDL_WindowID windowId, float x, float y, bool down, int clicks)
    {
        SDL_Event event{};
        event.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
        event.button.windowID = windowId;
        event.button.button = SDL_BUTTON_LEFT;
        event.button.down = down;
        event.button.clicks = static_cast<Uint8>(clicks);
        event.button.x = x;
        event.button.y = y;
        static_cast<void>(SDL_PushEvent(&event));
    }

    static void pushKey(SDL_WindowID windowId, SDL_Keycode key, SDL_Scancode scancode, SDL_Keymod mod)
    {
        SDL_Event event{};
        event.type = SDL_EVENT_KEY_DOWN;
        event.key.windowID = windowId;
        event.key.key = key;
        event.key.scancode = scancode;
        event.key.mod = mod;
        event.key.down = true;
        static_cast<void>(SDL_PushEvent(&event));
    }

    enum class Phase : std::uint8_t
    {
        DoubleClicks,
        OtherWindowsPress,
        Release,
        AltSpace,
        CheckMenu,
    };

    static constexpr int SETTLE = 10;
    static constexpr int STEP = 10;
    static constexpr int MAX_FRAMES = 600;

    IgnoredEventsObserved& m_Observed;
    Phase m_Phase = Phase::DoubleClicks;
    int m_NextFrame = SETTLE;
    std::pair<int, int> m_Position;
};

// #3221: the title bar acts only on its own window's events and only over its own empty area: a
// double-click for another window, below the bar or on the top resize edge doesn't maximize, and a press
// for another window starts no drag. Alt+Space opens the window menu, as Ctrl+Space does.
TEST_F(UiLayersOnDisplayTest, TitleBarIgnoresEventsThatArentForItsEmptyArea)
{
    if (!hasDisplay())
    {
        GTEST_SKIP() << "No display available (headless environment)";
    }

    Core::ApplicationSpecification spec;
    spec.Name = "UiLayersOnDisplayIgnoredEventsTest";
    spec.VSync = false;
    IgnoredEventsObserved observed;
    try
    {
        Core::Application app(spec);
        app.pushLayer<UI::UILayer>();
        app.pushLayer<App::TitleBarLayer>();
        app.pushLayer<IgnoredEventsDriverLayer>(observed);
        app.run();
    }
    catch (const std::exception& e)
    {
        FAIL() << "Application failed after the display probe passed: " << e.what();
    }

    EXPECT_FALSE(observed.timedOut);
    EXPECT_FALSE(observed.maximizedAfterIgnoredDoubleClicks);
    EXPECT_TRUE(observed.positionKeptAfterOtherWindowsPress);
    EXPECT_TRUE(observed.systemMenuOpenedByAltSpace);
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

TEST_F(UiLayersOnDisplayTest, AboutOpensWithItsIconAndTitleFont)
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

} // namespace
