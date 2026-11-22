#include "Core/Application.h"
#include "Core/HeadlessVideoDriverTestUtils.h"
#include "UI/AssetPath.h"
#include "version.h"

#include <gtest/gtest.h>

#include <exception>
#include <filesystem>
#include <optional>
#include <system_error>

#ifndef _WIN32
#include <cstdlib> // std::getenv(), in the Linux display check
#endif

namespace UI
{
namespace
{

// ========== Candidate priority ==========

TEST(AssetPathTest, ReturnsBinAssetsWhenAllCandidatesExist)
{
    // All candidates match: should return candidate 1 (bin/assets)
    const std::filesystem::path exeDir = "/fake/exe";
    const auto result = selectAssetsDir(exeDir, [](const std::filesystem::path& /*p*/) { return true; });
    EXPECT_EQ(result, exeDir / "bin" / "assets");
}

TEST(AssetPathTest, ReturnsBinAssetsWhenExeDirIsBuildRoot)
{
    // Root executable layout: only <exeDir>/bin/assets exists.
    const std::filesystem::path exeDir = "/fake/build-root";
    const std::filesystem::path expected = exeDir / "bin" / "assets";
    const auto result = selectAssetsDir(exeDir, [&](const std::filesystem::path& p) { return p == expected; });
    EXPECT_EQ(result, expected);
}

TEST(AssetPathTest, SkipsFirstAndReturnsSecondCandidateOnMatch)
{
    // Only candidate 2 (portable/build-dir assets) matches
    const std::filesystem::path exeDir = "/fake/prefix";
    const std::filesystem::path expected = exeDir / "assets";
    int callCount = 0;
    const auto result = selectAssetsDir(exeDir,
                                        [&](const std::filesystem::path& p)
                                        {
                                            ++callCount;
                                            return p == expected;
                                        });
    EXPECT_EQ(result, expected);
    EXPECT_EQ(callCount, 2); // candidate 1 was probed and missed
}

TEST(AssetPathTest, ReturnsThirdCandidateForFHSPrefixLayout)
{
    // Only candidate 3 (FHS, binary at prefix root) matches
    const std::filesystem::path exeDir = "/fake/prefix";
    const std::filesystem::path expected = exeDir / TASKSMACK_INSTALL_DATADIR / TASKSMACK_PROJECT_NAME_LOWER / "assets";
    int callCount = 0;
    const auto result = selectAssetsDir(exeDir,
                                        [&](const std::filesystem::path& p)
                                        {
                                            ++callCount;
                                            return p == expected;
                                        });
    EXPECT_EQ(result, expected);
    EXPECT_EQ(callCount, 3); // candidates 1 and 2 were probed and missed
}

TEST(AssetPathTest, ReturnsFourthCandidateForFHSBinLayout)
{
    // Only candidate 4 (FHS, binary in bin/) matches
    // exeDir = /usr/bin → candidate 4 = /usr/bin/../share/<app>/assets → /usr/share/<app>/assets
    const std::filesystem::path exeDir = "/usr/bin";
    const std::filesystem::path expected =
        std::filesystem::path("/usr") / TASKSMACK_INSTALL_DATADIR / TASKSMACK_PROJECT_NAME_LOWER / "assets";
    int callCount = 0;
    const auto result = selectAssetsDir(exeDir,
                                        [&](const std::filesystem::path& p)
                                        {
                                            ++callCount;
                                            return p == expected;
                                        });
    EXPECT_EQ(result, expected);
    EXPECT_EQ(callCount, 4); // candidates 1, 2 and 3 were probed and missed
}

// ========== Fallback behavior ==========

TEST(AssetPathTest, FallsBackToFirstCandidateWhenNoneExist)
{
    // No candidate matches: should return <exeDir>/assets fallback
    const std::filesystem::path exeDir = "/nonexistent/exe";
    const auto result = selectAssetsDir(exeDir, [](const std::filesystem::path& /*p*/) { return false; });
    EXPECT_EQ(result, exeDir / "assets");
}

// ========== findAssetsDir() (real Application, real filesystem) ==========

TEST(AssetPathTest, FindAssetsDirIsStableAcrossCalls)
{
    // findAssetsDir() caches its result in a function-local static (computed once per
    // process), so this is the only way to cover its body and executableDir()'s wrapper
    // around Core::Application::get() -- both need a live Application instance, which
    // selectAssetsDir()'s injectable-predicate tests above deliberately avoid depending on.
    if (!TestSupport::hasGLDisplay())
    {
        GTEST_SKIP() << "No display available (headless environment)";
    }

    Core::ApplicationSpecification spec;
    spec.Name = "FindAssetsDirTest";

    // Isolate construction in its own try/catch so its failure is reported as such. TestSupport::hasGLDisplay()
    // above already confirmed a GL-capable display, so a construction exception is a regression
    // (#1132), just like an exception from findAssetsDir() itself.
    std::optional<Core::Application> app;
    try
    {
        app.emplace(spec);
    }
    catch (const std::exception& e)
    {
        FAIL() << "Application creation failed after the display probe passed: " << e.what();
    }

    const auto first = findAssetsDir();
    const auto second = findAssetsDir();
    EXPECT_EQ(first, second);

    // Comparing against selectAssetsDir() run directly against the live app's real
    // executable directory and the real filesystem (the same "fonts" subdirectory probe
    // findAssetsDir() uses internally) proves the cached result actually resolves from
    // the live application, not just that the cache is internally consistent.
    const auto expected = selectAssetsDir(Core::Application::get().paths().executableDir(),
                                          [](const std::filesystem::path& p)
                                          {
                                              std::error_code ec;
                                              return std::filesystem::exists(p / "fonts", ec);
                                          });
    EXPECT_EQ(first, expected);
}

} // namespace
} // namespace UI
