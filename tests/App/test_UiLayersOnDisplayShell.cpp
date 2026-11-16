/// @file test_UiLayersOnDisplayShell.cpp
/// @brief UI::UILayer and App::TitleBarLayer in a real Core::Application (#1395): the hidden states Windows reports only to the window
/// (#3152), the UI layer, the shell's keys, a hidden shell, the tabs and the first run. Split from test_UiLayersOnDisplay.cpp by feature
/// (#3850); the shared checks and fixture are in UiLayersOnDisplayFixture.h.

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

#ifdef _WIN32
/// What the visibility driver saw (#3152, #3156).
struct VisibilityObserved
{
    int frames = 0;
    int framesWhileHidden = 0;       ///< Frames drawn while Window::isHidden() said hidden: none (#3156)
    int hiddenUpdates = 0;           ///< Layer::onHiddenUpdate() calls, the loop's wakes without a frame
    bool shownAtStart = false;       ///< Not hidden once the display is reported on
    bool hiddenWhenLocked = false;   ///< WM_WTSSESSION_CHANGE WTS_SESSION_LOCK hid it before SendMessage returned...
    bool stillHiddenLater = false;   ///< ...and it stayed hidden through the wait that followed
    bool shownWhenUnlocked = false;  ///< WTS_SESSION_UNLOCK showed it again...
    bool exposedAfterUnlock = false; ///< ...with an SDL_EVENT_WINDOW_EXPOSED to wake the hidden wait...
    Uint64 unlockToFrameMs = 0;      ///< ...and the next frame came this soon after the unlock was posted
    bool hiddenWhenDisplayOff = false;
    bool shownWhenDisplayOn = false;
    bool cloakUnavailable = false; ///< DwmSetWindowAttribute(DWMWA_CLOAK) failed: the cloak checks were skipped
    bool hiddenWhenCloaked = false;
    bool shownSoonAfterUncloak = false; ///< Within UNCLOAK_FOUND_WITHIN_MS, under one re-check interval
    bool timedOut = false;
};

/// GUID_SESSION_DISPLAY_STATUS (<winnt.h>), spelled out as Window.cpp does.
constexpr GUID TEST_SESSION_DISPLAY_STATUS{
    .Data1 = 0x2b84c20e, .Data2 = 0xad23, .Data3 = 0x4ddf, .Data4 = {0x93, 0xdb, 0x05, 0xff, 0xbd, 0x7e, 0xfc, 0xa5}};

/// Send @p hwnd the power-setting notification Windows sends when the session's display turns off
/// (PowerMonitorOff) or on (PowerMonitorOn): WM_POWERBROADCAST PBT_POWERSETTINGCHANGE, to this window alone.
void sendDisplayState(HWND hwnd, DWORD state)
{
    // POWERBROADCAST_SETTING ends in UCHAR Data[1], which Windows extends to DataLength bytes: lay
    // the header and the DWORD state out in a buffer big enough for both.
    constexpr std::size_t DATA_OFFSET = offsetof(POWERBROADCAST_SETTING, Data);
    alignas(POWERBROADCAST_SETTING) std::array<std::byte, std::max(sizeof(POWERBROADCAST_SETTING), DATA_OFFSET + sizeof(DWORD))> buffer{};
    POWERBROADCAST_SETTING header{};
    header.PowerSetting = TEST_SESSION_DISPLAY_STATUS;
    header.DataLength = sizeof(DWORD);
    std::memcpy(buffer.data(), &header, DATA_OFFSET);
    std::memcpy(std::span(buffer).subspan(DATA_OFFSET).data(), &state, sizeof(state));
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast,performance-no-ptr-to-int) - PBT_POWERSETTINGCHANGE's lParam is a POWERBROADCAST_SETTING*
    SendMessageW(hwnd, WM_POWERBROADCAST, PBT_POWERSETTINGCHANGE, reinterpret_cast<LPARAM>(buffer.data()));
}

/// The window's HWND, or nullptr.
[[nodiscard]] HWND hwndOf(const Core::Window& window)
{
    return static_cast<HWND>(
        SDL_GetPointerProperty(SDL_GetWindowProperties(window.getHandle()), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
}

/// Pushed after UILayer and TitleBarLayer: tells the real window what Windows tells it when the
/// session locks and unlocks and when the display turns off and on -- synthetic messages to this
/// window alone, through its subclass procedure, so nothing is locked or turned off -- then cloaks
/// and uncloaks the window itself (DWMWA_CLOAK, as another virtual desktop does), and records when
/// Window::isHidden() said the window was hidden. A hidden window draws no frames (#3156), so each
/// hidden step runs from onHiddenUpdate(), on the loop's wake, and the frame after it checks that
/// the window was shown again. The unlock is posted from another thread while the loop waits, as
/// Windows posts it, so the next frame proves its expose woke the wait rather than the wait's timeout.
class VisibilityDriverLayer : public Core::Layer
{
  public:
    explicit VisibilityDriverLayer(VisibilityObserved& observed) : Layer("VisibilityDriver"), m_Observed(observed)
    {}

    ~VisibilityDriverLayer() override
    {
        joinPoster();
    }

    VisibilityDriverLayer(const VisibilityDriverLayer&) = delete;
    VisibilityDriverLayer& operator=(const VisibilityDriverLayer&) = delete;
    VisibilityDriverLayer(VisibilityDriverLayer&&) = delete;
    VisibilityDriverLayer& operator=(VisibilityDriverLayer&&) = delete;

    void onDetach() override
    {
        joinPoster();
    }

    void onSDLEvent(SDL_Event* event) override
    {
        if (m_AwaitingExpose && event->type == SDL_EVENT_WINDOW_EXPOSED)
        {
            m_Observed.exposedAfterUnlock = true;
        }
    }

    void onRender() override
    {
        Core::AnimationRequest::request(Core::AnimationRequest::FULL_RATE); // keep frames coming
        const int frame = ++m_Observed.frames;
        const Core::Window& window = Core::Application::get().getWindow();
        if (window.isHidden())
        {
            ++m_Observed.framesWhileHidden;
        }
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
        auto* const hwnd = hwndOf(window);
        if (hwnd == nullptr)
        {
            ADD_FAILURE() << "the window has no HWND";
            Core::Application::get().stop();
            return;
        }
        const auto next = [this, frame](Phase phase, int frames)
        {
            m_Phase = phase;
            m_NextFrame = frame + frames;
        };
        switch (m_Phase)
        {
        case Phase::Lock:
            // Whatever the runner's own display reports at startup, start from a display that is on.
            sendDisplayState(hwnd, PowerMonitorOn);
            m_Observed.shownAtStart = !window.isHidden();
            SendMessageW(hwnd, WM_WTSSESSION_CHANGE, WTS_SESSION_LOCK, 0);
            m_Observed.hiddenWhenLocked = window.isHidden();
            m_Phase = Phase::Locked; // onHiddenUpdate() takes it from here
            break;
        case Phase::AwaitUnlock:
            // The first frame since the unlock was posted: the wake it caused drew it.
            m_Observed.shownWhenUnlocked = !window.isHidden();
            m_Observed.unlockToFrameMs = SDL_GetTicks() - m_UnlockPostedAt.load();
            next(Phase::DisplayOff, STEP);
            break;
        case Phase::DisplayOff:
            m_AwaitingExpose = false;
            sendDisplayState(hwnd, PowerMonitorOff);
            m_Observed.hiddenWhenDisplayOff = window.isHidden();
            m_Phase = Phase::DisplayIsOff; // onHiddenUpdate() takes it from here
            break;
        case Phase::AfterDisplayOn:
            m_Observed.shownWhenDisplayOn = !window.isHidden();
            next(Phase::Cloak, STEP);
            break;
        case Phase::Cloak:
        {
            const BOOL cloak = TRUE;
            if (FAILED(DwmSetWindowAttribute(hwnd, DWMWA_CLOAK, &cloak, sizeof(cloak))))
            {
                m_Observed.cloakUnavailable = true;
                next(Phase::Stop, 1);
                break;
            }
            m_CloakedAt = SDL_GetTicks();
            next(Phase::AwaitCloaked, 1);
            break;
        }
        case Phase::AwaitCloaked:
            // No message says the window was cloaked: frames go on until the loop's slow re-check finds
            // it, and onHiddenUpdate() takes it from there. One that never does is uncloaked here.
            if (SDL_GetTicks() - m_CloakedAt > CLOAK_FOUND_WITHIN_MS)
            {
                uncloak(hwnd);
                next(Phase::Stop, STEP);
            }
            else
            {
                next(Phase::AwaitCloaked, 1);
            }
            break;
        case Phase::AfterUncloak:
            // The loop re-checks a cloaked window on every wake, not once per interval, so it finds the
            // window shown again well within one.
            m_Observed.shownSoonAfterUncloak = !window.isHidden() && SDL_GetTicks() - m_UncloakedAt <= UNCLOAK_FOUND_WITHIN_MS;
            next(Phase::Stop, STEP);
            break;
        case Phase::Stop:
            Core::Application::get().stop();
            break;
        case Phase::Locked:
        case Phase::DisplayIsOff:
            break; // hidden: no frame should come
        }
    }

    void onHiddenUpdate() override
    {
        if (++m_Observed.hiddenUpdates > MAX_HIDDEN_UPDATES)
        {
            m_Observed.timedOut = true;
            Core::Application::get().stop();
            return;
        }
        const Core::Window& window = Core::Application::get().getWindow();
        auto* const hwnd = hwndOf(window);
        if (hwnd == nullptr)
        {
            return; // onRender() reports it
        }
        switch (m_Phase)
        {
        case Phase::Locked:
            // A whole hidden wait has passed: still hidden, so no frame came.
            m_Observed.stillHiddenLater = window.isHidden();
            m_AwaitingExpose = true;
            m_Poster = std::thread(
                [this, hwnd]
                {
                    std::this_thread::sleep_for(POST_DELAY);
                    m_UnlockPostedAt.store(SDL_GetTicks());
                    PostMessageW(hwnd, WM_WTSSESSION_CHANGE, WTS_SESSION_UNLOCK, 0);
                });
            m_Phase = Phase::AwaitUnlock;
            m_NextFrame = 0;
            break;
        case Phase::DisplayIsOff:
            sendDisplayState(hwnd, PowerMonitorOn);
            m_Phase = Phase::AfterDisplayOn;
            m_NextFrame = 0;
            break;
        case Phase::AwaitCloaked:
            // The loop found the cloak and stopped drawing.
            m_Observed.hiddenWhenCloaked = window.isHidden();
            uncloak(hwnd);
            m_Phase = Phase::AfterUncloak;
            m_NextFrame = 0;
            break;
        case Phase::Lock:
        case Phase::AwaitUnlock: // a wake before the unlock arrived: it is still to come
        case Phase::DisplayOff:
        case Phase::AfterDisplayOn:
        case Phase::Cloak:
        case Phase::AfterUncloak:
        case Phase::Stop:
            break;
        }
    }

  private:
    /// The steps, in order. A frame runs a shown step once m_NextFrame comes; onHiddenUpdate() runs
    /// a hidden one (Locked, DisplayIsOff, and AwaitCloaked once the cloak is found). AwaitUnlock's
    /// frame is the first after the posted unlock.
    enum class Phase : std::uint8_t
    {
        Lock,
        Locked,
        AwaitUnlock,
        DisplayOff,
        DisplayIsOff,
        AfterDisplayOn,
        Cloak,
        AwaitCloaked,
        AfterUncloak,
        Stop,
    };

    void uncloak(HWND hwnd)
    {
        const BOOL cloaked = FALSE;
        static_cast<void>(DwmSetWindowAttribute(hwnd, DWMWA_CLOAK, &cloaked, sizeof(cloaked)));
        m_UncloakedAt = SDL_GetTicks();
    }

    void joinPoster()
    {
        if (m_Poster.joinable())
        {
            m_Poster.join();
        }
    }

    static constexpr int SETTLE = 10; // frames before the first step: fonts, layout and minimum size settle
    static constexpr int STEP = 10;   // frames between steps on the shown window
    static constexpr int MAX_FRAMES = 600;
    static constexpr int MAX_HIDDEN_UPDATES = 30; // about 30 s of hidden waits
    // How long after the loop's wake the unlock is posted: well inside the next hidden wait.
    static constexpr std::chrono::milliseconds POST_DELAY{100};
    // Twice WINDOW_CLOAK_RECHECK_INTERVAL, plus a frame or two.
    static constexpr Uint64 CLOAK_FOUND_WITHIN_MS = (2 * static_cast<Uint64>(Core::WINDOW_CLOAK_RECHECK_INTERVAL.count())) + 500U;
    // Under one WINDOW_CLOAK_RECHECK_INTERVAL: a few cloaked polls.
    static constexpr Uint64 UNCLOAK_FOUND_WITHIN_MS = (static_cast<Uint64>(Core::WINDOW_CLOAK_RECHECK_INTERVAL.count()) * 3U) / 4U;

    VisibilityObserved& m_Observed;
    std::thread m_Poster; // posts the unlock while the loop waits
    std::atomic<Uint64> m_UnlockPostedAt{0};
    Uint64 m_CloakedAt = 0;   // SDL_GetTicks() when the window was cloaked...
    Uint64 m_UncloakedAt = 0; // ...and uncloaked
    int m_NextFrame = SETTLE;
    Phase m_Phase = Phase::Lock;
    bool m_AwaitingExpose = false; // the unlock was sent: count the expose it pushes
};

// #3152: SDL 3.4 reports no hidden state but minimized on Windows, so the window's subclass procedure
// reads the session's lock and unlock, its display turning off and on, and DWM's cloak, and
// Window::isHidden() -- what Application::run() paces drawing by -- reports each, then shows the
// window again at once when it ends, waking the hidden wait with an expose. While any of them hides
// the window, the loop draws no frames at all (#3156).
TEST_F(UiLayersOnDisplayTest, ALockedSessionAnOffDisplayAndACloakHideTheWindowUntilTheyEnd)
{
    if (!hasDisplay())
    {
        GTEST_SKIP() << "No display available (headless environment)";
    }

    Core::ApplicationSpecification spec;
    spec.Name = "UiLayersOnDisplayVisibilityTest";
    spec.VSync = false;
    VisibilityObserved observed;
    try
    {
        Core::Application app(spec);
        app.pushLayer<UI::UILayer>();
        app.pushLayer<App::TitleBarLayer>();
        app.pushLayer<VisibilityDriverLayer>(observed);
        app.run();
    }
    catch (const std::exception& e)
    {
        FAIL() << "Application failed after the display probe passed: " << e.what();
    }

    EXPECT_FALSE(observed.timedOut);
    EXPECT_EQ(observed.framesWhileHidden, 0) << "the hidden window still drew frames";
    EXPECT_GT(observed.hiddenUpdates, 0);
    EXPECT_TRUE(observed.shownAtStart);
    EXPECT_TRUE(observed.hiddenWhenLocked) << "WTS_SESSION_LOCK didn't hide the window";
    EXPECT_TRUE(observed.stillHiddenLater) << "the locked window was shown again before the unlock";
    EXPECT_TRUE(observed.shownWhenUnlocked) << "WTS_SESSION_UNLOCK left the window hidden";
    EXPECT_TRUE(observed.exposedAfterUnlock) << "the unlock pushed no SDL_EVENT_WINDOW_EXPOSED to wake the frame loop";
    // Well under the hidden wait's timeout: the expose woke the wait.
    EXPECT_LT(observed.unlockToFrameMs, static_cast<Uint64>(Core::WINDOW_HIDDEN_WAIT_TIMEOUT.count()) / 2U)
        << "the first frame after the unlock waited out the hidden wait";
    EXPECT_TRUE(observed.hiddenWhenDisplayOff) << "PowerMonitorOff didn't hide the window";
    EXPECT_TRUE(observed.shownWhenDisplayOn) << "PowerMonitorOn left the window hidden";
    if (observed.cloakUnavailable)
    {
        GTEST_SKIP() << "DWM can't cloak the window here, so the cloak checks were skipped";
    }
    EXPECT_TRUE(observed.hiddenWhenCloaked) << "the frame loop never found the cloaked window hidden";
    EXPECT_TRUE(observed.shownSoonAfterUncloak) << "the uncloaked window stayed hidden";
}
#endif
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

TEST_F(UiLayersOnDisplayTest, UiLayerLoadsUserThemesAndCountsDrawCalls)
{
    if (!hasDisplay())
    {
        GTEST_SKIP() << "No display available (headless environment)";
    }
    // A user theme beside config.toml (this test's own config directory): merged over the built-ins by
    // its file name (#1127).
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
                m_FontBefore = UI::Theme::get().fontPointSize(); // not again on a retry
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
            // A point larger, and the fonts rebuilt at it by UILayer at a frame boundary (#3375).
            m_Observed.fontGrew = UI::Theme::get().fontPointSize() != m_FontBefore && !UI::Theme::get().fontsNeedRebuild() &&
                                  UI::Theme::get().fontsBuiltPointSize() == UI::Theme::get().fontPointSize();
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
            m_Observed.fontShrankBack =
                UI::Theme::get().fontPointSize() == m_FontBefore && UI::Theme::get().fontsBuiltPointSize() == m_FontBefore;
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
    int m_FontBefore = UI::FONT_POINT_SIZE_DEFAULT;
};

TEST_F(UiLayersOnDisplayTest, TheShellsShortcutsAndTheDetailsRequestDoWhatTheySay)
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
    EXPECT_TRUE(observed.fontGrew);                     // Ctrl+= one point larger, fonts rebuilt (#3375)
    EXPECT_TRUE(observed.fontShrankBack);               // Ctrl+- back
}

/// What HiddenShellDriverLayer saw (#3156).
struct HiddenShellObserved
{
    int frames = 0;
    int framesWhileHidden = 0;           ///< Frames drawn while the window reported hidden: none
    int hiddenUpdates = 0;               ///< Layer::onHiddenUpdate() calls, the loop's wakes without a frame
    std::uint64_t versionWhenHidden = 0; ///< Process Details' lastSampleVersion() at the first hidden wake...
    std::uint64_t versionWhenShown = 0;  ///< ...and when the show was sent, with no frame in between
    Uint64 showToFrameMs = 0;            ///< From the show, sent while the loop waited, to the next frame
    bool watching = false;               ///< Process Details had taken a sample before the hide
    bool hideUnavailable = false;        ///< The window never reported hidden: nothing to check
    bool hiddenAtFirstWake = false;
    bool drewAfterShow = false;
    bool timedOut = false;
};

/// This process's PID, for Process Details to watch.
[[nodiscard]] std::int32_t ownPid()
{
#ifdef _WIN32
    return static_cast<std::int32_t>(GetCurrentProcessId());
#else
    return static_cast<std::int32_t>(getpid());
#endif
}

/// Pushed last over the whole shell: selects this process in Process Details at the fastest refresh
/// interval, hides the window -- minimized on Windows; elsewhere through Window::setHiddenForTesting(),
/// since Xvfb has no window manager to minimize it -- and records that no frame is drawn while it is
/// hidden, that Process Details still takes the selected process's samples on the loop's wakes, and
/// that a show sent from another thread while the loop waits brings the next frame at once: the
/// taskbar's SC_RESTORE on Windows, an SDL user event elsewhere, on which this layer clears the seam.
class HiddenShellDriverLayer : public Core::Layer
{
  public:
    HiddenShellDriverLayer(HiddenShellObserved& observed, const App::ShellLayer& shell)
        : Layer("HiddenShellDriver"), m_Observed(observed), m_Shell(shell)
    {}

    ~HiddenShellDriverLayer() override
    {
        joinShower();
    }

    HiddenShellDriverLayer(const HiddenShellDriverLayer&) = delete;
    HiddenShellDriverLayer& operator=(const HiddenShellDriverLayer&) = delete;
    HiddenShellDriverLayer(HiddenShellDriverLayer&&) = delete;
    HiddenShellDriverLayer& operator=(HiddenShellDriverLayer&&) = delete;

    void onDetach() override
    {
        joinShower();
    }

    void onSDLEvent(SDL_Event* event) override
    {
        if (event->type == SDL_EVENT_USER && m_Phase == Phase::AwaitShow)
        {
            Core::Application::get().getWindow().setHiddenForTesting(false);
        }
    }

    void onRender() override
    {
        Core::AnimationRequest::request(Core::AnimationRequest::FULL_RATE); // keep frames coming
        const int frame = ++m_Observed.frames;
        Core::Window& window = Core::Application::get().getWindow();
        if (frame > MAX_FRAMES)
        {
            m_Observed.timedOut = true;
            Core::Application::get().stop();
            return;
        }
        switch (m_Phase)
        {
        case Phase::Settle:
            if (frame >= SETTLE)
            {
                Core::RefreshRateChangedEvent fastest(Domain::Sampling::REFRESH_INTERVAL_MIN_MS);
                Core::Application::get().raiseEvent(fastest);
                Core::ProcessSelectedEvent selected(ownPid(), 0);
                Core::Application::get().raiseEvent(selected);
                m_Phase = Phase::AwaitWatching;
                m_Deadline = frame + WATCH_WAIT_FRAMES;
            }
            break;
        case Phase::AwaitWatching:
            // The pane takes every generation published since the selection, the process listed or not.
            m_Observed.watching = m_Shell.processDetailsPanelForTesting().lastSampleVersion() > 0;
            if (m_Observed.watching || frame >= m_Deadline)
            {
                hide(window);
                m_Phase = Phase::Hidden;
                m_Deadline = frame + HIDE_WAIT_FRAMES;
            }
            break;
        case Phase::Hidden:
            if (window.isHidden())
            {
                ++m_Observed.framesWhileHidden;
            }
            else if (frame >= m_Deadline)
            {
                m_Observed.hideUnavailable = true;
                Core::Application::get().stop();
            }
            break;
        case Phase::AwaitShow:
            if (window.isHidden())
            {
                ++m_Observed.framesWhileHidden;
                break;
            }
            m_Observed.drewAfterShow = true;
            m_Observed.showToFrameMs = SDL_GetTicks() - m_ShowSentAt.load();
            m_Phase = Phase::Stop;
            m_Deadline = frame + STEP;
            break;
        case Phase::Stop:
            if (frame >= m_Deadline)
            {
                Core::Application::get().stop();
            }
            break;
        }
    }

    void onHiddenUpdate() override
    {
        const int wake = ++m_Observed.hiddenUpdates;
        if (wake == 1)
        {
            m_HiddenAtMs = SDL_GetTicks();
        }
        // Bounded by time, not wakes (#4026): every sampler's publication wakes the hidden loop, so wakes come many
        // a second at the test's 100 ms interval. The wake cap is only a backstop.
        const Uint64 hiddenForMs = SDL_GetTicks() - m_HiddenAtMs;
        if (hiddenForMs > HIDDEN_TIMEOUT_MS || wake > MAX_HIDDEN_UPDATES)
        {
            m_Observed.timedOut = true;
            Core::Application::get().stop();
            return;
        }
        if (m_Phase != Phase::Hidden)
        {
            return;
        }
        const Core::Window& window = Core::Application::get().getWindow();
        const std::uint64_t version = m_Shell.processDetailsPanelForTesting().lastSampleVersion();
        if (wake == 1)
        {
            m_Observed.hiddenAtFirstWake = window.isHidden();
            m_Observed.versionWhenHidden = version;
            return;
        }
        // Two more generations taken in while hidden, or long enough to have (#4026: 10 wakes could pass before one
        // pass of the process sampler on a loaded runner, so the fallback is a time, not a count of wakes).
        if (version < m_Observed.versionWhenHidden + 2 && hiddenForMs < SHOW_AFTER_MS)
        {
            return;
        }
        m_Observed.versionWhenShown = version;
        m_Phase = Phase::AwaitShow;
        m_Shower = std::thread(
            [this, handle = window.getHandle()]
            {
                std::this_thread::sleep_for(SEND_DELAY);
                m_ShowSentAt.store(SDL_GetTicks());
#ifdef _WIN32
                // As the taskbar restores a minimized window: to this window alone.
                auto* const hwnd =
                    static_cast<HWND>(SDL_GetPointerProperty(SDL_GetWindowProperties(handle), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
                PostMessageW(hwnd, WM_SYSCOMMAND, SC_RESTORE, 0);
#else
                static_cast<void>(handle);
                SDL_Event event{};
                event.type = SDL_EVENT_USER;
                static_cast<void>(SDL_PushEvent(&event));
#endif
            });
    }

  private:
    enum class Phase : std::uint8_t
    {
        Settle,
        AwaitWatching,
        Hidden,
        AwaitShow,
        Stop,
    };

    static void hide(Core::Window& window)
    {
#ifdef _WIN32
        // No Win+Down, whatever the real keyboard is doing: the minimize must not restore (#1279).
        window.setShellRestoreKeysReader([] noexcept { return std::pair{false, false}; });
        window.minimize();
#else
        window.setHiddenForTesting(true);
#endif
    }

    void joinShower()
    {
        if (m_Shower.joinable())
        {
            m_Shower.join();
        }
    }

    static constexpr int SETTLE = 20; // frames before the selection: the panels attach and sample once
    static constexpr int STEP = 5;    // frames drawn after the show before stopping
    static constexpr int WATCH_WAIT_FRAMES = 600;
    static constexpr int HIDE_WAIT_FRAMES = 60;
    static constexpr int MAX_FRAMES = 2000;
    static constexpr Uint64 SHOW_AFTER_MS = 5000;      // the show, if two generations haven't come by then (#4026)
    static constexpr Uint64 HIDDEN_TIMEOUT_MS = 20000; // the run, hidden that long, has failed
    static constexpr int MAX_HIDDEN_UPDATES = 5000;    // a backstop only: wakes come many a second
    // How long after the loop's wake the show is sent: well inside the next hidden wait.
    static constexpr std::chrono::milliseconds SEND_DELAY{100};

    HiddenShellObserved& m_Observed;
    const App::ShellLayer& m_Shell;
    std::thread m_Shower; // sends the show while the loop waits
    std::atomic<Uint64> m_ShowSentAt{0};
    Uint64 m_HiddenAtMs = 0; // SDL_GetTicks() at the first hidden wake
    int m_Deadline = 0;
    Phase m_Phase = Phase::Settle;
};

// #3156: a hidden window draws no frames at all, where it drew about 5 a second. The loop waits for
// the event that shows it again, which brings the next frame at once, and the samplers keep the
// Settings interval meanwhile: Process Details still takes each generation of the selected process
// on the loop's wakes, so its charts have no gap after a long minimize.
TEST_F(UiLayersOnDisplayTest, AHiddenWindowDrawsNoFramesKeepsItsSamplesAndDrawsAtOnceWhenShown)
{
    if (!hasDisplay())
    {
        GTEST_SKIP() << "No display available (headless environment)";
    }
    Core::ApplicationSpecification spec;
    spec.Name = "UiLayersOnDisplayHiddenShellTest";
    spec.VSync = false;
    HiddenShellObserved observed;
    try
    {
        Core::Application app(spec);
        app.pushLayer<UI::UILayer>();
        auto& titleBar = app.pushLayer<App::TitleBarLayer>();
        auto& shell = app.pushLayer<App::ShellLayer>();
        shell.setTitleBar(&titleBar);
        app.pushLayer<HiddenShellDriverLayer>(observed, shell);
        app.run();
    }
    catch (const std::exception& e)
    {
        FAIL() << "Application failed after the display probe passed: " << e.what();
    }

    if (observed.hideUnavailable)
    {
        GTEST_SKIP() << "The window didn't minimize on this display, so there was nothing to check";
    }
    EXPECT_FALSE(observed.timedOut);
    EXPECT_TRUE(observed.watching) << "Process Details took no sample of the selected process before the hide";
    EXPECT_TRUE(observed.hiddenAtFirstWake);
    EXPECT_EQ(observed.framesWhileHidden, 0) << "the hidden window still drew frames";
    EXPECT_GE(observed.hiddenUpdates, 2);
    EXPECT_GT(observed.versionWhenShown, observed.versionWhenHidden) << "Process Details took no sample while the window was hidden";
    EXPECT_TRUE(observed.drewAfterShow) << "no frame after the show";
    // Well under the hidden wait's timeout: the show woke the wait.
    EXPECT_LT(observed.showToFrameMs, static_cast<Uint64>(Core::WINDOW_HIDDEN_WAIT_TIMEOUT.count()) / 2U)
        << "the first frame after the show waited out the hidden wait";
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

TEST_F(UiLayersOnDisplayTest, TheWholeShellShowsEachTabAndSavesItsConfigOnClose)
{
    if (!hasDisplay())
    {
        GTEST_SKIP() << "No display available (headless environment)";
    }
    // Never the user's own config: the fixture points TASKSMACK_CONFIG_DIR's directory, and UserConfig,
    // at this test's own temporary directory, and the save on close must land there.
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
            // Each column is set up with its persistent ID (App::persistentColumnId(), #3551), in ProcessColumn's order.
            bool ids = true;
            for (const App::ProcessColumn col : App::allProcessColumns())
            {
                const ImGuiTableColumn& column = table->Columns[static_cast<int>(App::toIndex(col))];
                ids = ids && std::cmp_equal(column.UserData, App::persistentColumnId(col));
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

TEST_F(UiLayersOnDisplayTest, AFirstRunOpensAtTheDefaultsAndSavesTheGoldenConfig)
{
    if (!hasDisplay())
    {
        GTEST_SKIP() << "No display available (headless environment)";
    }
    // A first launch: the fixture's empty TASKSMACK_CONFIG_DIR, and UserConfig at its defaults.
    const std::optional<std::filesystem::path>& configOverride = Core::ConfigDirOverride::active();
    ASSERT_TRUE(configOverride.has_value());
    const std::filesystem::path configDir = configOverride.value_or(std::filesystem::path{});
    ASSERT_EQ(configDir, scratchConfigDir());
    std::error_code ec;
    ASSERT_TRUE(std::filesystem::is_empty(configDir, ec)) << ec.message();
    App::UserConfig& config = App::UserConfig::get();
    ASSERT_EQ(config.configPath(), configDir / "config.toml");

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
    std::string expected = maskMachineDependentKeys(golden.value_or(std::string{}));
    if (onNativeWayland())
    {
        // The compositor never says where the window is, so no position is saved there (#2724).
        for (const std::string_view line : {std::string_view("x = \"<masked>\"\n"), std::string_view("y = \"<masked>\"\n")})
        {
            if (const std::size_t at = expected.find(line); at != std::string::npos)
            {
                expected.erase(at, line.size());
            }
        }
    }
    EXPECT_EQ(masked, expected) << "The first-run config.toml is not " << Core::pathToUtf8(goldenPath)
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
