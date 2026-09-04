/// @file test_MonospaceFontPath.cpp
/// @brief Tests for UI::selectMonospaceFontPath(), extracted from UILayer.cpp's
/// getMonospaceFontPath() (#770) so the "probe candidates in order" logic is testable with an
/// injectable existence predicate, mirroring UI::selectAssetsDir() (see test_AssetPath.cpp).

#include "UI/MonospaceFontPath.h"

#include <gtest/gtest.h>

#include <array>
#include <filesystem>

namespace UI
{
namespace
{

TEST(MonospaceFontPathTest, ReturnsFirstCandidateWhenAllExist)
{
    const std::array<std::filesystem::path, 2> candidates = {"/fonts/first.ttf", "/fonts/second.ttf"};
    const auto result = selectMonospaceFontPath(candidates, [](const std::filesystem::path& /*p*/) { return true; });
    EXPECT_EQ(result, candidates[0]);
}

TEST(MonospaceFontPathTest, SkipsFirstAndReturnsSecondCandidateOnMatch)
{
    const std::array<std::filesystem::path, 2> candidates = {"/fonts/first.ttf", "/fonts/second.ttf"};
    const auto result = selectMonospaceFontPath(candidates, [&](const std::filesystem::path& p) { return p == candidates[1]; });
    EXPECT_EQ(result, candidates[1]);
}

TEST(MonospaceFontPathTest, ReturnsEmptyPathWhenNoCandidateExists)
{
    const std::array<std::filesystem::path, 2> candidates = {"/fonts/first.ttf", "/fonts/second.ttf"};
    const auto result = selectMonospaceFontPath(candidates, [](const std::filesystem::path& /*p*/) { return false; });
    EXPECT_TRUE(result.empty());
}

TEST(MonospaceFontPathTest, EmptyCandidateListReturnsEmptyPath)
{
    const std::array<std::filesystem::path, 0> candidates{};
    const auto result = selectMonospaceFontPath(candidates, [](const std::filesystem::path& /*p*/) { return true; });
    EXPECT_TRUE(result.empty());
}

TEST(MonospaceFontPathTest, FindMonospaceFontPathDoesNotThrow)
{
    // findMonospaceFontPath() probes the real filesystem; it may or may not find a font on
    // this machine, but it must never throw regardless of what's installed.
    EXPECT_NO_THROW({ [[maybe_unused]] auto result = findMonospaceFontPath(); });
}

} // namespace
} // namespace UI
