/// @file test_UiLayersOnDisplay.cpp
/// @brief UI::UILayer and App::TitleBarLayer in a real Core::Application (#1395): the ImGui frame
/// UILayer brackets, the title bar TitleBarLayer draws into it, and each title-bar button's effect --
/// Settings, Help and About raise their events, Maximize and Restore change the window's state, and
/// Close stops the app. Also the minimum window size the bar sets from its contents.
///
/// Needs a display with a GL 3.3 core context (Xvfb in Linux CI, where TASKSMACK_REQUIRE_DISPLAY=1
/// turns a missing one into a failure); skipped elsewhere, like the ApplicationTest suite.

#include "App/TitleBarButtons.h"
#include "App/TitleBarLayer.h"
#include "Core/AnimationRequest.h"
#include "Core/Application.h"
#include "Core/ApplicationEvents.h"
#include "Core/Event.h"
#include "Core/HeadlessVideoDriverTestUtils.h"
#include "Core/Layer.h"
#include "Core/Window.h"
#include "UI/IconsFontAwesome6.h"
#include "UI/UILayer.h"

#include <SDL3/SDL_video.h>
#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h> // FindWindowByName(), ImHashStr(), ActivateItemByID(): the bar and its buttons

#include <cstddef>
#include <cstdlib>
#include <exception>
#include <string_view>

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

} // namespace
