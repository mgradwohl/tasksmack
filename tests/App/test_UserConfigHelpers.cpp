#include "App/UserConfigHelpers.h"

#include <gtest/gtest.h>
#include <toml++/toml.hpp>

using namespace App;

TEST(UserConfigHelpersTest, LoadAndClampAppliesClamp)
{
    auto tbl = toml::table{};
    tbl.insert_or_assign("metrics", toml::table{});
    tbl["metrics"]["max_sane_rate_bps"] = 12345;

    int dst = 0;
    UserConfigHelpers::loadAndClamp<int>(tbl, "metrics", "max_sane_rate_bps", dst, [](int v) { return std::clamp(v, 0, 20000); });
    EXPECT_EQ(dst, 12345);
}

TEST(UserConfigHelpersTest, LoadAndNarrowInt64NarrowsAndClamps)
{
    auto tbl = toml::table{};
    tbl.insert_or_assign("sampling", toml::table{});
    tbl["sampling"]["interval_ms"] = static_cast<std::int64_t>(5000);

    int dst = 0;
    UserConfigHelpers::loadAndNarrowInt64(tbl, "sampling", "interval_ms", dst, 1000, [](int v) { return Domain::Sampling::clampRefreshInterval(v); });
    EXPECT_EQ(dst, Domain::Sampling::clampRefreshInterval(5000));
}

TEST(UserConfigHelpersTest, LoadAndNarrowIntUsesDefaultWhenMissing)
{
    auto tbl = toml::table{};
    int dst = 0;
    // Key missing -> destination unchanged
    UserConfigHelpers::loadAndNarrowInt(tbl, "window", "width", dst, 800, 200, 16000);
    EXPECT_EQ(dst, 0);
}
