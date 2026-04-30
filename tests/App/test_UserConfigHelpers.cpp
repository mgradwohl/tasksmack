// NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers, readability-magic-numbers)

#include "App/UserConfigHelpers.h"
#include "Domain/SamplingConfig.h"

#include <gtest/gtest.h>

#include <toml++/toml.hpp>

namespace App
{
namespace
{

// ========== Helper Function Tests ==========

TEST(UserConfigHelpersTest, LoadAndClampAppliesClamp)
{
    // In-range value should be preserved
    {
        auto metrics = toml::table{};
        metrics.insert_or_assign("max_sane_rate_bps", 12345);

        auto tbl = toml::table{};
        tbl.insert_or_assign("metrics", std::move(metrics));

        int dst = 0;
        App::UserConfigHelpers::loadAndClamp<int>(tbl, "metrics", "max_sane_rate_bps", dst, [](int v) { return std::clamp(v, 0, 20000); });
        EXPECT_EQ(dst, 12345);
    }

    // Below-min should clamp to 0
    {
        auto metrics = toml::table{};
        metrics.insert_or_assign("max_sane_rate_bps", -100);

        auto tbl = toml::table{};
        tbl.insert_or_assign("metrics", std::move(metrics));

        int dst = 0;
        App::UserConfigHelpers::loadAndClamp<int>(tbl, "metrics", "max_sane_rate_bps", dst, [](int v) { return std::clamp(v, 0, 20000); });
        EXPECT_EQ(dst, 0);
    }

    // Above-max should clamp to 20000
    {
        auto metrics = toml::table{};
        metrics.insert_or_assign("max_sane_rate_bps", 25000);

        auto tbl = toml::table{};
        tbl.insert_or_assign("metrics", std::move(metrics));

        int dst = 0;
        App::UserConfigHelpers::loadAndClamp<int>(tbl, "metrics", "max_sane_rate_bps", dst, [](int v) { return std::clamp(v, 0, 20000); });
        EXPECT_EQ(dst, 20000);
    }
}

TEST(UserConfigHelpersTest, LoadAndNarrowInt64NarrowsAndClamps)
{
    auto sampling = toml::table{};
    sampling.insert_or_assign("interval_ms", static_cast<std::int64_t>(5000));

    auto tbl = toml::table{};
    tbl.insert_or_assign("sampling", std::move(sampling));

    int dst = 0;
    App::UserConfigHelpers::loadAndNarrowInt64(
        tbl, "sampling", "interval_ms", dst, 1000, [](int v) { return Domain::Sampling::clampRefreshInterval(v); });
    EXPECT_EQ(dst, static_cast<int>(Domain::Sampling::clampRefreshInterval(5000)));
}

TEST(UserConfigHelpersTest, LoadAndNarrowIntWithClampUsesDefaultWhenMissing)
{
    auto tbl = toml::table{};
    int dst = 999;
    // Key missing -> destination unchanged
    App::UserConfigHelpers::loadAndNarrowIntWithClamp(tbl, "window", "width", dst, 800, 200, 16000);
    EXPECT_EQ(dst, 999);
}

// ========== isValidConfigDir ==========

TEST(UserConfigHelpersTest, IsValidConfigDirAcceptsAbsolutePath)
{
    EXPECT_TRUE(App::UserConfigHelpers::isValidConfigDir(std::filesystem::path("/home/user/.config/tasksmack")));
}

TEST(UserConfigHelpersTest, IsValidConfigDirAcceptsAbsolutePathWithoutTraversal)
{
    EXPECT_TRUE(App::UserConfigHelpers::isValidConfigDir(std::filesystem::path("/tmp/tasksmack")));
}

TEST(UserConfigHelpersTest, IsValidConfigDirRejectsRelativePath)
{
    EXPECT_FALSE(App::UserConfigHelpers::isValidConfigDir(std::filesystem::path("relative/path")));
}

TEST(UserConfigHelpersTest, IsValidConfigDirRejectsDotDotTraversal)
{
    // A relative path with traversal components is rejected because it is not absolute.
    EXPECT_FALSE(App::UserConfigHelpers::isValidConfigDir(std::filesystem::path("../../etc/passwd")));
}

TEST(UserConfigHelpersTest, IsValidConfigDirNormalizesAbsoluteTraversal)
{
    // An absolute path with ".." is lexically normalized (e.g. /home/user/../../etc/passwd → /etc/passwd).
    // The normalized form is still an absolute path with no ".." components, so it is accepted.
    // Restricting WHICH absolute paths are allowed is a caller responsibility (e.g. checking the path
    // is under the expected config root).
    EXPECT_TRUE(App::UserConfigHelpers::isValidConfigDir(std::filesystem::path("/home/user/../../etc/passwd")));
}

TEST(UserConfigHelpersTest, IsValidConfigDirRejectsBareTraversal)
{
    EXPECT_FALSE(App::UserConfigHelpers::isValidConfigDir(std::filesystem::path("../../etc/shadow")));
}

TEST(UserConfigHelpersTest, IsValidConfigDirAcceptsRootPath)
{
    EXPECT_TRUE(App::UserConfigHelpers::isValidConfigDir(std::filesystem::path("/")));
}

TEST(UserConfigHelpersTest, IsValidConfigDirRejectsEmptyPath)
{
    // Empty path is not absolute.
    EXPECT_FALSE(App::UserConfigHelpers::isValidConfigDir(std::filesystem::path("")));
}

} // namespace
} // namespace App

// NOLINTEND(cppcoreguidelines-avoid-magic-numbers, readability-magic-numbers)
