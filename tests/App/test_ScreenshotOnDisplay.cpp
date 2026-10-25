/// @file test_ScreenshotOnDisplay.cpp
/// @brief TASKSMACK_SCREENSHOT (#2827) end to end in a real Core::Application: the hook's value read
/// back from the environment, the whole shell composed as main.cpp composes it with App::ScreenshotLayer
/// last, and the app running until the layer has written the PNG and closed the window through the
/// normal close request -- so the file exists, decodes to the drawable's size, shows more than one
/// colour, and config.toml was saved on the way out. Then a sequence (frames=3): three numbered files.
/// Then TASKSMACK_DEMO (#2827) playing a two-step script while a sequence is captured: the steps are
/// issued on the app clock from the settle point, the tab changes, and the theme changes between the
/// first frame and the last.
///
/// Needs a display with a GL 3.3 core context, as test_UiLayersOnDisplay.cpp does (Xvfb in Linux CI,
/// Mesa llvmpipe on Windows CI, where TASKSMACK_REQUIRE_DISPLAY=1 turns a missing one into a failure);
/// skipped elsewhere.

#include "App/AboutLayer.h"
#include "App/DemoLayer.h"
#include "App/DemoOverride.h"
#include "App/ElevationNoticeLayer.h"
#include "App/HelpLayer.h"
#include "App/RenderMetricsOverride.h"
#include "App/ScreenshotLayer.h"
#include "App/ScreenshotOverride.h"
#include "App/SettingsLayer.h"
#include "App/ShellLayer.h"
#include "App/TitleBarLayer.h"
#include "Core/Application.h"
#include "Core/ApplicationEvents.h"
#include "Core/ConfigDirOverride.h"
#include "Core/EnvRead.h"
#include "Core/Event.h"
#include "Core/HeadlessVideoDriverTestUtils.h"
#include "Core/Layer.h"
#include "Core/Utf8Path.h"
#include "Core/Window.h"
#include "Core/WindowEvents.h"
#include "UI/Theme.h"
#include "UI/UILayer.h"

#include <SDL3/SDL_stdinc.h>
#include <gtest/gtest.h>
// NOLINTNEXTLINE(misc-include-cleaner) - stb_image.h's declarations; the implementation is Platform/StbImage.cpp
#include <stb_image.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <map>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace
{

/// A display with a GL 3.3 core context: the real one when DISPLAY or WAYLAND_DISPLAY is set and
/// usable, else SDL's offscreen driver if Mesa EGL can back it (as test_UiLayersOnDisplay.cpp).
bool hasDisplay()
{
#ifdef _WIN32
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

/// A hook (TASKSMACK_SCREENSHOT, TASKSMACK_DEMO) set to @p value (UTF-8) for the test, and unset when
/// the test ends. Set in SDL's own environment table, which Core::readEnv() (SDL_getenv()) reads, not
/// with SDL_setenv_unsafe(): once a previous test's SDL_Quit() has freed that table, the latter falls
/// back to SetEnvironmentVariableA(), which takes the bytes as the ANSI code page and mangles a
/// non-ASCII path. A real launch never meets this: SDL builds the table from the process's UTF-16
/// environment.
class HookEnv
{
  public:
    HookEnv(const std::string_view name, const std::string& value)
        : m_Name(name), m_Set(SDL_SetEnvironmentVariable(SDL_GetEnvironment(), m_Name.c_str(), value.c_str(), true))
    {}

    ~HookEnv()
    {
        static_cast<void>(SDL_UnsetEnvironmentVariable(SDL_GetEnvironment(), m_Name.c_str()));
    }

    HookEnv(const HookEnv&) = delete;
    HookEnv& operator=(const HookEnv&) = delete;
    HookEnv(HookEnv&&) = delete;
    HookEnv& operator=(HookEnv&&) = delete;

    [[nodiscard]] bool set() const noexcept
    {
        return m_Set;
    }

  private:
    std::string m_Name; ///< Built here, so the destructor allocates nothing
    bool m_Set = false;
};

/// TASKSMACK_SCREENSHOT set to @p value for the test (HookEnv).
class ScreenshotEnv : public HookEnv
{
  public:
    explicit ScreenshotEnv(const std::string& value) : HookEnv(App::ScreenshotOverride::ENV_VAR, value)
    {}

    /// The hook's value parsed from the environment, as main.cpp's active() parses it.
    [[nodiscard]] static App::ScreenshotOverride::ParseResult parseFromEnvironment()
    {
        return App::ScreenshotOverride::parse(Core::readEnv(App::ScreenshotOverride::ENV_VAR));
    }
};

/// TASKSMACK_DEMO set to @p value for the test (HookEnv).
class DemoEnv : public HookEnv
{
  public:
    explicit DemoEnv(const std::string& value) : HookEnv(App::DemoOverride::ENV_VAR, value)
    {}

    /// The hook's value parsed from the environment, as main.cpp's active() parses it.
    [[nodiscard]] static App::DemoOverride::ParseResult parseFromEnvironment()
    {
        return App::DemoOverride::parse(Core::readEnv(App::DemoOverride::ENV_VAR));
    }
};

/// A decoded PNG: straight RGBA, rows top-down. Empty (0x0) when the file isn't one.
struct Png
{
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba;
};

[[nodiscard]] Png decodePng(const std::filesystem::path& path)
{
    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* decoded = stbi_load(Core::pathToUtf8(path).c_str(), &width, &height, &channels, 4);
    if (decoded == nullptr)
    {
        return {};
    }
    const std::span<const std::uint8_t> pixels(decoded, static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U);
    Png png{.width = width, .height = height, .rgba = std::vector<std::uint8_t>(pixels.begin(), pixels.end())};
    stbi_image_free(decoded);
    return png;
}

/// Whether any pixel's colour differs from the first one's: a screenshot of the shell is no blank.
[[nodiscard]] bool hasMoreThanOneColour(const Png& png)
{
    constexpr std::size_t PIXEL = 4;
    const std::span<const std::uint8_t> pixels(png.rgba);
    if (pixels.size() < PIXEL)
    {
        return false;
    }
    const std::span<const std::uint8_t> first = pixels.first(3);
    for (std::size_t offset = PIXEL; offset + PIXEL <= pixels.size(); offset += PIXEL)
    {
        if (!std::ranges::equal(pixels.subspan(offset, 3), first))
        {
            return true;
        }
    }
    return false;
}

/// The colour most of a PNG's pixels have (the window background, for a screenshot of the shell), as
/// RGB; black for an empty image.
[[nodiscard]] std::array<std::uint8_t, 3> dominantColour(const Png& png)
{
    constexpr std::size_t PIXEL = 4;
    std::map<std::array<std::uint8_t, 3>, std::size_t> counts;
    const std::span<const std::uint8_t> pixels(png.rgba);
    for (std::size_t offset = 0; offset + PIXEL <= pixels.size(); offset += PIXEL)
    {
        ++counts[{pixels[offset], pixels[offset + 1], pixels[offset + 2]}];
    }
    std::array<std::uint8_t, 3> dominant{};
    std::size_t most = 0;
    for (const auto& [colour, count] : counts)
    {
        if (count > most)
        {
            most = count;
            dominant = colour;
        }
    }
    return dominant;
}

/// What the observer saw.
struct Observed
{
    int frames = 0;
    int closeRequests = 0;              ///< WindowCloseEvents seen: the normal close path
    std::pair<int, int> lastSize{0, 0}; ///< The drawable's size on the last frame
    bool timedOut = false;
    std::string lastTab;      ///< The last ActiveTabChangedEvent's tab, by registered id; empty if none
    std::string firstThemeId; ///< UI::Theme's theme after the first frame
    std::string lastThemeId;  ///< ... and after the last
};

/// Pushed last, after ScreenshotLayer: counts the frames, notes the drawable's size and the theme on
/// each, sees the close request and the tab changes go by, and stops the app itself if the hook hasn't
/// closed it within TIMEOUT_SECONDS.
class ObserverLayer : public Core::Layer
{
  public:
    explicit ObserverLayer(Observed& observed) : Layer("ScreenshotObserver"), m_Observed(observed)
    {}

    void onAttach() override
    {
        m_Start = Core::Application::getTime();
    }

    void onPostRender() override
    {
        ++m_Observed.frames;
        m_Observed.lastSize = Core::Application::get().getWindow().getSizeInPixels();
        // After the frame: a theme queued before it (the startup one, a demo step's) has been applied.
        m_Observed.lastThemeId = UI::Theme::get().currentThemeId();
        if (m_Observed.frames == 1)
        {
            m_Observed.firstThemeId = m_Observed.lastThemeId;
        }
        if (Core::Application::getTime() - m_Start > TIMEOUT_SECONDS)
        {
            m_Observed.timedOut = true;
            Core::Application::get().stop();
        }
    }

    void onEvent(Core::Event& event) override
    {
        Core::EventDispatcher dispatcher(event);
        dispatcher.dispatch<Core::WindowCloseEvent>(
            [this](Core::WindowCloseEvent&)
            {
                ++m_Observed.closeRequests;
                return false; // observe, never veto
            });
        dispatcher.dispatch<Core::ActiveTabChangedEvent>(
            [this](Core::ActiveTabChangedEvent& e)
            {
                m_Observed.lastTab = e.tabName();
                return false;
            });
    }

  private:
    static constexpr double TIMEOUT_SECONDS = 20.0;

    Observed& m_Observed;
    double m_Start = 0.0;
};

/// What a run left behind, copied out of the layers before the application is destroyed.
struct RunResult
{
    App::ScreenshotLayer::Outcome outcome = App::ScreenshotLayer::Outcome::Pending;
    int framesWritten = 0;
    std::pair<int, int> capturedSize{0, 0};
    Observed observed;
};

/// A small window, so the capture is quick: the drawable's real size is read back, whatever the shell's
/// minimum makes of it.
[[nodiscard]] Core::ApplicationSpecification smallWindow(const char* name)
{
    Core::ApplicationSpecification spec;
    spec.Name = name;
    spec.Width = 640;
    spec.Height = 400;
    spec.VSync = false;
    return spec;
}

/// A fresh directory with a non-ASCII name under the system's temporary directory (a screenshot path
/// can have one, #1648), removed with everything in it when the test ends.
class ScreenshotOnDisplayTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        std::random_device random;
        constexpr int MAX_ATTEMPTS = 100;
        for (int attempt = 0; attempt < MAX_ATTEMPTS; ++attempt)
        {
            m_Dir = std::filesystem::temp_directory_path() / Core::utf8ToPath("tasksmack_снимок_" + std::to_string(random()));
            std::error_code ec;
            if (std::filesystem::create_directory(m_Dir, ec) && !ec)
            {
                return;
            }
        }
        FAIL() << "Failed to create a temp directory";
    }

    void TearDown() override
    {
        std::error_code ec;
        std::filesystem::remove_all(m_Dir, ec);
    }

    std::filesystem::path m_Dir;
};

TEST_F(ScreenshotOnDisplayTest, TheHookWritesThePngOfTheShellAndClosesTheWindow)
{
    if (!hasDisplay())
    {
        GTEST_SKIP() << "No display available (headless environment)";
    }
    // The config saved on close lands in AppTestsMain.cpp's temporary TASKSMACK_CONFIG_DIR.
    const std::optional<std::filesystem::path>& configDir = Core::ConfigDirOverride::active();
    ASSERT_TRUE(configDir.has_value());

    const std::filesystem::path file = m_Dir / Core::utf8ToPath("снимок.png");
    const ScreenshotEnv env(Core::pathToUtf8(file) + ";after=1");
    ASSERT_TRUE(env.set());
    const App::ScreenshotOverride::ParseResult parsed = ScreenshotEnv::parseFromEnvironment();
    ASSERT_TRUE(parsed.request.has_value()) << parsed.warning;
    const App::ScreenshotOverride::Request request = parsed.request.value_or(App::ScreenshotOverride::Request{});
    EXPECT_EQ(request.path, file);
    EXPECT_EQ(request.afterSeconds, 1);
    EXPECT_EQ(request.frames, 1);

    // Composed as main.cpp's runApp() composes it, the screenshot layer last.
    RunResult run;
    try
    {
        Core::Application app(smallWindow("ScreenshotOnDisplayTest"));
        app.pushLayer<UI::UILayer>();
        App::TitleBarLayer* titleBar = app.getWindow().isBorderless() ? &app.pushLayer<App::TitleBarLayer>() : nullptr;
        auto& shell = app.pushLayer<App::ShellLayer>();
        shell.setTitleBar(titleBar);
        app.pushLayer<App::HelpLayer>();
        app.pushLayer<App::AboutLayer>();
        app.pushLayer<App::SettingsLayer>();
        app.pushLayer<App::ElevationNoticeLayer>();
        const App::ScreenshotLayer& screenshot = app.pushLayer<App::ScreenshotLayer>(request, &shell);
        app.pushLayer<ObserverLayer>(run.observed);
        app.run();
        run.outcome = screenshot.outcome();
        run.framesWritten = screenshot.framesWritten();
        run.capturedSize = screenshot.capturedSize();
    }
    catch (const std::exception& e)
    {
        FAIL() << "Application failed after the display probe passed: " << e.what();
    }

    EXPECT_FALSE(run.observed.timedOut) << "the hook didn't close the window in time";
    EXPECT_EQ(run.outcome, App::ScreenshotLayer::Outcome::Written);
    EXPECT_EQ(run.framesWritten, 1);
    EXPECT_EQ(run.observed.closeRequests, 1) << "closed by the normal close request, once";
    EXPECT_GT(run.observed.frames, App::ScreenshotLayer::SETTLE_FRAMES);

    std::error_code ec;
    ASSERT_TRUE(std::filesystem::exists(file, ec)) << Core::pathToUtf8(file);
    const Png png = decodePng(file);
    ASSERT_GT(png.width, 0) << "not a PNG: " << stbi_failure_reason();
    EXPECT_EQ(png.width, run.capturedSize.first);
    EXPECT_EQ(png.height, run.capturedSize.second);
    EXPECT_EQ((std::pair{png.width, png.height}), run.observed.lastSize) << "the drawable's size";
    EXPECT_TRUE(hasMoreThanOneColour(png)) << "a blank image";
    EXPECT_TRUE(std::filesystem::exists(configDir.value_or(std::filesystem::path{}) / "config.toml", ec))
        << "saved by ShellLayer::onDetach()";
}

TEST_F(ScreenshotOnDisplayTest, ASequenceWritesItsNumberedFramesInOrderThenCloses)
{
    if (!hasDisplay())
    {
        GTEST_SKIP() << "No display available (headless environment)";
    }

    const std::filesystem::path given = m_Dir / "seq.png";
    const ScreenshotEnv env(Core::pathToUtf8(given) + ";after=1;frames=3;fps=10");
    ASSERT_TRUE(env.set());
    const App::ScreenshotOverride::ParseResult parsed = ScreenshotEnv::parseFromEnvironment();
    ASSERT_TRUE(parsed.request.has_value()) << parsed.warning;
    const App::ScreenshotOverride::Request request = parsed.request.value_or(App::ScreenshotOverride::Request{});
    EXPECT_EQ(request.frames, 3);
    EXPECT_EQ(request.fps, 10);

    RunResult run;
    try
    {
        Core::Application app(smallWindow("ScreenshotOnDisplaySequenceTest"));
        app.pushLayer<UI::UILayer>();
        if (app.getWindow().isBorderless())
        {
            app.pushLayer<App::TitleBarLayer>();
        }
        const App::ScreenshotLayer& screenshot = app.pushLayer<App::ScreenshotLayer>(request, nullptr);
        app.pushLayer<ObserverLayer>(run.observed);
        app.run();
        run.outcome = screenshot.outcome();
        run.framesWritten = screenshot.framesWritten();
        run.capturedSize = screenshot.capturedSize();
    }
    catch (const std::exception& e)
    {
        FAIL() << "Application failed after the display probe passed: " << e.what();
    }

    EXPECT_FALSE(run.observed.timedOut) << "the hook didn't close the window in time";
    EXPECT_EQ(run.outcome, App::ScreenshotLayer::Outcome::Written);
    EXPECT_EQ(run.framesWritten, 3);
    EXPECT_EQ(run.observed.closeRequests, 1);

    // seq-0001.png, seq-0002.png, seq-0003.png and nothing else: not the given name. Compared as UTF-8,
    // which gtest can print whatever the code page (#1648).
    std::vector<std::string> files;
    std::error_code ec;
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(m_Dir, ec))
    {
        files.push_back(Core::pathToUtf8(entry.path()));
    }
    std::ranges::sort(files);
    std::vector<std::string> expected;
    for (int index = 1; index <= 3; ++index)
    {
        expected.push_back(Core::pathToUtf8(App::ScreenshotOverride::frameFileName(given, index)));
    }
    EXPECT_EQ(files, expected);
    EXPECT_FALSE(std::filesystem::exists(given, ec));
    for (const std::string& frame : expected)
    {
        SCOPED_TRACE(frame);
        const Png png = decodePng(Core::utf8ToPath(frame));
        ASSERT_GT(png.width, 0) << "not a PNG: " << stbi_failure_reason();
        EXPECT_EQ(png.width, run.capturedSize.first);
        EXPECT_EQ(png.height, run.capturedSize.second);
    }
}

// TASKSMACK_DEMO (#2827) with a sequence: no startup selection, so the script's clock and the
// screenshot's settle (after=0) start on the first frame. The first capture follows SETTLE_FRAMES frames
// later and the third one 2 s after that (fps=1), so the script's tab= at 0.5 s and theme= at 1 s have
// both been issued, through the shell's own request paths, by the third frame -- which therefore shows
// the Processes tab in the dracula theme, in another colour than the first frame's startup theme.
TEST_F(ScreenshotOnDisplayTest, ADemoScriptPlaysWhileTheSequenceIsCaptured)
{
    if (!hasDisplay())
    {
        GTEST_SKIP() << "No display available (headless environment)";
    }

    const std::filesystem::path given = m_Dir / "demo.png";
    const ScreenshotEnv screenshotEnv(Core::pathToUtf8(given) + ";after=0;frames=3;fps=1");
    // metrics=on then metrics=export (#2923): with TASKSMACK_RENDER_METRICS unset the CSV lands beside the
    // screenshot, as main.cpp resolves it; the last frame is captured with the overlay on.
    const DemoEnv demoEnv("0.5:tab=processes,1:theme=dracula,1:metrics=on,1.5:metrics=export");
    ASSERT_TRUE(screenshotEnv.set());
    ASSERT_TRUE(demoEnv.set());
    const App::ScreenshotOverride::ParseResult parsedShot = ScreenshotEnv::parseFromEnvironment();
    ASSERT_TRUE(parsedShot.request.has_value()) << parsedShot.warning;
    const App::ScreenshotOverride::Request request = parsedShot.request.value_or(App::ScreenshotOverride::Request{});
    const App::DemoOverride::ParseResult parsedDemo = DemoEnv::parseFromEnvironment();
    ASSERT_TRUE(parsedDemo.steps.has_value()) << parsedDemo.warning;
    const std::vector<App::DemoOverride::Step> steps = parsedDemo.steps.value_or(std::vector<App::DemoOverride::Step>{});
    ASSERT_EQ(steps.size(), 4U);
    EXPECT_EQ(App::DemoOverride::describe(steps), "0.5:tab=processes, 1:theme=dracula, 1:metrics=on, 1.5:metrics=export");
    const std::filesystem::path metricsCsv = App::RenderMetricsOverride::csvBesideScreenshot(request.path);
    EXPECT_EQ(metricsCsv, m_Dir / "demo-metrics.csv");

    // Composed as main.cpp's runApp() composes it: the demo layer after the dialog layers, the
    // screenshot layer last.
    RunResult run;
    bool demoStarted = false;
    std::size_t stepsIssued = 0;
    bool renderMetricsShown = false;
    try
    {
        Core::Application app(smallWindow("ScreenshotOnDisplayDemoTest"));
        app.pushLayer<UI::UILayer>();
        App::TitleBarLayer* titleBar = app.getWindow().isBorderless() ? &app.pushLayer<App::TitleBarLayer>() : nullptr;
        auto& shell = app.pushLayer<App::ShellLayer>();
        shell.setTitleBar(titleBar);
        app.pushLayer<App::HelpLayer>();
        app.pushLayer<App::AboutLayer>();
        app.pushLayer<App::SettingsLayer>();
        app.pushLayer<App::ElevationNoticeLayer>();
        const App::DemoLayer& demo = app.pushLayer<App::DemoLayer>(steps, shell, metricsCsv);
        const App::ScreenshotLayer& screenshot = app.pushLayer<App::ScreenshotLayer>(request, &shell);
        app.pushLayer<ObserverLayer>(run.observed);
        app.run();
        run.outcome = screenshot.outcome();
        run.framesWritten = screenshot.framesWritten();
        run.capturedSize = screenshot.capturedSize();
        demoStarted = demo.started();
        stepsIssued = demo.stepsIssued();
        renderMetricsShown = shell.renderMetricsShown();
    }
    catch (const std::exception& e)
    {
        FAIL() << "Application failed after the display probe passed: " << e.what();
    }

    EXPECT_FALSE(run.observed.timedOut) << "the hook didn't close the window in time";
    EXPECT_EQ(run.outcome, App::ScreenshotLayer::Outcome::Written);
    EXPECT_EQ(run.framesWritten, 3);
    EXPECT_EQ(run.observed.closeRequests, 1);
    EXPECT_TRUE(demoStarted) << "the script's clock never started";
    EXPECT_EQ(stepsIssued, 4U) << "every step was due before the last capture";
    EXPECT_EQ(run.observed.lastTab, "Processes") << "tab=processes selected the Processes tab";
    EXPECT_NE(run.observed.firstThemeId, "dracula") << "the startup theme is another";
    EXPECT_EQ(run.observed.lastThemeId, "dracula") << "theme=dracula applied";
    EXPECT_TRUE(renderMetricsShown) << "metrics=on showed the Render Metrics overlay";
    // metrics=export wrote the overlay's CSV beside the screenshot: its header, then the last completed
    // frame's rows (the Processes tab draws no chart, so none here).
    ASSERT_TRUE(std::filesystem::exists(metricsCsv)) << Core::pathToUtf8(metricsCsv);
    {
        std::ifstream csvFile(metricsCsv, std::ios::binary);
        const std::string csv((std::istreambuf_iterator<char>(csvFile)), std::istreambuf_iterator<char>());
        EXPECT_TRUE(csv.starts_with("timestamp_us,scenario,frame_id,publication_id,chart,vertices,cpu_us\n")) << csv;
    }

    const Png first = decodePng(App::ScreenshotOverride::frameFileName(given, 1));
    const Png last = decodePng(App::ScreenshotOverride::frameFileName(given, 3));
    ASSERT_GT(first.width, 0) << "not a PNG: " << stbi_failure_reason();
    ASSERT_GT(last.width, 0) << "not a PNG: " << stbi_failure_reason();
    EXPECT_EQ((std::pair{last.width, last.height}), run.capturedSize);
    EXPECT_TRUE(hasMoreThanOneColour(first));
    EXPECT_TRUE(hasMoreThanOneColour(last));
    EXPECT_NE(first.rgba, last.rgba) << "the first and the last frame are the same image";
    EXPECT_NE(dominantColour(first), dominantColour(last)) << "the window background did not change with the theme";
}

} // namespace
