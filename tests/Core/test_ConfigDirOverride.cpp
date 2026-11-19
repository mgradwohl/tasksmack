/// @file test_ConfigDirOverride.cpp
/// @brief The TASKSMACK_CONFIG_DIR parser and resolver (#1596)

#include "Core/ConfigDirOverride.h"
#include "Core/Utf8Path.h"
#include "Mocks/HandedOverTestFolder.h"
#include "NonAsciiTestNames.h"
#include "Platform/IHandedOverFolder.h"

#include <SDL3/SDL_stdinc.h>
#include <gtest/gtest.h>
#include <spdlog/logger.h>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>

#include <chrono>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace Core::ConfigDirOverride
{
namespace
{

TEST(ConfigDirOverrideTest, UnsetIsNoOverride)
{
    EXPECT_FALSE(parse(nullptr).has_value());
}

TEST(ConfigDirOverrideTest, BlankIsNoOverride)
{
    EXPECT_FALSE(parse("").has_value());
    EXPECT_FALSE(parse("   ").has_value());
    EXPECT_FALSE(parse("\t\r\n ").has_value());
}

TEST(ConfigDirOverrideTest, ValueIsTheDirectory)
{
    const auto dir = parse("tasksmack-test/run1");
    ASSERT_TRUE(dir.has_value());
    EXPECT_EQ(dir.value_or(std::filesystem::path{}), std::filesystem::path("tasksmack-test/run1"));
}

TEST(ConfigDirOverrideTest, SurroundingWhitespaceIsTrimmed)
{
    const auto dir = parse("  tasksmack-test \t");
    ASSERT_TRUE(dir.has_value());
    EXPECT_EQ(dir.value_or(std::filesystem::path{}), std::filesystem::path("tasksmack-test"));
}

TEST(ConfigDirOverrideTest, ValueIsReadAsUtf8)
{
    // SDL_getenv() returns UTF-8 on every platform; "tëst" must not go through the ANSI code page.
    const auto dir = parse("t\xC3\xABst");
    ASSERT_TRUE(dir.has_value());
    EXPECT_EQ(dir.value_or(std::filesystem::path{}), std::filesystem::path(u8"tëst"));
}

TEST(ConfigDirOverrideTest, ResolveUsesTheOverrideWhenSet)
{
    const std::filesystem::path platformDir = std::filesystem::temp_directory_path() / "platform";
    const std::filesystem::path overrideDir = std::filesystem::temp_directory_path() / "override";
    EXPECT_EQ(resolve(overrideDir, platformDir), overrideDir);
}

TEST(ConfigDirOverrideTest, ResolveKeepsThePlatformDirWhenUnset)
{
    const std::filesystem::path platformDir = std::filesystem::temp_directory_path() / "platform";
    EXPECT_EQ(resolve(std::nullopt, platformDir), platformDir);
    EXPECT_EQ(resolve(parse("  "), platformDir), platformDir);
}

// #2728: an elevated restart's --config-dir is the launching user's directory, used as the override is, but it
// is no test hook, so the Administrator session isn't run as an unattended capture.
TEST(ConfigDirOverrideTest, AHandedOverDirectoryIsUsedButIsNoTestHook)
{
    // NOLINTNEXTLINE(performance-unnecessary-copy-initialization) - a copy: active() changes below
    const std::optional<std::filesystem::path> previous = active();
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "handed-over";

    adoptHandedOver(dir);
    ASSERT_TRUE(active().has_value());
    EXPECT_EQ(active().value_or(std::filesystem::path{}), dir.lexically_normal());
    EXPECT_FALSE(fromTestHook());

    setActiveForTesting(dir);
    EXPECT_TRUE(fromTestHook()) << "an override of a test's own is a test hook again";
    setActiveForTesting(std::nullopt);
    EXPECT_FALSE(fromTestHook()) << "no override at all";

    setActiveForTesting(previous);
}

// #3759: a TaskSmack run as root for a user keeps that user's config folder, the less-privileged side's, so it counts as
// handed over, though no override names it; a test's own override starts clean again.
TEST(ConfigDirOverrideTest, ARootSessionsInvokingUsersFolderCountsAsHandedOver)
{
    // NOLINTNEXTLINE(performance-unnecessary-copy-initialization) - a copy: active() changes below
    const std::optional<std::filesystem::path> previous = active();
    setActiveForTesting(std::nullopt);
    EXPECT_FALSE(handedOver());

    markLessPrivilegedFolder();
    EXPECT_TRUE(handedOver());
    EXPECT_FALSE(active().has_value()) << "the folder is still the one the platform picks";
    EXPECT_FALSE(fromTestHook());

    EXPECT_FALSE(handedOverFolderReleased()) << "a root session holds no folder, and doesn't fail closed for it";

    setActiveForTesting(previous);
    EXPECT_FALSE(handedOver()) << "a test's override starts with nothing handed over";
}

// #3787: only a weak reference to the held folder is kept, so once its owner (main.cpp) lets it go, nothing here
// keeps it, and the hand-off says it is no longer held.
TEST(ConfigDirOverrideTest, TheHeldFolderIsOnlyEverTheOwnersToKeep)
{
    // NOLINTNEXTLINE(performance-unnecessary-copy-initialization) - a copy: active() changes below
    const std::optional<std::filesystem::path> previous = active();
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "handed-over";
    auto folder = std::make_shared<TestSupport::HandedOverTestFolder>(dir);

    adoptHandedOver(dir, folder);
    EXPECT_TRUE(handedOver());
    EXPECT_EQ(handedOverFolder(), folder);
    EXPECT_EQ(folder.use_count(), 1) << "held by its owner alone";
    EXPECT_FALSE(handedOverFolderReleased());

    folder.reset();
    EXPECT_TRUE(handedOver()) << "still a hand-off, which fails closed";
    EXPECT_EQ(handedOverFolder(), nullptr);
    EXPECT_TRUE(handedOverFolderReleased());

    adoptHandedOver(dir);
    EXPECT_EQ(handedOverFolder(), nullptr) << "handed over with no held folder at all";
    EXPECT_TRUE(handedOverFolderReleased());
    setActiveForTesting(previous);
    EXPECT_FALSE(handedOver());
    EXPECT_EQ(handedOverFolder(), nullptr);
    EXPECT_FALSE(handedOverFolderReleased());
}

// #3774: a handed-over folder named with a trailing separator is the same folder as without one, to ConfigDirOverride
// and to the held folder alike, so UserConfig's comparison of the two doesn't refuse every save.
TEST(ConfigDirOverrideTest, AHandedOverFolderIsKeptWithoutATrailingSeparator)
{
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "handed-over";
    std::filesystem::path withSeparator = dir;
    withSeparator += std::filesystem::path::preferred_separator;
    EXPECT_EQ(Platform::handedOverFolderPath(withSeparator), Platform::handedOverFolderPath(dir));
    EXPECT_TRUE(Platform::handedOverFolderPath(withSeparator).has_filename());
    EXPECT_EQ(Platform::handedOverFolderPath(dir.root_path()), dir.root_path()) << "a root keeps its separator";

    // NOLINTNEXTLINE(performance-unnecessary-copy-initialization) - a copy: active() changes below
    const std::optional<std::filesystem::path> previous = active();
    adoptHandedOver(withSeparator);
    EXPECT_EQ(active().value_or(std::filesystem::path{}), Platform::handedOverFolderPath(dir));
    EXPECT_EQ(active().value_or(std::filesystem::path{}) / "config.toml", Platform::handedOverFolderPath(dir) / "config.toml");
    setActiveForTesting(previous);
}

/// In a death test's own child: TASKSMACK_CONFIG_DIR set to @p envDir, then a hand-off of @p handedDir, before
/// anything reads active(). Exits 0 when active() is the hand-off and nothing made @p envDir.
[[noreturn]] void handOffInChild(const std::filesystem::path& envDir, const std::filesystem::path& handedDir)
{
    const auto utf8 = envDir.u8string();
    static_cast<void>(SDL_setenv_unsafe(std::string(ENV_VAR).c_str(), std::string(utf8.begin(), utf8.end()).c_str(), 1));
    adoptHandedOver(handedDir);
    const bool handOffUsed =
        active() == std::optional<std::filesystem::path>(handedDir.lexically_normal()) && handedOver() && !fromTestHook();
    std::error_code ec;
    const bool envDirUntouched = !std::filesystem::exists(envDir, ec) && !ec;
    std::exit(handOffUsed && envDirUntouched ? 0 : 1); // NOLINT(concurrency-mt-unsafe) - the death test's own child
}

// #3780: a hand-off session never reads TASKSMACK_CONFIG_DIR, so the directory it names is never made, as an
// elevated TaskSmack would make it. Run in a child process (the threadsafe style re-executes this test), where
// nothing has read active() yet: in this one, earlier tests have.
TEST(ConfigDirOverrideDeathTest, AHandOffNeverTouchesTheTestHookDirectory)
{
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    const std::filesystem::path temp = std::filesystem::temp_directory_path();
    const std::filesystem::path envDir =
        temp / std::format("TaskSmack.HandOffEnvDir.{}", std::chrono::steady_clock::now().time_since_epoch().count());
    const std::filesystem::path handedDir = temp / "TaskSmack.HandedOver";
    EXPECT_EXIT(handOffInChild(envDir, handedDir), ::testing::ExitedWithCode(0), "");
    std::error_code ec;
    EXPECT_FALSE(std::filesystem::exists(envDir, ec));
}

/// In a death test's own child: TASKSMACK_CONFIG_DIR set to @p envDir, then the test hook ignored, as an elevated
/// session does, with no hand-off. Exits 0 when there is no override, it is said to be ignored, and nothing made
/// @p envDir.
[[noreturn]] void elevatedInChild(const std::filesystem::path& envDir)
{
    const auto utf8 = envDir.u8string();
    static_cast<void>(SDL_setenv_unsafe(std::string(ENV_VAR).c_str(), std::string(utf8.begin(), utf8.end()).c_str(), 1));
    ignoreTestHook();
    const bool noOverride = !active().has_value() && !fromTestHook() && !handedOver();
    const bool saidIgnored = testHookIgnored();
    std::error_code ec;
    const bool envDirUntouched = !std::filesystem::exists(envDir, ec) && !ec;
    std::exit(noOverride && saidIgnored && envDirUntouched ? 0 : 1); // NOLINT(concurrency-mt-unsafe) - the death test's own child
}

/// As elevatedInChild(), but with a hand-off adopted too: it is used, and the test hook still neither read nor made.
[[noreturn]] void elevatedHandOffInChild(const std::filesystem::path& envDir, const std::filesystem::path& handedDir)
{
    const auto utf8 = envDir.u8string();
    static_cast<void>(SDL_setenv_unsafe(std::string(ENV_VAR).c_str(), std::string(utf8.begin(), utf8.end()).c_str(), 1));
    ignoreTestHook();
    adoptHandedOver(handedDir);
    const bool handOffUsed = active() == std::optional<std::filesystem::path>(handedDir.lexically_normal()) && !fromTestHook();
    std::error_code ec;
    const bool envDirUntouched = !std::filesystem::exists(envDir, ec) && !ec;
    std::exit(handOffUsed && testHookIgnored() && envDirUntouched ? 0 : 1); // NOLINT(concurrency-mt-unsafe) - the death test's own child
}

// #3920: an elevated session that adopts no hand-off (one refused, or a launch as administrator) takes no folder from
// TASKSMACK_CONFIG_DIR and makes none, and says the hook was ignored. In a child, where nothing has read active() yet.
TEST(ConfigDirOverrideDeathTest, AnElevatedSessionIgnoresTheTestHook)
{
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    const std::filesystem::path temp = std::filesystem::temp_directory_path();
    const std::filesystem::path envDir =
        temp / std::format("TaskSmack.ElevatedEnvDir.{}", std::chrono::steady_clock::now().time_since_epoch().count());
    EXPECT_EXIT(elevatedInChild(envDir), ::testing::ExitedWithCode(0), "");
    EXPECT_EXIT(elevatedHandOffInChild(envDir, temp / "TaskSmack.HandedOver"), ::testing::ExitedWithCode(0), "");
    std::error_code ec;
    EXPECT_FALSE(std::filesystem::exists(envDir, ec));
}

// #3920: unelevated (nothing ignored), the hook isn't said to be ignored, whatever the environment holds.
TEST(ConfigDirOverrideTest, NothingIsIgnoredUnlessTheSessionIsElevated)
{
    EXPECT_FALSE(testHookIgnored());
}

// ========== readHandedOverFiles() (#4094): the user themes of a hand-off, read through the folder held ==========

/// A handed-over folder whose listing and reads a test sets: @p listing is what listFiles() answers (or @p listError,
/// when set), and each name in @p reads what readFile() answers for it; a name it doesn't hold reads as not there.
/// Records what was asked for, so a test can tell what was read.
class ScriptedHandedOverFolder final : public Platform::IHandedOverFolder
{
  public:
    using ReadResult = std::expected<std::optional<std::string>, std::string>;

    ScriptedHandedOverFolder(std::filesystem::path path,
                             std::vector<std::filesystem::path> listing,
                             std::map<std::filesystem::path, ReadResult> reads,
                             std::optional<std::string> listError = std::nullopt)
        : m_Path(std::move(path)), m_Listing(std::move(listing)), m_Reads(std::move(reads)), m_ListError(std::move(listError))
    {}

    [[nodiscard]] const std::filesystem::path& path() const noexcept override
    {
        return m_Path;
    }

    [[nodiscard]] std::expected<void*, std::string> openLockFile(const std::filesystem::path& /*name*/) const override
    {
        return std::unexpected(std::string("a scripted folder holds no lock"));
    }

    [[nodiscard]] ReadResult readFile(const std::filesystem::path& relative) const override
    {
        m_Read.push_back(relative);
        const auto it = m_Reads.find(relative);
        return it != m_Reads.end() ? it->second : ReadResult(std::optional<std::string>{});
    }

    [[nodiscard]] std::expected<void, std::string> replaceFile(const std::filesystem::path& /*name*/,
                                                               std::string_view /*contents*/) const override
    {
        return std::unexpected(std::string("a scripted folder is read-only"));
    }

    [[nodiscard]] std::expected<void, std::string> createFolder(const std::filesystem::path& /*name*/) const override
    {
        return std::unexpected(std::string("a scripted folder is read-only"));
    }

    [[nodiscard]] std::expected<std::vector<std::filesystem::path>, std::string> listFiles(const std::filesystem::path& name) const override
    {
        m_Listed.push_back(name);
        if (m_ListError.has_value())
        {
            return std::unexpected(*m_ListError);
        }
        return m_Listing;
    }

    /// The names readFile() was asked for, in order.
    [[nodiscard]] const std::vector<std::filesystem::path>& read() const noexcept
    {
        return m_Read;
    }

    /// The folders listFiles() was asked for, in order.
    [[nodiscard]] const std::vector<std::filesystem::path>& listed() const noexcept
    {
        return m_Listed;
    }

  private:
    std::filesystem::path m_Path;
    std::vector<std::filesystem::path> m_Listing;
    std::map<std::filesystem::path, ReadResult> m_Reads;
    std::optional<std::string> m_ListError;
    mutable std::vector<std::filesystem::path> m_Read;
    mutable std::vector<std::filesystem::path> m_Listed;
};

/// Hands @p folder over for the test's length, with spdlog's default logger captured, and puts both back after.
class ReadHandedOverFilesTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Previous = active();
        m_PreviousLogger = spdlog::default_logger();
        spdlog::set_default_logger(
            std::make_shared<spdlog::logger>("handed-over-files-test", std::make_shared<spdlog::sinks::ostream_sink_st>(m_Log)));
    }

    void TearDown() override
    {
        spdlog::set_default_logger(m_PreviousLogger);
        setActiveForTesting(m_Previous);
    }

    /// The folder handed over, as main.cpp hands one over: absolute, held by the test.
    [[nodiscard]] static std::filesystem::path handedDir()
    {
        return Platform::handedOverFolderPath(std::filesystem::temp_directory_path() / "handed-over-files");
    }

    /// What was logged so far.
    [[nodiscard]] std::string log() const
    {
        return m_Log.str();
    }

  private:
    std::optional<std::filesystem::path> m_Previous;
    std::shared_ptr<spdlog::logger> m_PreviousLogger;
    std::ostringstream m_Log;
};

TEST_F(ReadHandedOverFilesTest, NothingHandedOverReadsNothingThroughAHeldFolder)
{
    setActiveForTesting(std::nullopt);
    EXPECT_FALSE(readHandedOverFiles("themes", ".toml").has_value()) << "read by path then, if at all";

    setActiveForTesting(handedDir());
    EXPECT_FALSE(readHandedOverFiles("themes", ".toml").has_value()) << "a test hook's folder is no hand-off";

    markLessPrivilegedFolder();
    EXPECT_FALSE(readHandedOverFiles("themes", ".toml").has_value()) << "a root session for a user holds no folder";
}

TEST_F(ReadHandedOverFilesTest, AHandOffWhoseFolderIsNoLongerHeldFailsClosed)
{
    auto folder = std::make_shared<ScriptedHandedOverFolder>(handedDir(),
                                                             std::vector<std::filesystem::path>{"mine.toml"},
                                                             std::map<std::filesystem::path, ScriptedHandedOverFolder::ReadResult>{});
    adoptHandedOver(handedDir(), folder);
    folder.reset();

    const auto files = readHandedOverFiles("themes", ".toml");
    ASSERT_TRUE(files.has_value());
    ASSERT_FALSE(files->has_value()) << "read by path instead";
    EXPECT_TRUE(files->error().contains("isn't held")) << files->error();

    adoptHandedOver(handedDir());
    const auto neverHeld = readHandedOverFiles("themes", ".toml");
    ASSERT_TRUE(neverHeld.has_value());
    EXPECT_FALSE(neverHeld->has_value()) << "handed over with no held folder at all";
}

TEST_F(ReadHandedOverFilesTest, OnlyFilesWithTheExtensionAreReadInTheOrderListed)
{
    const std::filesystem::path themes = "themes";
    const auto folder = std::make_shared<ScriptedHandedOverFolder>(
        handedDir(),
        std::vector<std::filesystem::path>{"b.toml", "notes.txt", "a.toml", "backup.toml.bak", "toml", ".toml", "c.tom"},
        std::map<std::filesystem::path, ScriptedHandedOverFolder::ReadResult>{
            {themes / "a.toml", std::optional<std::string>("[meta]\nname = \"A\"\n")},
            {themes / "b.toml", std::optional<std::string>("[meta]\nname = \"B\"\n")},
        });
    adoptHandedOver(handedDir(), folder);

    const auto files = readHandedOverFiles(themes, ".toml");
    ASSERT_TRUE(files.has_value());
    ASSERT_TRUE(files->has_value()) << files->error();
    ASSERT_EQ((*files)->size(), 2U);
    EXPECT_EQ((**files)[0].path, handedDir() / themes / "b.toml") << "the full path, for what is logged and shown";
    EXPECT_EQ((**files)[0].text, "[meta]\nname = \"B\"\n");
    EXPECT_EQ((**files)[1].path, handedDir() / themes / "a.toml");
    EXPECT_EQ((**files)[1].text, "[meta]\nname = \"A\"\n");
    EXPECT_EQ(folder->listed(), std::vector<std::filesystem::path>{themes});
    EXPECT_EQ(folder->read(), (std::vector<std::filesystem::path>{themes / "b.toml", themes / "a.toml"})) << "nothing else is even opened";
}

TEST_F(ReadHandedOverFilesTest, AnEmptyFolderIsNoFiles)
{
    const auto folder = std::make_shared<ScriptedHandedOverFolder>(
        handedDir(), std::vector<std::filesystem::path>{}, std::map<std::filesystem::path, ScriptedHandedOverFolder::ReadResult>{});
    adoptHandedOver(handedDir(), folder);

    const auto files = readHandedOverFiles("themes", ".toml");
    ASSERT_TRUE(files.has_value());
    ASSERT_TRUE(files->has_value()) << files->error();
    EXPECT_TRUE((*files)->empty());
}

// The folder on disk, through HandedOverTestFolder: a themes folder that isn't there is no themes, not an error.
TEST_F(ReadHandedOverFilesTest, AMissingFolderIsNoFiles)
{
    const std::filesystem::path dir = Platform::handedOverFolderPath(
        std::filesystem::temp_directory_path() /
        std::format("tasksmack-handed-over-missing-{}", std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    ASSERT_FALSE(ec) << ec.message();
    const auto folder = std::make_shared<TestSupport::HandedOverTestFolder>(dir);
    adoptHandedOver(dir, folder);

    const auto files = readHandedOverFiles("themes", ".toml");
    ASSERT_TRUE(files.has_value());
    ASSERT_TRUE(files->has_value()) << files->error();
    EXPECT_TRUE((*files)->empty());
    std::filesystem::remove_all(dir, ec);
}

TEST_F(ReadHandedOverFilesTest, AFolderThatCantBeListedIsAnError)
{
    const auto folder = std::make_shared<ScriptedHandedOverFolder>(handedDir(),
                                                                   std::vector<std::filesystem::path>{},
                                                                   std::map<std::filesystem::path, ScriptedHandedOverFolder::ReadResult>{},
                                                                   std::string("themes is a link, junction or mount point"));
    adoptHandedOver(handedDir(), folder);

    const auto files = readHandedOverFiles("themes", ".toml");
    ASSERT_TRUE(files.has_value());
    ASSERT_FALSE(files->has_value());
    EXPECT_EQ(files->error(), "themes is a link, junction or mount point") << "the folder's own reason";
}

// A file the held folder refuses (a link, a hard link, one too large) is skipped, saying which and why; the rest are
// still read. One gone since the listing is skipped quietly.
TEST_F(ReadHandedOverFilesTest, AFileThatCantBeReadIsSkippedAndLoggedAndTheRestAreRead)
{
    const std::filesystem::path themes = "themes";
    const auto folder =
        std::make_shared<ScriptedHandedOverFolder>(handedDir(),
                                                   std::vector<std::filesystem::path>{"gone.toml", "linked.toml", "mine.toml"},
                                                   std::map<std::filesystem::path, ScriptedHandedOverFolder::ReadResult>{
                                                       {themes / "linked.toml", std::unexpected(std::string("it is hard-linked"))},
                                                       {themes / "mine.toml", std::optional<std::string>("[meta]\nname = \"Mine\"\n")},
                                                   });
    adoptHandedOver(handedDir(), folder);

    const auto files = readHandedOverFiles(themes, ".toml");
    ASSERT_TRUE(files.has_value());
    ASSERT_TRUE(files->has_value()) << files->error();
    ASSERT_EQ((*files)->size(), 1U);
    EXPECT_EQ((**files)[0].path, handedDir() / themes / "mine.toml");

    const std::string logged = log();
    EXPECT_TRUE(logged.contains("Skipping")) << logged;
    EXPECT_TRUE(logged.contains(Core::pathToUtf8(handedDir() / themes / "linked.toml"))) << logged;
    EXPECT_TRUE(logged.contains("it is hard-linked")) << logged;
    EXPECT_FALSE(logged.contains("gone.toml")) << "a file gone since the listing isn't worth a warning: " << logged;
}

#ifdef _WIN32
// #4316: a relative TASKSMACK_CONFIG_DIR is made absolute from the working directory, and the text logged for it was
// u8string(), which throws for a lone surrogate: out of active()'s static initialiser, so out of startup. The path is
// kept exactly, and only the logged text shows U+FFFD.
TEST(ConfigDirOverrideTest, ARelativeValueUnderALoneSurrogateFolderIsTakenExactlyAndNeverThrows)
{
    const TestSupport::LoneSurrogateTempDir dir;
    if (!dir.created())
    {
        GTEST_SKIP() << "the filesystem refused a lone surrogate in a folder name";
    }
    std::error_code ec;
    const std::filesystem::path previousCwd = std::filesystem::current_path(ec);
    ASSERT_FALSE(ec) << ec.message();
    std::filesystem::current_path(dir.path(), ec);
    ASSERT_FALSE(ec) << ec.message();
    struct RestoreCwd
    {
        std::filesystem::path path;
        explicit RestoreCwd(std::filesystem::path previous) : path(std::move(previous))
        {}
        RestoreCwd(const RestoreCwd&) = delete;
        RestoreCwd& operator=(const RestoreCwd&) = delete;
        RestoreCwd(RestoreCwd&&) = delete;
        RestoreCwd& operator=(RestoreCwd&&) = delete;
        ~RestoreCwd()
        {
            std::error_code restoreError;
            std::filesystem::current_path(path, restoreError);
        }
    };
    const RestoreCwd restore{previousCwd};

    std::optional<std::filesystem::path> taken;
    ASSERT_NO_THROW(taken = takeTestHookValue("cfg"));
    ASSERT_TRUE(taken.has_value());
    EXPECT_EQ(taken.value_or(std::filesystem::path{}).native(), (dir.path() / "cfg").lexically_normal().native())
        << "the exact folder, lone surrogate and all";
    EXPECT_TRUE(std::filesystem::is_directory(dir.path() / "cfg"));

    // A directory that can't be made (a file is in the way) is logged, by its display text.
    {
        const std::ofstream blocker(dir.path() / "blocker");
    }
    std::ostringstream logStream;
    const auto previousLogger = spdlog::default_logger();
    spdlog::set_default_logger(std::make_shared<spdlog::logger>("capture", std::make_shared<spdlog::sinks::ostream_sink_st>(logStream)));
    ASSERT_NO_THROW(taken = takeTestHookValue("blocker/cfg"));
    spdlog::set_default_logger(previousLogger);
    ASSERT_TRUE(taken.has_value());
    EXPECT_EQ(taken.value_or(std::filesystem::path{}).native(), (dir.path() / "blocker" / "cfg").lexically_normal().native());
    const std::string logged = logStream.str();
    EXPECT_TRUE(logged.contains("could not create")) << logged;
    EXPECT_TRUE(logged.contains("\xEF\xBF\xBD")) << "the lone surrogate shown as U+FFFD: " << logged;
}
#endif

} // namespace
} // namespace Core::ConfigDirOverride
