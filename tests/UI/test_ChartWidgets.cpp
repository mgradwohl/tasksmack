#include "UI/ChartWidgets.h"
#include "UI/RateAxis.h"
#include "UI/Theme.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <format>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace UI::Widgets
{
namespace
{

TEST(ChartWidgetsTest, ComputeAlphaClampsTauMin)
{
    const auto interval = std::chrono::milliseconds(10);
    const double alpha = computeAlpha(0.0, interval);

    const double expected = 1.0 - std::exp(-10.0 / 20.0);
    EXPECT_NEAR(alpha, expected, 1e-6);
}

TEST(ChartWidgetsTest, ComputeAlphaClampsTauMax)
{
    const auto interval = std::chrono::milliseconds(2000);
    const double alpha = computeAlpha(0.0, interval);

    const double expected = 1.0 - std::exp(-2000.0 / 400.0);
    EXPECT_NEAR(alpha, expected, 1e-6);
}

TEST(ChartWidgetsTest, ComputeAlphaUsesDeltaTimeWhenPositive)
{
    const auto interval = std::chrono::milliseconds(1000);
    const double alpha = computeAlpha(0.1, interval);

    const double expected = 1.0 - std::exp(-100.0 / 400.0);
    EXPECT_NEAR(alpha, expected, 1e-6);
}

TEST(ChartWidgetsTest, ComputeAlphaFallsBackForNonPositiveDelta)
{
    const auto interval = std::chrono::milliseconds(1000);
    const double alphaZero = computeAlpha(0.0, interval);
    const double alphaNegative = computeAlpha(-0.05, interval);

    EXPECT_NEAR(alphaZero, alphaNegative, 1e-6);
}

TEST(ChartWidgetsTest, ComputeAlphaFloatOverloadMatchesDoubleOverload)
{
    const auto interval = std::chrono::milliseconds(750);
    const float deltaTime = 0.123F;

    const double fromFloat = computeAlpha(deltaTime, interval);
    const double fromDouble = computeAlpha(static_cast<double>(deltaTime), interval);

    EXPECT_NEAR(fromFloat, fromDouble, 1e-9);
}

TEST(ChartWidgetsTest, SmoothTowardsInterpolates)
{
    constexpr double current = 10.0;
    constexpr double target = 20.0;

    EXPECT_DOUBLE_EQ(smoothTowards(current, target, 0.0), current);
    EXPECT_DOUBLE_EQ(smoothTowards(current, target, 1.0), target);
    EXPECT_DOUBLE_EQ(smoothTowards(current, target, 0.25), 12.5);
}

TEST(ChartWidgetsTest, InitializeOrSmoothReturnsTargetWhenUninitialized)
{
    EXPECT_DOUBLE_EQ(initializeOrSmooth(10.0, 25.0, 0.5, false), 25.0);
}

TEST(ChartWidgetsTest, InitializeOrSmoothAppliesSmoothingWhenInitialized)
{
    EXPECT_DOUBLE_EQ(initializeOrSmooth(10.0, 30.0, 0.25, true), 15.0);
}

// ========== tailAlignedSpan ==========

TEST(ChartWidgetsTest, TailAlignedSpanReturnsWholeVectorWhenCountExceedsSize)
{
    const std::vector<int> data{1, 2, 3};
    const auto span = tailAlignedSpan(data, 10);

    EXPECT_EQ(span.offset, 0U);
    ASSERT_EQ(span.values.size(), 3U);
    EXPECT_EQ(span.values[0], 1);
    EXPECT_EQ(span.values[2], 3);
}

TEST(ChartWidgetsTest, TailAlignedSpanReturnsLastCountElements)
{
    const std::vector<int> data{1, 2, 3, 4, 5};
    const auto span = tailAlignedSpan(data, 2);

    EXPECT_EQ(span.offset, 3U);
    ASSERT_EQ(span.values.size(), 2U);
    EXPECT_EQ(span.values[0], 4);
    EXPECT_EQ(span.values[1], 5);
}

TEST(ChartWidgetsTest, TailAlignedSpanWithZeroCountReturnsEmptySpanAtEnd)
{
    const std::vector<int> data{1, 2, 3};
    const auto span = tailAlignedSpan(data, 0);

    EXPECT_EQ(span.offset, 3U);
    EXPECT_TRUE(span.values.empty());
}

TEST(ChartWidgetsTest, TailAlignedSpanWithEmptyDataReturnsEmptySpan)
{
    const std::vector<int> data{};
    const auto span = tailAlignedSpan(data, 5);

    EXPECT_EQ(span.offset, 0U);
    EXPECT_TRUE(span.values.empty());
}

// ========== NowBar ==========

TEST(NowBarTest, ExplicitEmptyTooltipTextIsEmpty)
{
    const NowBar bar{.valueText = "50%", .label = "CPU", .tooltipText = {}, .color = {}};
    EXPECT_TRUE(bar.tooltipText.empty());
}

TEST(NowBarTest, DefaultValue01IsZero)
{
    const NowBar bar{.valueText = "0%", .label = "CPU", .tooltipText = {}, .color = {}};
    EXPECT_DOUBLE_EQ(bar.value01, 0.0);
}

TEST(NowBarTest, TooltipTextStoresArbitraryContent)
{
    const NowBar bar{.valueText = "50%", .label = "CPU", .tooltipText = "CPU Total: 50% (4 cores)", .value01 = 0.5, .color = {}};
    EXPECT_EQ(bar.tooltipText, "CPU Total: 50% (4 cores)");
}

// ========== selectNowBarTooltip ==========

TEST(NowBarTest, SelectTooltipPrefersTooltipText)
{
    const NowBar bar{.valueText = "50%", .label = "CPU", .tooltipText = "CPU Total: 50%", .value01 = 0.5, .color = {}};
    EXPECT_EQ(selectNowBarTooltip(bar), "CPU Total: 50%");
}

TEST(NowBarTest, SelectTooltipFallsBackToLabelColonValueWhenTooltipTextEmpty)
{
    const NowBar bar{.valueText = "50%", .label = "CPU", .tooltipText = {}, .value01 = 0.5, .color = {}};
    EXPECT_EQ(selectNowBarTooltip(bar), "CPU: 50%");
}

TEST(NowBarTest, SelectTooltipFallsBackToValueTextWhenBothEmpty)
{
    const NowBar bar{.valueText = "50%", .label = {}, .tooltipText = {}, .value01 = 0.5, .color = {}};
    EXPECT_EQ(selectNowBarTooltip(bar), "50%");
}

TEST(NowBarTest, SelectTooltipFallsBackToLabelWhenValueTextEmpty)
{
    const NowBar bar{.valueText = {}, .label = "CPU", .tooltipText = {}, .value01 = 0.5, .color = {}};
    EXPECT_EQ(selectNowBarTooltip(bar), "CPU");
}

// ========== reduceSeriesKeepingGaps ==========

TEST(ChartWidgetsReduceTest, ReductionKeepsAGapThatFallsBetweenPickedSamples)
{
    // The #1041 review case: 1,440 samples reduced to the 720-point cap pick indices 718 and 720,
    // so a lone NaN at 719 used to vanish and the line bridged the missing reading.
    constexpr int count = 1440;
    constexpr int outCount = LINE_PLOT_MAX_POINTS_DENSE;
    std::vector<float> x(count);
    std::vector<float> y(count, 50.0F);
    for (int i = 0; i < count; ++i)
    {
        x[static_cast<std::size_t>(i)] = static_cast<float>(i);
    }
    y[719] = std::numeric_limits<float>::quiet_NaN();

    std::vector<float> outX(outCount);
    std::vector<float> outY(outCount);
    reduceSeriesKeepingGaps(x.data(), y.data(), count, outCount, outX.data(), outY.data());

    int nanCount = 0;
    for (const float v : outY)
    {
        nanCount += std::isnan(v) ? 1 : 0;
    }
    EXPECT_EQ(nanCount, 1);
    EXPECT_FLOAT_EQ(outY.front(), 50.0F);
    EXPECT_FLOAT_EQ(outY.back(), 50.0F);
    EXPECT_FLOAT_EQ(outX.back(), static_cast<float>(count - 1));
}

TEST(ChartWidgetsReduceTest, ReductionOfAFiniteSeriesIsAPlainStride)
{
    constexpr int count = 10;
    constexpr int outCount = 4;
    std::vector<double> x(count);
    std::vector<double> y(count);
    for (int i = 0; i < count; ++i)
    {
        x[static_cast<std::size_t>(i)] = static_cast<double>(i);
        y[static_cast<std::size_t>(i)] = static_cast<double>(i) * 10.0;
    }

    std::vector<double> outX(outCount);
    std::vector<double> outY(outCount);
    reduceSeriesKeepingGaps(x.data(), y.data(), count, outCount, outX.data(), outY.data());

    // Indices k * 9 / 3 = 0, 3, 6, 9.
    EXPECT_DOUBLE_EQ(outY[0], 0.0);
    EXPECT_DOUBLE_EQ(outY[1], 30.0);
    EXPECT_DOUBLE_EQ(outY[2], 60.0);
    EXPECT_DOUBLE_EQ(outY[3], 90.0);
    EXPECT_DOUBLE_EQ(outX[3], 9.0);
}

TEST(ChartWidgetsReduceTest, ReductionKeepsALeadingGap)
{
    constexpr int count = 10;
    constexpr int outCount = 4;
    std::vector<float> x(count, 0.0F);
    std::vector<float> y(count, 1.0F);
    y[0] = std::numeric_limits<float>::quiet_NaN();

    std::vector<float> outX(outCount);
    std::vector<float> outY(outCount);
    reduceSeriesKeepingGaps(x.data(), y.data(), count, outCount, outX.data(), outY.data());

    EXPECT_TRUE(std::isnan(outY[0]));
    EXPECT_FLOAT_EQ(outY[1], 1.0F);
}

// ========== reduceSeriesMinMax (#1010) ==========

TEST(ChartWidgetsReduceTest, BucketWidthIsAPowerOfTwoThatHoldsAsTheSpanDrifts)
{
    // 300 s into 239 buckets: 1.255 s rounds up to 2 s, and stays 2 s as the span drifts.
    EXPECT_DOUBLE_EQ(minMaxBucketWidth(300.0, 239), 2.0);
    EXPECT_DOUBLE_EQ(minMaxBucketWidth(299.9, 239), 2.0);
    EXPECT_DOUBLE_EQ(minMaxBucketWidth(300.1, 239), 2.0);
    EXPECT_DOUBLE_EQ(minMaxBucketWidth(30.0, 239), 0.25); // 0.1255 rounds up to 2^-2
    EXPECT_DOUBLE_EQ(minMaxBucketWidth(0.0, 239), 0.0);
    EXPECT_DOUBLE_EQ(minMaxBucketWidth(std::numeric_limits<double>::quiet_NaN(), 239), 0.0);
}

namespace
{
// 100 ms samples over 300 s, as "seconds before now" (the last sample at x = 0).
struct ReduceFixture
{
    static constexpr int COUNT = 3000;
    std::vector<double> x = std::vector<double>(COUNT);
    std::vector<double> y = std::vector<double>(COUNT, 10.0);
    ReduceFixture()
    {
        for (int i = 0; i < COUNT; ++i)
        {
            x[static_cast<std::size_t>(i)] = (static_cast<double>(i) - (COUNT - 1)) * 0.1;
        }
    }
};
} // namespace

TEST(ChartWidgetsReduceTest, MinMaxReductionKeepsASingleSamplePeak)
{
    ReduceFixture f;
    f.y[1234] = 99.0; // one sample; a stride of ~4 would usually skip it
    std::vector<double> outX(LINE_PLOT_MAX_POINTS_DENSE);
    std::vector<double> outY(LINE_PLOT_MAX_POINTS_DENSE);
    const int written =
        reduceSeriesMinMax(f.x.data(), f.y.data(), ReduceFixture::COUNT, LINE_PLOT_MAX_POINTS_DENSE, 1000.0, outX.data(), outY.data());
    ASSERT_GT(written, 0);
    ASSERT_LE(written, LINE_PLOT_MAX_POINTS_DENSE);
    const auto peak = std::ranges::max(std::span(outY).first(static_cast<std::size_t>(written)));
    EXPECT_DOUBLE_EQ(peak, 99.0);
    // The newest sample is never dropped.
    EXPECT_DOUBLE_EQ(outX[static_cast<std::size_t>(written) - 1], 0.0);
}

TEST(ChartWidgetsReduceTest, MinMaxReductionIsStableAsTheWindowScrolls)
{
    // Every frame, x shifts left by the time elapsed and the anchor (now) moves right by the same
    // amount, so each sample's absolute time -- and therefore its bucket -- is unchanged.
    ReduceFixture f;
    for (int i = 0; i < ReduceFixture::COUNT; ++i)
    {
        f.y[static_cast<std::size_t>(i)] = static_cast<double>((i * 37) % 101); // jagged
    }
    std::vector<double> firstX(LINE_PLOT_MAX_POINTS_DENSE);
    std::vector<double> firstY(LINE_PLOT_MAX_POINTS_DENSE);
    const double now = 5000.03;
    const int firstCount =
        reduceSeriesMinMax(f.x.data(), f.y.data(), ReduceFixture::COUNT, LINE_PLOT_MAX_POINTS_DENSE, now, firstX.data(), firstY.data());

    for (const double elapsed : {0.016, 0.033, 0.05, 0.083})
    {
        std::vector<double> shifted(f.x);
        for (auto& v : shifted)
        {
            v -= elapsed;
        }
        std::vector<double> outX(LINE_PLOT_MAX_POINTS_DENSE);
        std::vector<double> outY(LINE_PLOT_MAX_POINTS_DENSE);
        const int count = reduceSeriesMinMax(
            shifted.data(), f.y.data(), ReduceFixture::COUNT, LINE_PLOT_MAX_POINTS_DENSE, now + elapsed, outX.data(), outY.data());
        ASSERT_EQ(count, firstCount) << "elapsed " << elapsed;
        for (int k = 0; k < count; ++k)
        {
            EXPECT_DOUBLE_EQ(outY[static_cast<std::size_t>(k)], firstY[static_cast<std::size_t>(k)]) << "point " << k;
        }
    }
}

TEST(ChartWidgetsReduceTest, MinMaxReductionKeepsAGap)
{
    ReduceFixture f;
    f.y[1500] = std::numeric_limits<double>::quiet_NaN();
    std::vector<double> outX(LINE_PLOT_MAX_POINTS_DENSE);
    std::vector<double> outY(LINE_PLOT_MAX_POINTS_DENSE);
    const int written =
        reduceSeriesMinMax(f.x.data(), f.y.data(), ReduceFixture::COUNT, LINE_PLOT_MAX_POINTS_DENSE, 0.0, outX.data(), outY.data());
    int gaps = 0;
    for (int k = 0; k < written; ++k)
    {
        gaps += std::isnan(outY[static_cast<std::size_t>(k)]) ? 1 : 0;
    }
    EXPECT_EQ(gaps, 1);
    // Points stay in x order, so the line runs left to right through the gap.
    for (int k = 1; k < written; ++k)
    {
        EXPECT_LT(outX[static_cast<std::size_t>(k) - 1], outX[static_cast<std::size_t>(k)]);
    }
}

TEST(ChartWidgetsReduceTest, MinMaxReductionAlwaysEndsAtTheNewestAndStartsAtTheOldestSample)
{
    // A flat series: every bucket's min and max are its first sample, so without the end samples
    // the line would stop up to a bucket width short of x = 0 and start late on the left.
    for (const double now : {1000.0, 1000.37, 1000.81}) // endpoints at different places in their buckets
    {
        ReduceFixture f;
        std::vector<double> outX(LINE_PLOT_MAX_POINTS_DENSE);
        std::vector<double> outY(LINE_PLOT_MAX_POINTS_DENSE);
        const int written =
            reduceSeriesMinMax(f.x.data(), f.y.data(), ReduceFixture::COUNT, LINE_PLOT_MAX_POINTS_DENSE, now, outX.data(), outY.data());
        ASSERT_GT(written, 1);
        ASSERT_LE(written, LINE_PLOT_MAX_POINTS_DENSE);
        EXPECT_DOUBLE_EQ(outX.front(), f.x.front()) << "now " << now;
        EXPECT_DOUBLE_EQ(outX[static_cast<std::size_t>(written) - 1], f.x.back()) << "now " << now;
    }
}

TEST(ChartWidgetsReduceTest, MinMaxReductionNeverExceedsItsBudget)
{
    // Worst case: every bucket has a distinct min, max and gap.
    ReduceFixture f;
    for (int i = 0; i < ReduceFixture::COUNT; ++i)
    {
        f.y[static_cast<std::size_t>(i)] = (i % 7 == 3) ? std::numeric_limits<double>::quiet_NaN() : static_cast<double>((i * 13) % 17);
    }
    for (const int budget : {30, 31, 32, 100, LINE_PLOT_MAX_POINTS_DENSE})
    {
        std::vector<double> outX(static_cast<std::size_t>(budget));
        std::vector<double> outY(static_cast<std::size_t>(budget));
        const int written = reduceSeriesMinMax(f.x.data(), f.y.data(), ReduceFixture::COUNT, budget, 7.3, outX.data(), outY.data());
        EXPECT_LE(written, budget);
        EXPECT_DOUBLE_EQ(outX[static_cast<std::size_t>(written) - 1], f.x.back()) << "budget " << budget;
    }
}

namespace
{
// Asserts that no two consecutive finite output points have a NaN source sample between them --
// which is what drawing the line across a gap would mean.
void expectNoBridgedGap(const ReduceFixture& f, const std::vector<double>& outX, const std::vector<double>& outY, int written)
{
    const auto sourceIndexOf = [&](double x)
    {
        return static_cast<int>(std::lround((x / 0.1) + (ReduceFixture::COUNT - 1)));
    };
    for (int k = 1; k < written; ++k)
    {
        const auto a = static_cast<std::size_t>(k - 1);
        const auto b = static_cast<std::size_t>(k);
        if (std::isnan(outY[a]) || std::isnan(outY[b]))
        {
            continue;
        }
        for (int src = sourceIndexOf(outX[a]) + 1; src < sourceIndexOf(outX[b]); ++src)
        {
            EXPECT_FALSE(std::isnan(f.y[static_cast<std::size_t>(src)])) << "points " << k - 1 << "-" << k << " bridge the gap at " << src;
        }
    }
}
} // namespace

TEST(ChartWidgetsReduceTest, MinMaxReductionNeverBridgesAGapWhateverItsLayoutInOneBucket)
{
    // With now = 0.05 and 100 ms samples, the 2 s buckets hold 20 samples each; 1499..1518 is one
    // bucket. Each layout puts the bucket's min and max between gaps, the case that used to bridge.
    struct Layout
    {
        std::vector<int> gaps;
        int maxAt;
        int minAt;
    };
    const std::vector<Layout> layouts{
        {.gaps = {1504}, .maxAt = 1500, .minAt = 1510},             // one run, extremes either side
        {.gaps = {1504, 1512}, .maxAt = 1508, .minAt = 1516},       // two runs
        {.gaps = {1504, 1508, 1512}, .maxAt = 1506, .minAt = 1510}, // three runs (#1051 review)
        {.gaps = {1502, 1503, 1509, 1515}, .maxAt = 1506, .minAt = 1512},
    };
    for (const auto& layout : layouts)
    {
        ReduceFixture f;
        for (const int i : layout.gaps)
        {
            f.y[static_cast<std::size_t>(i)] = std::numeric_limits<double>::quiet_NaN();
        }
        f.y[static_cast<std::size_t>(layout.maxAt)] = 50.0;
        f.y[static_cast<std::size_t>(layout.minAt)] = 0.5;
        std::vector<double> outX(LINE_PLOT_MAX_POINTS_DENSE);
        std::vector<double> outY(LINE_PLOT_MAX_POINTS_DENSE);
        const int written =
            reduceSeriesMinMax(f.x.data(), f.y.data(), ReduceFixture::COUNT, LINE_PLOT_MAX_POINTS_DENSE, 0.05, outX.data(), outY.data());
        SCOPED_TRACE(layout.gaps.size());
        expectNoBridgedGap(f, outX, outY, written);
    }
}

TEST(ChartWidgetsReduceTest, ReductionOfABuiltTimeAxisIsStableAsNowAdvances)
{
    // The production path: buildTimeAxis(timestamps, n, now) then a reduction anchored at the same
    // now. With a float axis, x + now did not recover the timestamp exactly and the error changed as
    // now advanced, so a sample this close to a bucket boundary could change bucket between frames
    // (#1051 review). Timestamps are large, like steady_clock seconds on a long-running machine.
    constexpr std::size_t COUNT = 3000;
    std::vector<double> timestamps(COUNT);
    std::vector<double> values(COUNT);
    const double start = 864'000.0; // ten days of uptime
    for (std::size_t i = 0; i < COUNT; ++i)
    {
        timestamps[i] = start + (static_cast<double>(i) * 0.1);
        values[i] = static_cast<double>((i * 37) % 101);
    }
    // One sample 2 microseconds before a 2 s bucket boundary, with a value that makes it a bucket max.
    timestamps[1500] = 864'150.0 - 2e-6;
    values[1500] = 500.0;

    std::vector<double> firstY;
    for (const double elapsed : {0.0, 0.0161, 0.0334, 0.0517, 0.0833, 0.1})
    {
        const double now = timestamps.back() + 0.04 + elapsed;
        const auto x = buildTimeAxis(timestamps, COUNT, now);
        std::vector<double> outX(LINE_PLOT_MAX_POINTS_DENSE);
        std::vector<double> outY(LINE_PLOT_MAX_POINTS_DENSE);
        const int written =
            reduceSeriesMinMax(x.data(), values.data(), static_cast<int>(COUNT), LINE_PLOT_MAX_POINTS_DENSE, now, outX.data(), outY.data());
        outY.resize(static_cast<std::size_t>(written));
        if (firstY.empty())
        {
            firstY = outY;
            continue;
        }
        EXPECT_EQ(outY, firstY) << "elapsed " << elapsed;
    }
}

TEST(ChartWidgetsReduceTest, MinMaxReductionFallsBackForAnUnusableSpan)
{
    // x not increasing (all equal): no usable bucket width, so it falls back to the stride.
    std::vector<float> x(10, 0.0F);
    std::vector<float> y(10, 1.0F);
    std::vector<float> outX(4);
    std::vector<float> outY(4);
    EXPECT_EQ(reduceSeriesMinMax(x.data(), y.data(), 10, 4, 0.0, outX.data(), outY.data()), 4);
}

// ========== Axis formatters ==========

TEST(ChartWidgetsFormattersTest, FormatAxisLocalizedHandlesSuffixes)
{
    char buf[32]{};
    int len = formatAxisLocalized(1500.0, buf, static_cast<int>(sizeof(buf)), nullptr);
    EXPECT_GT(len, 0);
    EXPECT_EQ(std::string(buf), "1.5K");

    len = formatAxisLocalized(2'000'000.0, buf, static_cast<int>(sizeof(buf)), nullptr);
    EXPECT_GT(len, 0);
    EXPECT_EQ(std::string(buf), "2.0M");
}

TEST(ChartWidgetsFormattersTest, FormatAxisLocalizedReturnsZeroWhenBufferTooSmall)
{
    char buf[2]{};
    const int len = formatAxisLocalized(999.0, buf, static_cast<int>(sizeof(buf)), nullptr);
    EXPECT_EQ(len, 0);
}

TEST(ChartWidgetsFormattersTest, FormatAxisLocalizedClampsTinyValueToZero)
{
    char buf[32]{};
    const int len = formatAxisLocalized(0.1, buf, static_cast<int>(sizeof(buf)), nullptr);
    EXPECT_GT(len, 0);
    EXPECT_EQ(std::string(buf), "0.0");
}

TEST(ChartWidgetsFormattersTest, FormatAxisBytesPerSecScalesUnits)
{
    char buf[32]{};
    int len = formatAxisBytesPerSec(100.0, buf, static_cast<int>(sizeof(buf)), nullptr);
    EXPECT_GT(len, 0);
    EXPECT_EQ(std::string(buf), "100.0B/s");

    len = formatAxisBytesPerSec(2048.0, buf, static_cast<int>(sizeof(buf)), nullptr);
    EXPECT_GT(len, 0);
    EXPECT_EQ(std::string(buf), "2.0KB/s");
}

TEST(ChartWidgetsFormattersTest, FormatAxisBytesUsesBinaryUnitsWithoutRateSuffix)
{
    // Matches UI::Format::formatBytes (binary units) so the GPU Memory axis agrees with its tooltip (#1023).
    char buf[32]{};
    int len = formatAxisBytes(512.0, buf, static_cast<int>(sizeof(buf)), nullptr);
    EXPECT_GT(len, 0);
    EXPECT_EQ(std::string(buf), "512.0B");

    len = formatAxisBytes(1536.0, buf, static_cast<int>(sizeof(buf)), nullptr);
    EXPECT_GT(len, 0);
    EXPECT_EQ(std::string(buf), "1.5KB");

    len = formatAxisBytes(1.5 * 1024.0 * 1024.0 * 1024.0, buf, static_cast<int>(sizeof(buf)), nullptr);
    EXPECT_GT(len, 0);
    EXPECT_EQ(std::string(buf), "1.5GB");

    len = formatAxisBytes(-0.1, buf, static_cast<int>(sizeof(buf)), nullptr);
    EXPECT_GT(len, 0);
    EXPECT_EQ(std::string(buf), "0.0B");
}

TEST(ChartWidgetsFormattersTest, FormatAxisBytesPerSecClampsTinyNegativeToZero)
{
    char buf[32]{};
    const int len = formatAxisBytesPerSec(-0.1, buf, static_cast<int>(sizeof(buf)), nullptr);
    EXPECT_GT(len, 0);
    EXPECT_EQ(std::string(buf), "0.0B/s");
}

TEST(ChartWidgetsFormattersTest, FormatAxisWattsUsesWAndMilliwatts)
{
    char buf[32]{};
    int len = formatAxisWatts(10.0, buf, static_cast<int>(sizeof(buf)), nullptr);
    EXPECT_GT(len, 0);
    EXPECT_EQ(std::string(buf), "10.0W");

    len = formatAxisWatts(0.5, buf, static_cast<int>(sizeof(buf)), nullptr);
    EXPECT_GT(len, 0);
    EXPECT_EQ(std::string(buf), "500.0mW");
}

TEST(ChartWidgetsFormattersTest, FormatAxisWattsClampsTinyNegativeToZeroMilliwatts)
{
    char buf[32]{};
    const int len = formatAxisWatts(-0.00001, buf, static_cast<int>(sizeof(buf)), nullptr);
    EXPECT_GT(len, 0);
    EXPECT_EQ(std::string(buf), "0.0mW");
}

TEST(ChartWidgetsFormattersTest, FormatAxisPercentFormatsOneDecimal)
{
    char buf[32]{};
    const int len = formatAxisPercent(12.34, buf, static_cast<int>(sizeof(buf)), nullptr);
    EXPECT_GT(len, 0);
    EXPECT_EQ(std::string(buf), "12.3%");
}

TEST(ChartWidgetsFormattersTest, FormatAxisPercentClampsTinyNegativeToZero)
{
    char buf[32]{};
    const int len = formatAxisPercent(-0.1, buf, static_cast<int>(sizeof(buf)), nullptr);
    EXPECT_GT(len, 0);
    EXPECT_EQ(std::string(buf), "0.0%");
}

TEST(ChartWidgetsFormattersTest, FormatAxisLocalizedHandlesGigaSuffix)
{
    char buf[32]{};
    const int len = formatAxisLocalized(2'500'000'000.0, buf, static_cast<int>(sizeof(buf)), nullptr);
    EXPECT_GT(len, 0);
    EXPECT_EQ(std::string(buf), "2.5G");
}

TEST(ChartWidgetsFormattersTest, FormatAxisBytesPerSecHandlesMegaAndGigaSuffixes)
{
    char buf[32]{};
    int len = formatAxisBytesPerSec(5.0 * 1024.0 * 1024.0, buf, static_cast<int>(sizeof(buf)), nullptr);
    EXPECT_GT(len, 0);
    EXPECT_EQ(std::string(buf), "5.0MB/s");

    len = formatAxisBytesPerSec(2.0 * 1024.0 * 1024.0 * 1024.0, buf, static_cast<int>(sizeof(buf)), nullptr);
    EXPECT_GT(len, 0);
    EXPECT_EQ(std::string(buf), "2.0GB/s");
}

// ========== Tooltip rows (#1008, #1020) ==========

TEST(ChartWidgetsTest, TooltipRowIsLabelColonValue)
{
    EXPECT_EQ(formatTooltipRow("Read", "1.5 MB/s"), "Read: 1.5 MB/s");
    EXPECT_EQ(formatTooltipRow("Page Faults/s", "12/s"), "Page Faults/s: 12/s");
}

TEST(ChartWidgetsTest, SampleWithNoReadingFormatsAsNA)
{
    const auto percent = [](double v)
    {
        return std::format("{:.0f}%", v);
    };
    EXPECT_EQ(formatSampleOrNA(42.0, percent), "42%");
    EXPECT_EQ(formatSampleOrNA(std::numeric_limits<double>::quiet_NaN(), percent), "N/A");
    EXPECT_EQ(formatSampleOrNA(std::numeric_limits<double>::infinity(), percent), "N/A");
}

// ========== holdLastValueToNow (#1016) ==========

TEST(ChartWidgetsTest, HoldExtendsTheLastValueToNow)
{
    std::vector<double> x{-3.0, -2.0, -0.7};
    std::vector<double> y{10.0, 20.0, 30.0};
    holdLastValueToNow(x, y);
    ASSERT_EQ(x.size(), 4U);
    EXPECT_DOUBLE_EQ(x.back(), 0.0);
    EXPECT_DOUBLE_EQ(y.back(), 30.0);
}

TEST(ChartWidgetsTest, HoldLeavesAGapAtTheEndAlone)
{
    // A trailing NaN is a missing reading: there is nothing to hold, and the gap must stay a gap.
    std::vector<double> x{-2.0, -1.0};
    std::vector<double> y{5.0, std::numeric_limits<double>::quiet_NaN()};
    holdLastValueToNow(x, y);
    EXPECT_EQ(x.size(), 2U);
}

TEST(ChartWidgetsTest, HoldDoesNothingForAnEmptyOrAlreadyCurrentSeries)
{
    std::vector<double> emptyX;
    std::vector<double> emptyY;
    holdLastValueToNow(emptyX, emptyY);
    EXPECT_TRUE(emptyX.empty());

    std::vector<double> x{-1.0, 0.0};
    std::vector<double> y{1.0, 2.0};
    holdLastValueToNow(x, y);
    EXPECT_EQ(x.size(), 2U);
}

// ========== normalizeToUnitInterval ==========

TEST(ChartWidgetsTest, NormalizeToUnitIntervalScalesAndClamps)
{
    EXPECT_DOUBLE_EQ(normalizeToUnitInterval(25.0, 100.0), 0.25);
    EXPECT_DOUBLE_EQ(normalizeToUnitInterval(150.0, 100.0), 1.0);
    EXPECT_DOUBLE_EQ(normalizeToUnitInterval(-5.0, 100.0), 0.0);
    EXPECT_DOUBLE_EQ(normalizeToUnitInterval(5.0, 0.0), 0.0);
}

TEST(ChartWidgetsTest, NormalizeToUnitIntervalOfAMissingValueIsAnEmptyBar)
{
    // std::clamp passes NaN straight through; a bar with no value must be empty, not NaN.
    EXPECT_DOUBLE_EQ(normalizeToUnitInterval(std::numeric_limits<double>::quiet_NaN(), 100.0), 0.0);
    EXPECT_DOUBLE_EQ(normalizeToUnitInterval(5.0, std::numeric_limits<double>::quiet_NaN()), 0.0);
}

// A bar normalised against its chart's axis bound sits at the same height as its line (#1003).
TEST(ChartWidgetsTest, ABarScaledToItsAxisMatchesItsLine)
{
    const double upper = rateAxisUpperBound(1000.0, RATE_AXIS_MIN_SPAN_BYTES_PER_SEC);
    const auto config = rateHistoryConfigWithUpper("##t", -300.0, 0.0, formatAxisBytesPerSec, upper);
    ASSERT_TRUE(config.yLimits.has_value());
    // The line's height for the peak value, as a fraction of the axis, equals the bar's.
    const double lineFraction = 1000.0 / config.yLimits.value_or(std::pair{0.0, 1.0}).second;
    EXPECT_DOUBLE_EQ(normalizeToUnitInterval(1000.0, upper), lineFraction);
    EXPECT_LT(lineFraction, 1.0); // headroom above the peak, never a full bar beside a line at 91 %
}

// ========== forEachFiniteRun (#989) ==========

namespace
{
[[nodiscard]] std::vector<std::pair<int, int>> finiteRuns(const std::vector<float>& values)
{
    std::vector<std::pair<int, int>> runs;
    UI::Widgets::forEachFiniteRun(
        values.data(), static_cast<int>(values.size()), [&](int start, int length) { runs.emplace_back(start, length); });
    return runs;
}
} // namespace

TEST(ChartWidgetsTest, FiniteRunsOfAnUnbrokenSeriesIsOneRun)
{
    EXPECT_EQ(finiteRuns({1.0F, 2.0F, 3.0F}), (std::vector<std::pair<int, int>>{{0, 3}}));
}

// A missing reading in the middle splits the series, so the fill leaves a gap there.
TEST(ChartWidgetsTest, FiniteRunsSplitAtNaN)
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    EXPECT_EQ(finiteRuns({1.0F, 2.0F, nan, 4.0F, 5.0F}), (std::vector<std::pair<int, int>>{{0, 2}, {3, 2}}));
    EXPECT_EQ(finiteRuns({1.0F, nan, nan, 4.0F}), (std::vector<std::pair<int, int>>{{0, 1}, {3, 1}}));
}

TEST(ChartWidgetsTest, FiniteRunsSkipLeadingAndTrailingGaps)
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    EXPECT_EQ(finiteRuns({nan, 2.0F, 3.0F, nan}), (std::vector<std::pair<int, int>>{{1, 2}}));
}

TEST(ChartWidgetsTest, FiniteRunsOfNothingFiniteIsNoRuns)
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    EXPECT_TRUE(finiteRuns({}).empty());
    EXPECT_TRUE(finiteRuns({nan, nan}).empty());
    EXPECT_TRUE(finiteRuns({std::numeric_limits<float>::infinity()}).empty());
}

TEST(ChartWidgetsTest, NormalizeToUnitIntervalScalesWithinRange)
{
    EXPECT_DOUBLE_EQ(normalizeToUnitInterval(50.0, 100.0), 0.5);
}

TEST(ChartWidgetsTest, NormalizeToUnitIntervalReturnsZeroWhenMaxValueIsZeroOrNegative)
{
    EXPECT_DOUBLE_EQ(normalizeToUnitInterval(50.0, 0.0), 0.0);
    EXPECT_DOUBLE_EQ(normalizeToUnitInterval(50.0, -10.0), 0.0);
}

TEST(ChartWidgetsTest, NormalizeToUnitIntervalClampsAboveMax)
{
    EXPECT_DOUBLE_EQ(normalizeToUnitInterval(150.0, 100.0), 1.0);
}

TEST(ChartWidgetsTest, NormalizeToUnitIntervalClampsBelowZero)
{
    EXPECT_DOUBLE_EQ(normalizeToUnitInterval(-10.0, 100.0), 0.0);
}

// ========== Time-axis helpers ==========

TEST(ChartWidgetsTimeAxisTest, MakeTimeAxisConfigClampsOffset)
{
    const std::vector<double> timestamps{10.0, 20.0, 30.0, 40.0};
    const auto cfg = makeTimeAxisConfig(timestamps, 5.0, 100.0);

    EXPECT_DOUBLE_EQ(cfg.span, 30.0);
    EXPECT_DOUBLE_EQ(cfg.maxOffset, 25.0);
    EXPECT_DOUBLE_EQ(cfg.clampedOffset, 25.0);
    EXPECT_DOUBLE_EQ(cfg.xMin, -30.0);
    EXPECT_DOUBLE_EQ(cfg.xMax, -25.0);
}

TEST(ChartWidgetsTimeAxisTest, MakeTimeAxisConfigWithEmptyTimestampsUsesDefaultWindow)
{
    const std::vector<double> timestamps{};
    const auto cfg = makeTimeAxisConfig(timestamps, 12.0, 4.0);

    EXPECT_DOUBLE_EQ(cfg.span, 0.0);
    EXPECT_DOUBLE_EQ(cfg.maxOffset, 0.0);
    EXPECT_DOUBLE_EQ(cfg.clampedOffset, 0.0);
    EXPECT_DOUBLE_EQ(cfg.xMin, -12.0);
    EXPECT_DOUBLE_EQ(cfg.xMax, 0.0);
}

TEST(ChartWidgetsTimeAxisTest, MakeTimeAxisConfigClampsNegativeOffsetToZero)
{
    const std::vector<double> timestamps{5.0, 15.0};
    const auto cfg = makeTimeAxisConfig(timestamps, 6.0, -3.0);

    EXPECT_DOUBLE_EQ(cfg.clampedOffset, 0.0);
    EXPECT_DOUBLE_EQ(cfg.xMin, -6.0);
    EXPECT_DOUBLE_EQ(cfg.xMax, 0.0);
}

TEST(ChartWidgetsTimeAxisTest, BuildTimeAxisReturnsRelativeTimes)
{
    const std::vector<double> timestamps{10.0, 20.0, 30.0};
    const auto axis = buildTimeAxis(timestamps, 2, 30.0);

    ASSERT_EQ(axis.size(), 2U);
    EXPECT_FLOAT_EQ(axis[0], -10.0F);
    EXPECT_FLOAT_EQ(axis[1], 0.0F);
}

TEST(ChartWidgetsTimeAxisTest, BuildTimeAxisReturnsEmptyWhenInputEmpty)
{
    const std::vector<double> timestamps{};
    const auto axis = buildTimeAxis(timestamps, 5, 30.0);
    EXPECT_TRUE(axis.empty());
}

TEST(ChartWidgetsTimeAxisTest, BuildTimeAxisDoublesReturnsRelativeTimes)
{
    const std::vector<double> timestamps{10.0, 20.0, 30.0};
    const auto axis = buildTimeAxisDoubles(timestamps, 3, 25.0);

    ASSERT_EQ(axis.size(), 3U);
    EXPECT_DOUBLE_EQ(axis[0], -15.0);
    EXPECT_DOUBLE_EQ(axis[1], -5.0);
    EXPECT_DOUBLE_EQ(axis[2], 5.0);
}

TEST(ChartWidgetsTimeAxisTest, BuildTimeAxisDoublesRespectsDesiredCount)
{
    const std::vector<double> timestamps{1.0, 3.0, 7.0, 9.0};
    const auto axis = buildTimeAxisDoubles(timestamps, 2, 10.0);

    ASSERT_EQ(axis.size(), 2U);
    EXPECT_DOUBLE_EQ(axis[0], -3.0);
    EXPECT_DOUBLE_EQ(axis[1], -1.0);
}

TEST(ChartWidgetsTimeAxisTest, HoveredIndexFromPlotXHandlesBoundsAndMiddle)
{
    const std::vector<float> axisF{-10.0F, -5.0F, 0.0F};
    const auto resF_lo = hoveredIndexFromPlotX(axisF, -99.0);
    ASSERT_TRUE(resF_lo.has_value());
    EXPECT_EQ(resF_lo.value(), 0U);
    const auto resF_hi = hoveredIndexFromPlotX(axisF, 99.0);
    ASSERT_TRUE(resF_hi.has_value());
    EXPECT_EQ(resF_hi.value(), 2U);
    const auto resF_mid = hoveredIndexFromPlotX(axisF, -4.2);
    ASSERT_TRUE(resF_mid.has_value());
    EXPECT_EQ(resF_mid.value(), 1U);

    const std::vector<double> axisD{-10.0, -5.0, 0.0};
    const auto resD_lo = hoveredIndexFromPlotX(axisD, -9.9);
    ASSERT_TRUE(resD_lo.has_value());
    EXPECT_EQ(resD_lo.value(), 0U);
    const auto resD_mid = hoveredIndexFromPlotX(axisD, -2.5);
    ASSERT_TRUE(resD_mid.has_value());
    EXPECT_EQ(resD_mid.value(), 1U);
}

TEST(ChartWidgetsTimeAxisTest, HoveredIndexFromPlotXTieSelectsLowerNeighbor)
{
    const std::vector<float> axisF{-10.0F, -5.0F};
    const auto resFTie = hoveredIndexFromPlotX(axisF, -7.5);
    ASSERT_TRUE(resFTie.has_value());
    EXPECT_EQ(resFTie.value(), 0U);

    const std::vector<double> axisD{-10.0, -5.0};
    const auto resDTie = hoveredIndexFromPlotX(axisD, -7.5);
    ASSERT_TRUE(resDTie.has_value());
    EXPECT_EQ(resDTie.value(), 0U);
}

TEST(ChartWidgetsTimeAxisTest, HoveredIndexFromPlotXReturnsNulloptForEmptyInput)
{
    const std::vector<float> axisF{};
    const std::vector<double> axisD{};
    EXPECT_FALSE(hoveredIndexFromPlotX(axisF, 0.0).has_value());
    EXPECT_FALSE(hoveredIndexFromPlotX(axisD, 0.0).has_value());
}

// ========== HistoryChart configuration ==========

TEST(HistoryChartConfigTest, PercentConfigLocksZeroToHundred)
{
    const auto cfg = percentHistoryConfig("##CPU", -60.0, 0.0);
    EXPECT_STREQ(cfg.id, "##CPU");
    EXPECT_DOUBLE_EQ(cfg.xMin, -60.0);
    EXPECT_DOUBLE_EQ(cfg.xMax, 0.0);
    ASSERT_TRUE(cfg.yLimits.has_value());
    EXPECT_DOUBLE_EQ(cfg.yLimits->first, 0.0);
    EXPECT_DOUBLE_EQ(cfg.yLimits->second, 100.0);
    EXPECT_EQ(cfg.yFormatter, &formatAxisPercent);
    EXPECT_TRUE(cfg.showLegend);
    EXPECT_FLOAT_EQ(cfg.height, HISTORY_PLOT_HEIGHT_DEFAULT);
}

TEST(HistoryChartConfigTest, AutoFitConfigHasNoYLimits)
{
    const auto cfg = autoFitHistoryConfig("##Net", -30.0, 0.0, formatAxisBytesPerSec);
    EXPECT_STREQ(cfg.id, "##Net");
    EXPECT_FALSE(cfg.yLimits.has_value());
    EXPECT_EQ(cfg.yFormatter, &formatAxisBytesPerSec);
}

TEST(HistoryChartConfigTest, RateConfigPinsZeroAndSizesTheTopFromTheData)
{
    const auto cfg = rateHistoryConfig("##Disk", -300.0, 0.0, formatAxisBytesPerSec, 10'000.0, RATE_AXIS_MIN_SPAN_BYTES_PER_SEC);
    EXPECT_STREQ(cfg.id, "##Disk");
    EXPECT_DOUBLE_EQ(cfg.xMin, -300.0);
    EXPECT_DOUBLE_EQ(cfg.xMax, 0.0);
    ASSERT_TRUE(cfg.yLimits.has_value());
    EXPECT_DOUBLE_EQ(cfg.yLimits->first, 0.0);
    EXPECT_DOUBLE_EQ(cfg.yLimits->second, rateAxisUpperBound(10'000.0, RATE_AXIS_MIN_SPAN_BYTES_PER_SEC));
    EXPECT_EQ(cfg.yFormatter, &formatAxisBytesPerSec);
}

TEST(HistoryChartConfigTest, RateConfigStillSetsLimitsForAnAllZeroSeries)
{
    // The regression that matters: if rateHistoryConfig() ever stopped assigning yLimits, the axis
    // would fall back to ImPlot's auto-fit and reproduce #920 exactly -- a +/-0.5 sliver with a
    // negative tick. The RateAxis unit tests would not notice, because they only cover the maths.
    const auto cfg = rateHistoryConfig("##Idle", -300.0, 0.0, formatAxisBytesPerSec, 0.0, RATE_AXIS_MIN_SPAN_BYTES_PER_SEC);
    ASSERT_TRUE(cfg.yLimits.has_value());
    EXPECT_DOUBLE_EQ(cfg.yLimits->first, 0.0);
    EXPECT_DOUBLE_EQ(cfg.yLimits->second, RATE_AXIS_MIN_SPAN_BYTES_PER_SEC);
    EXPECT_GT(cfg.yLimits->second, cfg.yLimits->first);
}

TEST(HistoryChartConfigTest, RateConfigTakesTheLockedAxisPathNotAutoFit)
{
    const auto cfg = rateHistoryConfig("##Watts", -60.0, 0.0, formatAxisWatts, 0.0, RATE_AXIS_MIN_SPAN_WATTS);
    EXPECT_EQ(historyChartYAxisFlags(cfg.yLimits.has_value()), ImPlotAxisFlags_Lock | Y_AXIS_FLAGS_DEFAULT);
}

TEST(HistoryChartConfigTest, YAxisFlagsLockWithFixedLimitsAutoFitOtherwise)
{
    EXPECT_EQ(historyChartYAxisFlags(true), ImPlotAxisFlags_Lock | Y_AXIS_FLAGS_DEFAULT);
    EXPECT_EQ(historyChartYAxisFlags(false), ImPlotAxisFlags_AutoFit | Y_AXIS_FLAGS_DEFAULT);
}

// ========== historyChartBeginPlotFlags (perf-plan #843 phase 1: showLegend=false must
// actually suppress the legend, not just skip customizing it) ==========

TEST(ChartWidgetsTest, DefaultPlotFlagsHideImPlotsMouseReadout)
{
    // Every history chart has its own tooltip; ImPlot's raw cursor coordinates were a second,
    // unlabelled readout of the same point (#1039).
    EXPECT_TRUE((PLOT_FLAGS_DEFAULT & ImPlotFlags_NoMouseText) != 0);
}

TEST(HistoryChartConfigTest, BeginPlotFlagsUnchangedWhenLegendShown)
{
    EXPECT_EQ(historyChartBeginPlotFlags(PLOT_FLAGS_DEFAULT, true), PLOT_FLAGS_DEFAULT);
}

TEST(HistoryChartConfigTest, BeginPlotFlagsAddsNoLegendWhenLegendHidden)
{
    const ImPlotFlags result = historyChartBeginPlotFlags(PLOT_FLAGS_DEFAULT, false);
    EXPECT_EQ(result, PLOT_FLAGS_DEFAULT | ImPlotFlags_NoLegend);
    EXPECT_TRUE(result & ImPlotFlags_NoLegend);
}

TEST(HistoryChartConfigTest, BeginPlotFlagsPreservesOtherConfiguredBitsWhenLegendHidden)
{
    const ImPlotFlags configured = PLOT_FLAGS_DEFAULT | ImPlotFlags_NoTitle;
    const ImPlotFlags result = historyChartBeginPlotFlags(configured, false);
    EXPECT_TRUE(result & ImPlotFlags_NoTitle);
    EXPECT_TRUE(result & ImPlotFlags_NoMenus);
    EXPECT_TRUE(result & ImPlotFlags_NoLegend);
}

// ========== Generic helpers ==========

TEST(ChartWidgetsHelpersTest, FormatAgeSecondsUsesAbsoluteValue)
{
    EXPECT_EQ(formatAgeSeconds(2.5), "Age: 2.5s");
    EXPECT_EQ(formatAgeSeconds(-2.5), "Age: 2.5s");
}

// ========== Chart anti-aliasing toggle (perf-plan #843 phase 1) ==========

// Fixture restores the shared global to its documented default (true) after every test, so
// tests stay order-independent regardless of gtest's actual run order within this binary --
// this is process-wide, mutable state (App's composition root is the only intended writer
// outside tests), not something each test can get an isolated copy of.
class ChartAntiAliasingTest : public ::testing::Test
{
  protected:
    void TearDown() override
    {
        setChartAntiAliasingEnabled(true);
    }
};

TEST_F(ChartAntiAliasingTest, DefaultsToEnabled)
{
    // Deliberately does NOT call setChartAntiAliasingEnabled() first: the point is to observe
    // the flag's value as left by whatever ran before this test, which -- given every test in
    // this fixture restores it to true in TearDown() -- should always be true. A regression
    // that flips the compile-time default to false, or a test elsewhere that fails to restore
    // it, would surface here.
    EXPECT_TRUE(chartAntiAliasingEnabled());
}

TEST_F(ChartAntiAliasingTest, SetterUpdatesGetter)
{
    setChartAntiAliasingEnabled(false);
    EXPECT_FALSE(chartAntiAliasingEnabled());

    setChartAntiAliasingEnabled(true);
    EXPECT_TRUE(chartAntiAliasingEnabled());
}

// ========== clearChartAntiAliasingFlags (pure bit logic backing HistoryChart's AA override) ====

TEST(ClearChartAntiAliasingFlagsTest, ClearsExactlyTheThreeAABits)
{
    constexpr ImDrawListFlags allAaBitsPlusUnrelated = ImDrawListFlags_AntiAliasedLines | ImDrawListFlags_AntiAliasedLinesUseTex |
                                                       ImDrawListFlags_AntiAliasedFill | ImDrawListFlags_AllowVtxOffset;

    const ImDrawListFlags cleared = clearChartAntiAliasingFlags(allAaBitsPlusUnrelated);

    EXPECT_EQ(cleared, ImDrawListFlags_AllowVtxOffset) << "only the non-AA bit should survive";
}

TEST(ClearChartAntiAliasingFlagsTest, PreservesUnrelatedBitsWhenNoAABitsSet)
{
    constexpr ImDrawListFlags flags = ImDrawListFlags_AllowVtxOffset;
    EXPECT_EQ(clearChartAntiAliasingFlags(flags), flags);
}

TEST(ClearChartAntiAliasingFlagsTest, IsIdempotentOnAlreadyClearedFlags)
{
    const ImDrawListFlags onceCleared = clearChartAntiAliasingFlags(ImDrawListFlags_AntiAliasedLines);
    EXPECT_EQ(clearChartAntiAliasingFlags(onceCleared), onceCleared);
}

TEST(ClearChartAntiAliasingFlagsTest, NoOpOnZeroFlags)
{
    EXPECT_EQ(clearChartAntiAliasingFlags(0), 0);
}

// ========== NowBar width (#971) ==========

// 2.25 em is the fixed 24px it replaces at the reference em (the Medium preset on a 1.0 display
// scale), so the bars are unchanged there.
TEST(ChartWidgetsTest, NowBarWidthIsUnchangedAtTheReferenceFont)
{
    EXPECT_FLOAT_EQ(nowBarWidth(32.0F / 3.0F), 24.0F);
}

// The reported case: on a 175% display the bars stayed 24 physical pixels beside 37px text.
TEST(ChartWidgetsTest, NowBarWidthGrowsWithTheFont)
{
    EXPECT_FLOAT_EQ(nowBarWidth(37.33F), 84.0F);
    EXPECT_FLOAT_EQ(nowBarWidth(14.0F), 32.0F); // whole pixels: 31.5 rounds up
}

TEST(ChartWidgetsTest, NowBarWidthSurvivesDegenerateInput)
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    for (const float em : {nan, 0.0F, -12.0F, std::numeric_limits<float>::infinity()})
    {
        const float width = nowBarWidth(em);
        EXPECT_TRUE(std::isfinite(width));
        EXPECT_GE(width, 1.0F);
    }
}
} // namespace
} // namespace UI::Widgets
