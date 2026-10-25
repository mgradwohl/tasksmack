/// @file AppTestsMain.cpp
/// @brief main() for TaskSmackAppTests (#1395): the tests run a real Core::Application, whose
/// PathService and UserConfig read TASKSMACK_CONFIG_DIR once per process, on first use. Pointing it at
/// a fresh temporary directory before any test runs means nothing these tests save (ShellLayer's
/// onDetach() writes config.toml) can reach the user's own config. The directory is removed afterwards.
/// A suite whose fixture derives from ScratchConfigDirTest (ScratchConfigDir.h) goes further: each of
/// its tests gets a new, empty directory of its own, so none loads what an earlier one saved (#2932).
/// Every test also fails on a warning or error it didn't expect (LogWarningGuard.h, #2718).

#include "Core/ConfigDirOverride.h"
#include "Core/Utf8Path.h"
#include "LogWarningGuard.h"

#include <SDL3/SDL_stdinc.h>
#include <gtest/gtest.h>

#include <array>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <random>
#include <string>
#include <system_error>
#include <vector>

namespace
{

/// A new, empty directory under the system's temporary directory, or an empty path on failure.
std::filesystem::path makeTempDir()
{
    std::error_code ec;
    const std::filesystem::path base = std::filesystem::temp_directory_path(ec);
    if (ec)
    {
        return {};
    }
    std::random_device random;
    for (int attempt = 0; attempt < 100; ++attempt)
    {
        std::filesystem::path candidate = base / ("tasksmack_app_tests_" + std::to_string(random()));
        if (std::filesystem::create_directory(candidate, ec) && !ec)
        {
            return candidate;
        }
    }
    return {};
}

} // namespace

// The allow-list filter behind the unexpected-warning check (#2718), tested here, where it is used.
namespace TestSupport
{
namespace
{

TEST(LogWarningGuardTest, OnlyLinesNoEntryMatchesAreUnexpected)
{
    constexpr std::array allowed{AllowedWarning{.contains = "synthetic machine", .why = "test"}};
    const std::vector<std::string> logged{
        "warning: TASKSMACK_SYNTHETIC is set: showing a synthetic machine, not this one",
        "error: Couldn't open the GPU device",
        "warning: synthetic machine again",
    };
    EXPECT_EQ(unexpectedWarnings(logged, allowed), (std::vector<std::string>{"error: Couldn't open the GPU device"}));
}

TEST(LogWarningGuardTest, NothingLoggedIsNothingUnexpected)
{
    EXPECT_TRUE(unexpectedWarnings({}, ALLOWED_WARNINGS).empty());
}

TEST(LogWarningGuardTest, OffscreenOnlyEntriesApplyOnlyUnderTheOffscreenDriver)
{
    constexpr std::array allowed{
        AllowedWarning{.contains = "everywhere", .why = "test"},
        AllowedWarning{.contains = "headless", .why = "test", .offscreenOnly = true},
    };
    const std::vector<std::string> logged{"warning: headless"};
    EXPECT_EQ(unexpectedWarnings(logged, applicableWarnings(allowed, false)), logged);
    EXPECT_TRUE(unexpectedWarnings(logged, applicableWarnings(allowed, true)).empty());
    EXPECT_EQ(applicableWarnings(allowed, false).size(), 1U);
}

TEST(LogWarningGuardTest, EveryAllowedEntrySaysWhy)
{
    for (const AllowedWarning& entry : ALLOWED_WARNINGS)
    {
        EXPECT_FALSE(entry.contains.empty());
        EXPECT_FALSE(entry.why.empty()) << entry.contains;
    }
}

} // namespace
} // namespace TestSupport

int main(int argc, char** argv)
{
    try
    {
        ::testing::InitGoogleTest(&argc, argv);
        // A test that logs a warning or error it doesn't expect fails (#2718).
        TestSupport::LogWarningGuard::install();
        const std::filesystem::path configDir = makeTempDir();
        if (configDir.empty())
        {
            std::fputs("TaskSmackAppTests: couldn't make a temporary config directory\n", stderr);
            return 1;
        }
        // SDL's setter: ConfigDirOverride reads it back with SDL_getenv(), which this updates too (UTF-8).
        const std::string name(Core::ConfigDirOverride::ENV_VAR);
        if (SDL_setenv_unsafe(name.c_str(), Core::pathToUtf8(configDir).c_str(), 1) != 0)
        {
            std::fputs("TaskSmackAppTests: couldn't set TASKSMACK_CONFIG_DIR\n", stderr);
            return 1;
        }
        const int result = RUN_ALL_TESTS();
        std::error_code ec;
        std::filesystem::remove_all(configDir, ec);
        return result;
    }
    catch (const std::exception& e)
    {
        std::fputs("TaskSmackAppTests: ", stderr);
        std::fputs(e.what(), stderr);
        std::fputs("\n", stderr);
        return 1;
    }
}
