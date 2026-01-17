#include "App/UserConfigHelpers.h"
#include "Domain/SamplingConfig.h"

#include <gtest/gtest.h>

#include <toml++/toml.hpp>

using namespace App;

TEST(UserConfigHelpersTest, LoadAndClampAppliesClamp)
{
    // In-range value should be preserved
    {
        auto metrics = toml::table{};
        metrics.insert_or_assign("max_sane_rate_bps", 12345);

        auto tbl = toml::table{};
        tbl.insert_or_assign("metrics", std::move(metrics));

        int dst = 0;
        UserConfigHelpers::loadAndClamp<int>(tbl, "metrics", "max_sane_rate_bps", dst, [](int v) { return std::clamp(v, 0, 20000); });
        EXPECT_EQ(dst, 12345);
    }

    // Below-min should clamp to 0
    {
        auto metrics = toml::table{};
        metrics.insert_or_assign("max_sane_rate_bps", -100);

        auto tbl = toml::table{};
        tbl.insert_or_assign("metrics", std::move(metrics));

        int dst = 0;
        UserConfigHelpers::loadAndClamp<int>(tbl, "metrics", "max_sane_rate_bps", dst, [](int v) { return std::clamp(v, 0, 20000); });
        EXPECT_EQ(dst, 0);
    }

    // Above-max should clamp to 20000
    {
        auto metrics = toml::table{};
        metrics.insert_or_assign("max_sane_rate_bps", 25000);

        auto tbl = toml::table{};
        tbl.insert_or_assign("metrics", std::move(metrics));

        int dst = 0;
        UserConfigHelpers::loadAndClamp<int>(tbl, "metrics", "max_sane_rate_bps", dst, [](int v) { return std::clamp(v, 0, 20000); });
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
    UserConfigHelpers::loadAndNarrowInt64(
        tbl, "sampling", "interval_ms", dst, 1000, [](int v) { return Domain::Sampling::clampRefreshInterval(v); });
    EXPECT_EQ(dst, static_cast<int>(Domain::Sampling::clampRefreshInterval(5000)));
}

TEST(UserConfigHelpersTest, LoadAndNarrowIntUsesDefaultWhenMissing)
{
    auto tbl = toml::table{};
    int dst = 0;
    // Key missing -> destination unchanged
    UserConfigHelpers::loadAndNarrowInt(tbl, "window", "width", dst, 800, 200, 16000);
    EXPECT_EQ(dst, 0);
}
