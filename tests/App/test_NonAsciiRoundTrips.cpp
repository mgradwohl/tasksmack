/// @file test_NonAsciiRoundTrips.cpp
/// @brief Non-ASCII text through every place a path or name crosses an encoding boundary (#1648,
/// slice C): the config directory and config.toml, user themes, the log file, the startup selection
/// by process name, process names and command lines from the probes to the Processes table, and the
/// argument handed to the system's file opener.
///
/// This binary has no application manifest, so on Windows it runs in the system's ANSI code page
/// (often 1252), not UTF-8. Every test here passes without activeCodePage: the code under test must
/// convert paths explicitly (Core/Utf8Path.h, W APIs, the native path), never through the code page.
/// Everything is written under a fresh temporary directory with a non-ASCII name, removed afterwards.

#include "App/Panels/ProcessesPanel.h"
#include "App/PlatformOpen.h"
#include "App/SelectOverride.h"
#include "App/UserConfig.h"
#include "Core/ApplicationEvents.h"
#include "Core/ConfigDirOverride.h"
#include "Core/LogFileSink.h"
#include "Core/Utf8Path.h"
#include "Domain/ProcessModel.h"
#include "Domain/ProcessSnapshot.h"
#include "Mocks/MockProbes.h"
#include "NonAsciiTestNames.h"
#include "Platform/ProcessTypes.h"
#include "UI/ThemeLoader.h"

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <implot.h>
#include <spdlog/logger.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace
{

using TestSupport::allNonAsciiNames;
using TestSupport::CJK_NAME;
using TestSupport::EMOJI_NAME;
using TestSupport::LATIN_NAME;
using TestSupport::NON_ASCII_NAMES;
using TestSupport::NonAsciiTempDir;
using TestSupport::RTL_NAME;

/// Not valid UTF-8: a truncated two-byte sequence, a lone continuation byte, and 0xFF (never valid).
constexpr std::string_view INVALID_UTF8 = "bad\xC3(\x80\xFFname";

[[nodiscard]] std::string readWholeFile(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

/// The names of @p dir's entries, as UTF-8.
[[nodiscard]] std::vector<std::string> entryNames(const std::filesystem::path& dir)
{
    std::vector<std::string> names;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
    {
        names.push_back(Core::pathToUtf8(it->path().filename()));
    }
    std::ranges::sort(names);
    return names;
}

class NonAsciiRoundTripTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        if (!m_Dir.created())
        {
            GTEST_SKIP() << "The filesystem refused a non-ASCII directory name under the temp directory";
        }
    }

    [[nodiscard]] const std::filesystem::path& dir() const noexcept
    {
        return m_Dir.path();
    }

  private:
    NonAsciiTempDir m_Dir;
};

// ========== Paths and UTF-8 text ==========

TEST_F(NonAsciiRoundTripTest, PathTextRoundTripsEveryName)
{
    for (const std::string_view name : NON_ASCII_NAMES)
    {
        const std::filesystem::path path = Core::utf8ToPath(name);
        EXPECT_EQ(Core::pathToUtf8(path), name);
        EXPECT_EQ(Core::utf8ToPath(Core::pathToUtf8(dir() / path)), dir() / path);
    }
}

#ifdef _WIN32
TEST(NonAsciiPathTest, NarrowConversionIsUnsafeWithoutTheManifest)
{
    // Why every path above goes through Core::pathToUtf8 / utf8ToPath: without activeCodePage the
    // narrow conversions use the system code page. Recorded so a failure elsewhere can be read
    // against the code page this run had.
    const UINT codePage = GetACP();
    RecordProperty("GetACP", static_cast<int>(codePage));
    if (codePage == CP_UTF8)
    {
        GTEST_SKIP() << "The system code page is UTF-8 (the \"Beta: Use Unicode UTF-8\" setting), so narrow conversions are safe here";
    }
    const std::filesystem::path cjk = Core::utf8ToPath(CJK_NAME);
    std::string narrow;
    bool threw = false;
    try
    {
        narrow = cjk.string();
    }
    catch (const std::system_error&)
    {
        threw = true;
    }
    // Either it can't be represented at all, or it comes out as something other than UTF-8.
    EXPECT_TRUE(threw || narrow != CJK_NAME) << "GetACP() = " << codePage;
}
#endif

// ========== Config directory and config.toml ==========

class NonAsciiConfigTest : public NonAsciiRoundTripTest
{
  protected:
    void SetUp() override
    {
        NonAsciiRoundTripTest::SetUp();
        if (IsSkipped())
        {
            return;
        }
        m_OriginalPath = App::UserConfig::get().configPath();
    }

    void TearDown() override
    {
        if (!m_OriginalPath.empty())
        {
            App::UserConfig::get().resetConfigPathForTesting(m_OriginalPath);
        }
    }

  private:
    std::filesystem::path m_OriginalPath;
};

TEST_F(NonAsciiConfigTest, ConfigDirOverrideNamesTheExactDirectory)
{
    // TASKSMACK_CONFIG_DIR's value is UTF-8 (SDL_getenv converts the UTF-16 environment).
    const std::string value = Core::pathToUtf8(dir() / "config");
    const std::optional<std::filesystem::path> parsed = Core::ConfigDirOverride::parse(value.c_str());
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed.value(), dir() / "config");
}

TEST_F(NonAsciiConfigTest, ConfigSavesAndLoadsUnderANonAsciiDirectoryWithNonAsciiValues)
{
    // A config directory that doesn't exist yet (save() creates it), named like the others, as
    // TASKSMACK_CONFIG_DIR would give it.
    const std::string configDirText = std::format("{}/{}-config", Core::pathToUtf8(dir()), allNonAsciiNames());
    const std::optional<std::filesystem::path> configDir = Core::ConfigDirOverride::parse(configDirText.c_str());
    ASSERT_TRUE(configDir.has_value());
    const std::filesystem::path configPath = configDir.value() / "config.toml";

    // The free-text settings: the theme id (a user theme's file name) and the table layout.
    const std::string themeId = allNonAsciiNames();
    const std::string layout = std::format("Name {} | {}", RTL_NAME, EMOJI_NAME);

    auto& config = App::UserConfig::get();
    config.resetConfigPathForTesting(configPath);
    config.settings().themeId = themeId;
    config.settings().processTableLayout = layout;
    config.save();

    // At exactly that path: no sibling under a mangled name.
    ASSERT_TRUE(std::filesystem::exists(configPath));
    EXPECT_EQ(entryNames(dir()), std::vector<std::string>{std::format("{}-config", allNonAsciiNames())});
    // Stored as UTF-8 text, not escaped and not in the code page.
    const std::string text = readWholeFile(configPath);
    EXPECT_TRUE(text.contains(themeId)) << text;
    EXPECT_TRUE(text.contains(layout)) << text;

    config.resetConfigPathForTesting(configPath); // defaults again, so load() must read the file
    ASSERT_NE(config.settings().themeId, themeId);
    config.load();
    EXPECT_EQ(config.settings().themeId, themeId);
    EXPECT_EQ(config.settings().processTableLayout, layout);

    // A second save merges into the existing file, which save() reads back first.
    config.settings().processTableLayout = std::string(LATIN_NAME);
    config.save();
    config.resetConfigPathForTesting(configPath);
    config.load();
    EXPECT_EQ(config.settings().themeId, themeId);
    EXPECT_EQ(config.settings().processTableLayout, LATIN_NAME);
}

// ========== User themes ==========

TEST_F(NonAsciiRoundTripTest, UserThemeWithANonAsciiFileNameIsDiscoveredAndLoaded)
{
    const std::filesystem::path themesDir = dir() / "themes";
    std::filesystem::create_directories(themesDir);
    const std::string stem = std::format("{}{}", CJK_NAME, EMOJI_NAME);
    const std::filesystem::path themePath = themesDir / Core::utf8ToPath(stem + ".toml");
    std::filesystem::copy_file(std::filesystem::path(TASKSMACK_SOURCE_THEMES_DIR) / "arctic-fire.toml", themePath);

    const std::vector<UI::ThemeInfo> themes = UI::ThemeLoader::discoverThemes(themesDir);
    ASSERT_EQ(themes.size(), 1U);
    EXPECT_EQ(themes[0].id, stem); // the id config.toml stores: UTF-8, whatever the code page
    EXPECT_EQ(themes[0].path, themePath);
    EXPECT_TRUE(UI::ThemeLoader::loadTheme(themePath).has_value());
}

// ========== Log file ==========

TEST_F(NonAsciiRoundTripTest, LogFileIsCreatedAtTheExactNonAsciiPath)
{
    // As main.cpp opens %TEMP%\tasksmack-debug.log, with a directory spdlog has to create.
    const std::filesystem::path logDir = dir() / Core::utf8ToPath(std::format("logs-{}", allNonAsciiNames()));
    const std::filesystem::path logPath = logDir / "tasksmack-debug.log";
    const std::string message = std::format("hello {}", allNonAsciiNames());
    {
        spdlog::logger logger("NonAsciiLogTest", Core::makeLogFileSink(logPath, true));
        logger.set_pattern("%v");
        logger.info("{}", message);
        logger.flush();
    } // closes the file, so the directory can be removed afterwards

    ASSERT_TRUE(std::filesystem::exists(logPath));
    EXPECT_EQ(entryNames(dir()), std::vector<std::string>{Core::pathToUtf8(logDir.filename())});
    EXPECT_TRUE(readWholeFile(logPath).contains(message));
}

// ========== Startup selection by process name (TASKSMACK_SELECT_NAME) ==========

[[nodiscard]] Domain::ProcessSnapshot snapshotNamed(std::int32_t pid, std::string name)
{
    Domain::ProcessSnapshot snapshot;
    snapshot.pid = pid;
    snapshot.name = std::move(name);
    return snapshot;
}

TEST(NonAsciiSelectTest, SelectsAProcessByItsNonAsciiName)
{
    for (const std::string_view name : NON_ASCII_NAMES)
    {
        const std::string exe = std::format("{}.exe", name);
        const std::string value = std::format("  {}  ", exe);
        const auto parsed = App::SelectOverride::parse(nullptr, value.c_str(), nullptr);
        ASSERT_TRUE(parsed.target.has_value()) << exe;
        EXPECT_EQ(parsed.target.value().name, exe);
        EXPECT_TRUE(App::SelectOverride::matches(parsed.target.value(), snapshotNamed(7, exe))) << exe;
        EXPECT_FALSE(App::SelectOverride::matches(parsed.target.value(), snapshotNamed(7, "other.exe"))) << exe;
    }
}

TEST(NonAsciiSelectTest, NonAsciiCaseFollowsThePlatform)
{
    const auto target = App::SelectOverride::parse(nullptr, "ÜNÏCÖDÉ.EXE", nullptr).target;
    ASSERT_TRUE(target.has_value());
#ifdef _WIN32
    // Windows file names ignore case beyond ASCII too (ordinal, on UTF-16).
    EXPECT_TRUE(App::SelectOverride::matches(target.value(), snapshotNamed(1, "ünïcödé.exe")));
#else
    EXPECT_FALSE(App::SelectOverride::matches(target.value(), snapshotNamed(1, "ünïcödé.exe")));
#endif
    EXPECT_TRUE(App::SelectOverride::matches(target.value(), snapshotNamed(1, "ÜNÏCÖDÉ.EXE")));
}

TEST(NonAsciiSelectTest, InvalidUtf8NamesCompareExactlyWithoutCrashing)
{
    // A Linux process name is bytes and needn't be UTF-8; the comparison must not crash or treat
    // two different invalid names as equal (an empty conversion of each).
    EXPECT_TRUE(App::SelectOverride::processNamesEqual(INVALID_UTF8, INVALID_UTF8));
    EXPECT_FALSE(App::SelectOverride::processNamesEqual(INVALID_UTF8, "bad\xC3(\x80\xFEname"));
    EXPECT_FALSE(App::SelectOverride::processNamesEqual(INVALID_UTF8, ""));
}

// ========== Process names and command lines ==========

[[nodiscard]] std::vector<Platform::ProcessCounters> nonAsciiCounters()
{
    std::vector<Platform::ProcessCounters> counters;
    std::int32_t pid = 100;
    for (const std::string_view name : NON_ASCII_NAMES)
    {
        auto counter = TestMocks::makeProcessCounters(pid, std::format("{}.exe", name));
        counter.command = std::format(R"(C:\{}\{}.exe --title="{} {}")", CJK_NAME, name, RTL_NAME, LATIN_NAME);
        counter.user = std::string(LATIN_NAME);
        counters.push_back(std::move(counter));
        ++pid;
    }
    auto invalid = TestMocks::makeProcessCounters(pid, std::string(INVALID_UTF8));
    invalid.command = std::format("/opt/{}/run", INVALID_UTF8);
    counters.push_back(std::move(invalid));
    return counters;
}

TEST(NonAsciiProcessTest, SnapshotsKeepNonAsciiNamesAndCommandsByteForByte)
{
    const std::vector<Platform::ProcessCounters> counters = nonAsciiCounters();
    Domain::ProcessModel model(nullptr);
    model.updateFromCounters(counters, 100000);
    model.updateFromCounters(counters, 101000); // the second pass reuses the single-line memo (#1624)

    const std::vector<Domain::ProcessSnapshot> snapshots = model.snapshots();
    ASSERT_EQ(snapshots.size(), counters.size());
    for (const auto& counter : counters)
    {
        const auto found = std::ranges::find_if(snapshots, [&](const auto& snapshot) { return snapshot.pid == counter.pid; });
        ASSERT_NE(found, snapshots.end());
        // Sanitizing replaces only C0 controls and DEL, so every multi-byte sequence -- and every
        // invalid byte, which the UI later draws as a replacement glyph -- passes through as it is.
        EXPECT_EQ(found->name, counter.name);
        EXPECT_EQ(found->command, counter.command);
        EXPECT_EQ(found->user, counter.user);
    }
}

class NonAsciiProcessesPanelTest : public ::testing::Test
{
  protected:
    static constexpr float DISPLAY_WIDTH = 1600.0F;
    static constexpr float DISPLAY_HEIGHT = 1200.0F;

    void SetUp() override
    {
        m_ImGui = ImGui::CreateContext();
        m_ImPlot = ImPlot::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.DisplaySize = ImVec2(DISPLAY_WIDTH, DISPLAY_HEIGHT);
        io.DeltaTime = 1.0F / 60.0F;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
        io.Fonts->AddFontDefault(); // ASCII only: every other character is drawn as a fallback glyph
    }

    void TearDown() override
    {
        ImPlot::DestroyContext(m_ImPlot);
        ImGui::DestroyContext(m_ImGui);
    }

    /// One frame of @p panel's content, and the text it drew.
    [[nodiscard]] static std::string renderAndCapture(App::ProcessesPanel& panel)
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
        ImGui::SetNextWindowSize(ImVec2(DISPLAY_WIDTH, DISPLAY_HEIGHT));
        ImGui::Begin("Shell", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
        ImGui::LogToBuffer();
        panel.renderContent();
        std::string captured = GImGui->LogBuffer.c_str();
        ImGui::LogFinish();
        ImGui::End();
        ImGui::Render();
        return captured;
    }

  private:
    ImGuiContext* m_ImGui = nullptr;
    ImPlotContext* m_ImPlot = nullptr;
};

TEST_F(NonAsciiProcessesPanelTest, TheTableDrawsNonAsciiAndInvalidNamesWithoutCrashing)
{
    const auto panel = std::make_unique<App::ProcessesPanel>(
        []
        {
            auto probe = std::make_unique<TestMocks::MockProcessProbe>();
            for (auto& counter : nonAsciiCounters())
            {
                probe->withProcess(std::move(counter));
            }
            return App::ProcessesPanelPlatform{.probe = std::move(probe), .actions = std::make_unique<TestMocks::MockProcessActions>()};
        });
    panel->onAttach();
    Core::ActiveTabChangedEvent shown("Processes");
    panel->onEvent(shown);

    static_cast<void>(renderAndCapture(*panel));
    const std::string text = renderAndCapture(*panel);
    for (const std::string_view name : NON_ASCII_NAMES)
    {
        EXPECT_TRUE(text.contains(std::format("{}.exe", name))) << name;
    }
    // The tree view lays rows out differently (indents, its own sort); it must cope too.
    panel->toggleTreeView();
    static_cast<void>(renderAndCapture(*panel));
    EXPECT_TRUE(renderAndCapture(*panel).contains(std::format("{}.exe", CJK_NAME)));
    panel->onDetach();
}

// ========== Open in the file manager / browser ==========
// Only the argument conversion: these tests never launch anything.

TEST_F(NonAsciiRoundTripTest, OpenerGetsThePathsOwnNativeForm)
{
    const std::filesystem::path file = dir() / Core::utf8ToPath(std::format("{}.toml", allNonAsciiNames()));
    const App::PlatformOpen::NativeTarget argument = App::PlatformOpen::nativeTargetFromPath(file);
    EXPECT_EQ(argument, file.native());
#ifdef _WIN32
    EXPECT_TRUE(argument.ends_with(L"\\任务管理器🔥Ünïcödéمدير.toml"));
#else
    EXPECT_TRUE(argument.ends_with(std::format("/{}.toml", allNonAsciiNames())));
#endif
}

TEST(NonAsciiOpenTest, OpenerConvertsUtf8TargetsWithoutTheCodePage)
{
    const std::string url = std::format("https://example.com/{}", allNonAsciiNames());
    const App::PlatformOpen::NativeTarget argument = App::PlatformOpen::nativeTargetFromUtf8(url);
#ifdef _WIN32
    EXPECT_EQ(argument, L"https://example.com/任务管理器🔥Ünïcödéمدير");
    // Not UTF-8: refused (empty, so nothing is opened), never guessed at.
    EXPECT_TRUE(App::PlatformOpen::nativeTargetFromUtf8(INVALID_UTF8).empty());
#else
    EXPECT_EQ(argument, url);
    EXPECT_EQ(App::PlatformOpen::nativeTargetFromUtf8(INVALID_UTF8), INVALID_UTF8); // bytes, as xdg-open gets them
#endif
}

#ifndef _WIN32
// ========== Linux: a file name that isn't UTF-8 ==========

TEST_F(NonAsciiRoundTripTest, InvalidUtf8FileNameRoundTripsAsBytes)
{
    // A Linux file name is bytes. One that isn't UTF-8 keeps its exact bytes through the helpers, so
    // the file can still be opened by the path rebuilt from its text.
    const std::filesystem::path file = dir() / Core::utf8ToPath(INVALID_UTF8);
    {
        std::ofstream out(file);
        if (!out)
        {
            GTEST_SKIP() << "The filesystem refused a file name that isn't UTF-8";
        }
        out << "x";
    }
    EXPECT_EQ(Core::pathToUtf8(file.filename()), INVALID_UTF8);
    EXPECT_TRUE(std::filesystem::exists(Core::utf8ToPath(Core::pathToUtf8(file))));
    EXPECT_EQ(entryNames(dir()), std::vector<std::string>{std::string(INVALID_UTF8)});
}
#endif

} // namespace
