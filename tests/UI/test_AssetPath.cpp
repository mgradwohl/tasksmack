#include "UI/AssetPath.h"
#include "version.h"

#include <gtest/gtest.h>

#include <filesystem>

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

} // namespace
} // namespace UI
