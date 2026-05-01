#include "UI/AssetPath.h"
#include "version.h"

#include <gtest/gtest.h>

#include <filesystem>

namespace UI
{
namespace
{

// ========== Candidate priority ==========

TEST(AssetPathTest, ReturnsFirstCandidateWhenAllExist)
{
    // All candidates match: should return candidate 1 (build-dir layout)
    const std::filesystem::path exeDir = "/fake/exe";
    const auto result = selectAssetsDir(exeDir, [](const std::filesystem::path& /*p*/) { return true; });
    EXPECT_EQ(result, exeDir / "assets");
}

TEST(AssetPathTest, SkipsFirstAndReturnsSecondCandidateOnMatch)
{
    // Only candidate 2 (FHS, binary at prefix root) matches
    const std::filesystem::path exeDir = "/fake/prefix";
    const std::filesystem::path expected = exeDir / "share" / TASKSMACK_PROJECT_NAME_LOWER / "assets";
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

TEST(AssetPathTest, ReturnsThirdCandidateForFHSBinLayout)
{
    // Only candidate 3 (FHS, binary in bin/) matches
    // exeDir = /usr/bin → candidate 3 = /usr/bin/../share/<app>/assets → /usr/share/<app>/assets
    const std::filesystem::path exeDir = "/usr/bin";
    const std::filesystem::path expected = std::filesystem::path("/usr") / "share" / TASKSMACK_PROJECT_NAME_LOWER / "assets";
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

// ========== Fallback behavior ==========

TEST(AssetPathTest, FallsBackToFirstCandidateWhenNoneExist)
{
    // No candidate matches: should return candidate 1 as fallback
    const std::filesystem::path exeDir = "/nonexistent/exe";
    const auto result = selectAssetsDir(exeDir, [](const std::filesystem::path& /*p*/) { return false; });
    EXPECT_EQ(result, exeDir / "assets");
}

} // namespace
} // namespace UI
