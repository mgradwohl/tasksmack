/// @file test_CpuSummaryText.cpp
/// @brief Tests for App::Detail::cpuCoreSummary() and CpuCoreSummaryCache, shared by the Overview and
/// CPU Cores headers (#1180)

#include "App/Panels/CpuSummaryText.h"
#include "UI/Format.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>

namespace
{

using App::Detail::cpuCoreSummary;
using App::Detail::CpuCoreSummaryCache;

TEST(CpuSummaryTextTest, WithClockShowsGigahertz)
{
    EXPECT_EQ(cpuCoreSummary(16, 3700), " (16 logical processors @ 3.70 GHz)");
    EXPECT_EQ(cpuCoreSummary(8, 2450), " (8 logical processors @ 2.45 GHz)");
}

TEST(CpuSummaryTextTest, UnknownClockIsLeftOut)
{
    EXPECT_EQ(cpuCoreSummary(16, 0), " (16 logical processors)");
}

TEST(CpuSummaryTextTest, OneProcessorIsSingular)
{
    EXPECT_EQ(cpuCoreSummary(1, 0), " (1 logical processor)");
    EXPECT_EQ(cpuCoreSummary(1, 1000), " (1 logical processor @ 1.00 GHz)");
}

TEST(CpuSummaryTextTest, MatchesTheExpressionBothHeadersSpelledOutBefore)
{
    // The expression the Overview and CPU Cores headers each spelled out before #1180.
    for (const std::uint64_t freqMHz : {std::uint64_t{0}, std::uint64_t{1}, std::uint64_t{3700}, std::uint64_t{5'000'000}})
    {
        for (const int cores : {0, 1, 2, 64, 256})
        {
            const double freq = (freqMHz > 0) ? static_cast<double>(freqMHz) : 0.0;
            EXPECT_EQ(cpuCoreSummary(cores, freqMHz), UI::Format::formatLogicalProcessorSummary(cores, freq));
        }
    }
}

TEST(CpuSummaryTextTest, CacheReturnsTheSummaryForItsInputs)
{
    CpuCoreSummaryCache cache;
    EXPECT_EQ(cache.get(16, 3700), cpuCoreSummary(16, 3700));
    EXPECT_EQ(cache.get(16, 3700), cpuCoreSummary(16, 3700));
}

TEST(CpuSummaryTextTest, CacheRebuildsWhenEitherInputChanges)
{
    CpuCoreSummaryCache cache;
    EXPECT_EQ(cache.get(16, 3700), " (16 logical processors @ 3.70 GHz)");
    EXPECT_EQ(cache.get(16, 4200), " (16 logical processors @ 4.20 GHz)");
    EXPECT_EQ(cache.get(8, 4200), " (8 logical processors @ 4.20 GHz)");
    EXPECT_EQ(cache.get(8, 0), " (8 logical processors)");
}

TEST(CpuSummaryTextTest, CacheKeepsItsTextWhileInputsHold)
{
    CpuCoreSummaryCache cache;
    const std::string* const first = &cache.get(4, 2000);
    const std::string& second = cache.get(4, 2000);
    EXPECT_EQ(&second, first); // The same cached string, not a fresh one
    EXPECT_EQ(second, " (4 logical processors @ 2.00 GHz)");
}

} // namespace
