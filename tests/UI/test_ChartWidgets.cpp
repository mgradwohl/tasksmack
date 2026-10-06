#include "UI/ChartWidgets.h"
#include "UI/Format.h"
#include "UI/RateAxis.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <tuple>
#include <unordered_map>
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

TEST(NowBarListTest, HoldsBarsInOrderAndViewsThemAsASpan)
{
    NowBarList bars;
    EXPECT_TRUE(bars.empty());
    bars.push_back({.valueText = "12%", .label = "Read", .tooltipText = {}, .value01 = 0.12, .color = {}});
    bars.push_back({.valueText = "34%", .label = "Write", .tooltipText = {}, .value01 = 0.34, .color = {}});
    ASSERT_EQ(bars.size(), 2U);

    const std::span<const NowBar> view = bars;
    ASSERT_EQ(view.size(), 2U);
    EXPECT_EQ(view[0].label, "Read");
    EXPECT_EQ(view[1].valueText, "34%");
    EXPECT_DOUBLE_EQ(view[1].value01, 0.34);
}

TEST(NowBarListTest, CapacityCoversTheLargestChart)
{
    // The GPU core chart has the most bars: utilization, memory, clock, encoder and decoder.
    EXPECT_GE(NowBarList::CAPACITY, 5U);
}

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

// ========== TimeAxisPool (#1018) ==========

TEST(TimeAxisPoolTest, HandsOutDistinctBuffersWithinAFrame)
{
    TimeAxisPool pool;
    auto& first = pool.acquire(1);
    auto& second = pool.acquire(1);
    EXPECT_NE(&first, &second);
    EXPECT_EQ(pool.bufferCount(), 2U);
}

TEST(TimeAxisPoolTest, ReusesBuffersAndTheirCapacityInTheNextFrame)
{
    // The point of the pool: from the second frame on, building the axes allocates nothing.
    TimeAxisPool pool;
    const std::vector<double> timestamps = {1.0, 2.0, 3.0, 4.0};
    auto& frame1 = pool.acquire(1);
    fillTimeAxis(frame1, timestamps, 4, 5.0);
    const double* storage = frame1.data();

    auto& frame2 = pool.acquire(2);
    EXPECT_EQ(&frame2, &frame1);
    fillTimeAxis(frame2, timestamps, 3, 6.0);
    EXPECT_EQ(frame2.data(), storage); // no reallocation for a shorter axis
    EXPECT_EQ(frame2, (std::vector<double>{-4.0, -3.0, -2.0}));
    EXPECT_EQ(pool.bufferCount(), 1U);
}

TEST(TimeAxisPoolTest, EarlierBuffersSurviveThePoolGrowingInTheSameFrame)
{
    // A chart holds a span of its axis while later charts in the frame acquire more buffers.
    TimeAxisPool pool;
    const std::vector<double> timestamps = {10.0, 20.0};
    auto& first = pool.acquire(7);
    fillTimeAxis(first, timestamps, 2, 20.0);
    const std::span<const double> held(first);
    for (int i = 0; i < 64; ++i)
    {
        static_cast<void>(pool.acquire(7));
    }
    ASSERT_EQ(held.size(), 2U);
    EXPECT_DOUBLE_EQ(held[0], -10.0);
    EXPECT_DOUBLE_EQ(held[1], 0.0);
}

TEST(TimeAxisPoolTest, ReleasesTheBuffersOfABurstOnceTheyGoUnused)
{
    // #1173: one frame of 64 long axes (CPU Cores on a 64-core machine) used to pin 64 buffers at
    // their peak size forever, long after the tab was left.
    TimeAxisPool pool;
    const std::vector<double> timestamps(18000, 1.0); // 30 min at 100 ms
    for (int i = 0; i < 64; ++i)
    {
        fillTimeAxis(pool.acquire(1), timestamps, timestamps.size(), 2.0);
    }
    ASSERT_EQ(pool.bufferCount(), 64U);

    // From then on, two short charts a frame.
    int frame = 2;
    for (; frame <= 1 + TimeAxisPool::RELEASE_AFTER_FRAMES; ++frame)
    {
        fillTimeAxis(pool.acquire(frame), timestamps, 10, 2.0);
        fillTimeAxis(pool.acquire(frame), timestamps, 10, 2.0);
    }
    // Still within the grace period of the burst's last use: nothing is freed yet, so switching
    // back to the tab does not reallocate.
    EXPECT_EQ(pool.bufferCount(), 64U);

    static_cast<void>(pool.acquire(frame));
    EXPECT_EQ(pool.bufferCount(), 2U);
    // The two buffers still in use keep their capacity; the 62 others' is gone.
    EXPECT_LE(pool.retainedCapacity(), 2U * timestamps.size());
}

TEST(TimeAxisPoolTest, FramesThatAskForNoAxisStillReleaseUnusedBuffers)
{
    // #1173: after leaving the chart tabs (the Processes tab asks for no time axis), buffers must
    // still go once unused; beginFrame() is called every frame, acquire() only by charts.
    TimeAxisPool pool;
    const std::vector<double> timestamps(18000, 1.0);
    for (int i = 0; i < 8; ++i)
    {
        fillTimeAxis(pool.acquire(1), timestamps, timestamps.size(), 2.0);
    }
    ASSERT_EQ(pool.bufferCount(), 8U);

    int frame = 2;
    for (; frame <= 1 + TimeAxisPool::RELEASE_AFTER_FRAMES; ++frame)
    {
        pool.beginFrame(frame);
    }
    EXPECT_EQ(pool.bufferCount(), 8U); // still within the grace period
    pool.beginFrame(frame);
    EXPECT_EQ(pool.bufferCount(), 0U);
    EXPECT_EQ(pool.retainedCapacity(), 0U);
}

TEST(TimeAxisPoolTest, BeginFrameTwiceInAFrameKeepsBuffersAlreadyHandedOut)
{
    // trimFrameCaches() and the frame's first acquire() both call beginFrame(); a repeat in the same
    // frame must not reset the hand-out index, or the next acquire() would reuse a buffer in use.
    TimeAxisPool pool;
    auto& first = pool.acquire(5);
    first.assign(3, 1.0);
    // A span into the heap buffer, not a reference to the Slot: acquiring again may grow the pool and
    // move the Slot, but its buffer stays put (see EarlierBuffersSurviveThePoolGrowingInTheSameFrame).
    const std::span<const double> held(first);
    pool.beginFrame(5);
    auto& second = pool.acquire(5);
    second.assign(3, 2.0);
    EXPECT_EQ(pool.bufferCount(), 2U);
    ASSERT_EQ(held.size(), 3U);
    for (const double value : held)
    {
        EXPECT_DOUBLE_EQ(value, 1.0);
    }
}

TEST(TimeAxisPoolTest, KeepsBuffersThatAreStillAskedFor)
{
    TimeAxisPool pool;
    for (int frame = 1; frame <= 3 * TimeAxisPool::RELEASE_AFTER_FRAMES; ++frame)
    {
        static_cast<void>(pool.acquire(frame));
        static_cast<void>(pool.acquire(frame));
        static_cast<void>(pool.acquire(frame));
    }
    EXPECT_EQ(pool.bufferCount(), 3U);
}

TEST(TimeAxisPoolTest, AFrameCountThatGoesBackwardsReleasesTheOldBuffers)
{
    // A new ImGui context restarts the frame count; the old context's buffers are not kept forever.
    TimeAxisPool pool;
    for (int i = 0; i < 8; ++i)
    {
        static_cast<void>(pool.acquire(5000));
    }
    static_cast<void>(pool.acquire(1));
    EXPECT_EQ(pool.bufferCount(), 1U);
}

// ========== Frame-keyed caches (#1181) ==========

TEST(FrameScopeDeathTest, AFrameKeyedCacheUsedOutsideAFrameAssertsInDebugBuilds)
{
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    // Release builds compile the check out, so the statement then simply runs.
    EXPECT_DEBUG_DEATH(Detail::requireWithinImGuiFrame(false), "outside an ImGui frame");
}

TEST(FrameScopeTest, AFrameKeyedCacheUsedInsideAFrameDoesNotAssert)
{
    Detail::requireWithinImGuiFrame(true);
    SUCCEED();
}

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

TEST(ChartWidgetsReduceTest, AlignedReductionCapsStackedSeriesAndKeepsThemAligned)
{
    // #1022 review: the stacked CPU bands and the process CPU lines were drawn with ImPlot directly,
    // uncapped. reduceAlignedSeries() caps them while keeping every series at the same x points.
    ReduceFixture f;
    std::vector<double> user(ReduceFixture::COUNT, 5.0);
    std::vector<double> top(ReduceFixture::COUNT, 20.0);
    std::vector<double> carried(ReduceFixture::COUNT);
    for (int i = 0; i < ReduceFixture::COUNT; ++i)
    {
        carried[static_cast<std::size_t>(i)] = static_cast<double>(i); // identifies the source sample
    }
    user[1234] = 60.0; // a single-sample peak in one band
    top[2345] = 95.0;  // and in another
    auto x = f.x;

    reduceAlignedSeries(x, {&user, &top}, {&carried}, LINE_PLOT_MAX_POINTS_DENSE, 1000.0);

    ASSERT_LE(x.size(), static_cast<std::size_t>(LINE_PLOT_MAX_POINTS_DENSE));
    ASSERT_GT(x.size(), 2U);
    ASSERT_EQ(user.size(), x.size());
    ASSERT_EQ(top.size(), x.size());
    ASSERT_EQ(carried.size(), x.size());
    // Each kept point is one source sample, taken from every series at once.
    for (std::size_t k = 0; k < x.size(); ++k)
    {
        const auto source = static_cast<std::size_t>(carried[k]);
        EXPECT_DOUBLE_EQ(x[k], f.x[source]) << "point " << k;
        if (k > 0)
        {
            EXPECT_GT(carried[k], carried[k - 1]);
        }
    }
    EXPECT_DOUBLE_EQ(std::ranges::max(user), 60.0);
    EXPECT_DOUBLE_EQ(std::ranges::max(top), 95.0);
    // The oldest and newest samples are always kept.
    EXPECT_DOUBLE_EQ(carried.front(), 0.0);
    EXPECT_DOUBLE_EQ(x.back(), 0.0);
}

TEST(ChartWidgetsReduceTest, AlignedReductionKeyedOnBandValuesKeepsASpikeUnderAFlatTop)
{
    // #1061 review: System rises from 10 to 30 at one sample while User falls from 50 to 30, so the
    // cumulative System top stays at 60. Choosing points by the band's own value keeps that spike;
    // the tops ride along as carried series.
    const ReduceFixture f;
    std::vector<double> user(ReduceFixture::COUNT, 50.0);
    std::vector<double> system(ReduceFixture::COUNT, 10.0);
    user[1234] = 30.0;
    system[1234] = 30.0;
    std::vector<double> systemTop(ReduceFixture::COUNT);
    for (std::size_t i = 0; i < systemTop.size(); ++i)
    {
        systemTop[i] = user[i] + system[i]; // 60 throughout
    }
    auto x = f.x;

    reduceAlignedSeries(x, {&user, &system}, {&systemTop}, LINE_PLOT_MAX_POINTS_DENSE, 1000.0);

    ASSERT_LE(x.size(), static_cast<std::size_t>(LINE_PLOT_MAX_POINTS_DENSE));
    EXPECT_DOUBLE_EQ(std::ranges::max(system), 30.0);
    EXPECT_DOUBLE_EQ(std::ranges::min(user), 30.0);
    // The band between User and the System top shows it: 30 thick at the spike, 10 elsewhere.
    EXPECT_TRUE(
        std::ranges::any_of(std::views::iota(std::size_t{0}, x.size()), [&](std::size_t k) { return systemTop[k] - user[k] == 30.0; }));
}

TEST(ChartWidgetsReduceTest, AlignedReductionNeverDrawsASeriesAcrossItsGap)
{
    // #1061 review: series A has two separate gaps in one bucket, and B's peak and dip fall between
    // and after them. B's picks must not give A finite points on both sides of a gap with no gap point
    // between: the drawn line would cross a missing reading.
    const ReduceFixture f;
    std::vector<double> a(ReduceFixture::COUNT, 10.0);
    std::vector<double> b(ReduceFixture::COUNT, 50.0);
    // With x anchored at 1000, samples 1479-1518 share one 4 s bucket (2 keys: 118 buckets over 300 s).
    a[1485] = std::numeric_limits<double>::quiet_NaN();
    a[1505] = std::numeric_limits<double>::quiet_NaN();
    b[1495] = 90.0;                  // between A's gaps
    b[1510] = 5.0;                   // after the second
    std::vector<double> sourceA = a; // full-resolution A, to check the reduced points against
    auto x = f.x;

    reduceAlignedSeries(x, {&a, &b}, {}, LINE_PLOT_MAX_POINTS_DENSE, 1000.0);

    ASSERT_LE(x.size(), static_cast<std::size_t>(LINE_PLOT_MAX_POINTS_DENSE));
    const auto sourceOf = [](double xv)
    {
        return static_cast<std::size_t>(std::lround((xv / 0.1) + (ReduceFixture::COUNT - 1)));
    };
    for (std::size_t k = 1; k < x.size(); ++k)
    {
        if (!std::isfinite(a[k - 1]) || !std::isfinite(a[k]))
        {
            continue;
        }
        // Two consecutive finite points of A: no missing reading of A may lie between them.
        for (std::size_t i = sourceOf(x[k - 1]); i <= sourceOf(x[k]); ++i)
        {
            EXPECT_TRUE(std::isfinite(sourceA[i])) << "A drawn across its gap at sample " << i << " (points " << k - 1 << "-" << k << ")";
        }
    }
    EXPECT_TRUE(std::ranges::any_of(a, [](double v) { return std::isnan(v); }));
}

TEST(ChartWidgetsReduceTest, AlignedReductionLeavesShortSeriesAndKeepsGaps)
{
    std::vector<double> x = {-3.0, -2.0, -1.0, 0.0};
    std::vector<double> y = {1.0, 2.0, 3.0, 4.0};
    reduceAlignedSeries(x, {&y}, {}, LINE_PLOT_MAX_POINTS_DENSE, 0.0);
    EXPECT_EQ(x.size(), 4U);

    ReduceFixture f;
    f.y[1500] = std::numeric_limits<double>::quiet_NaN(); // one missing reading
    auto gx = f.x;
    reduceAlignedSeries(gx, {&f.y}, {}, LINE_PLOT_MAX_POINTS_DENSE, 1000.0);
    EXPECT_TRUE(std::ranges::any_of(f.y, [](double v) { return std::isnan(v); }));
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

// ========== Cached reductions (#1139) ==========

TEST(ChartWidgetsReduceTest, MinMaxPointsReplayToTheSameSeriesAsTheReduction)
{
    // History charts now keep the chosen points (index + gap) and replay them each frame instead of
    // reducing the whole history again: the replay must draw exactly what the reduction would.
    ReduceFixture f;
    f.y[1234] = 99.0;
    f.y[1500] = std::numeric_limits<double>::quiet_NaN();
    f.y[2001] = -5.0;
    std::vector<double> outX(LINE_PLOT_MAX_POINTS_DENSE);
    std::vector<double> outY(LINE_PLOT_MAX_POINTS_DENSE);
    const int written =
        reduceSeriesMinMax(f.x.data(), f.y.data(), ReduceFixture::COUNT, LINE_PLOT_MAX_POINTS_DENSE, 1000.0, outX.data(), outY.data());

    std::vector<ReducedPoint> points;
    reduceSeriesMinMaxPoints(f.x.data(), f.y.data(), ReduceFixture::COUNT, LINE_PLOT_MAX_POINTS_DENSE, 1000.0, points);

    ASSERT_EQ(points.size(), static_cast<std::size_t>(written));
    for (std::size_t k = 0; k < points.size(); ++k)
    {
        const auto source = static_cast<std::size_t>(points[k].index);
        EXPECT_DOUBLE_EQ(outX[k], f.x[source]) << "point " << k;
        if (points[k].gap)
        {
            EXPECT_TRUE(std::isnan(outY[k])) << "point " << k;
        }
        else
        {
            EXPECT_DOUBLE_EQ(outY[k], f.y[source]) << "point " << k;
        }
    }
    EXPECT_TRUE(std::ranges::any_of(points, [](const ReducedPoint& p) { return p.gap; }));
}

TEST(ChartWidgetsReduceTest, MinMaxPointsOfAShortSeriesAreEverySample)
{
    const std::vector<double> x = {-3.0, -2.0, -1.0, 0.0};
    const std::vector<double> y = {1.0, 2.0, 3.0, 4.0};
    std::vector<ReducedPoint> points;
    reduceSeriesMinMaxPoints(x.data(), y.data(), 4, LINE_PLOT_MAX_POINTS_DENSE, 0.0, points);
    ASSERT_EQ(points.size(), 4U);
    for (std::size_t k = 0; k < points.size(); ++k)
    {
        EXPECT_EQ(points[k], (ReducedPoint{.index = static_cast<int>(k), .gap = false}));
    }
}

TEST(ChartWidgetsReduceTest, MinMaxPointsDoNotDependOnNow)
{
    // What makes caching them sound: x is "seconds before now" and the buckets are anchored at now,
    // so the same samples seen a few frames later -- every x shifted, the anchor shifted with it --
    // choose the same points. Only new data can change them.
    ReduceFixture f;
    f.y[777] = 42.0;
    f.y[1501] = std::numeric_limits<double>::quiet_NaN();
    constexpr double now = 5000.0;
    std::vector<ReducedPoint> first;
    reduceSeriesMinMaxPoints(f.x.data(), f.y.data(), ReduceFixture::COUNT, LINE_PLOT_MAX_POINTS_DENSE, now, first);

    for (const double later : {0.016, 0.5, 0.9})
    {
        std::vector<double> shifted = f.x;
        for (double& v : shifted)
        {
            v -= later;
        }
        std::vector<ReducedPoint> again;
        reduceSeriesMinMaxPoints(shifted.data(), f.y.data(), ReduceFixture::COUNT, LINE_PLOT_MAX_POINTS_DENSE, now + later, again);
        EXPECT_EQ(again, first) << "now advanced by " << later;
    }
}

TEST(ChartWidgetsReduceTest, AlignedPointsReplayToTheSameSeriesAsTheInPlaceReduction)
{
    ReduceFixture f;
    std::vector<double> user(ReduceFixture::COUNT, 5.0);
    std::vector<double> system(ReduceFixture::COUNT, 20.0);
    user[1234] = 60.0;
    system[2345] = 95.0;
    // Two gap runs of one series in one bucket collapse it (see AlignedReductionNeverDrawsASeriesAcrossItsGap).
    user[1485] = std::numeric_limits<double>::quiet_NaN();
    user[1505] = std::numeric_limits<double>::quiet_NaN();

    std::vector<ReducedPoint> points;
    reduceAlignedPoints<double>(
        f.x, {std::span<const double>(user), std::span<const double>(system)}, LINE_PLOT_MAX_POINTS_DENSE, 1000.0, points);

    auto x = f.x;
    auto reducedUser = user;
    auto reducedSystem = system;
    reduceAlignedSeries(x, {&reducedUser, &reducedSystem}, {}, LINE_PLOT_MAX_POINTS_DENSE, 1000.0);

    ASSERT_EQ(points.size(), x.size());
    for (std::size_t k = 0; k < points.size(); ++k)
    {
        const auto source = static_cast<std::size_t>(points[k].index);
        EXPECT_DOUBLE_EQ(x[k], f.x[source]) << "point " << k;
        if (points[k].gap)
        {
            EXPECT_TRUE(std::isnan(reducedUser[k]) && std::isnan(reducedSystem[k])) << "point " << k;
        }
        else
        {
            EXPECT_DOUBLE_EQ(reducedUser[k], user[source]) << "point " << k;
            EXPECT_DOUBLE_EQ(reducedSystem[k], system[source]) << "point " << k;
        }
    }
    EXPECT_TRUE(std::ranges::any_of(points, [](const ReducedPoint& p) { return p.gap; }));
}

TEST(ChartWidgetsReduceTest, AlignedPointsAcceptFloatSeriesAndKeepEverySampleOfAShortOne)
{
    // The system histories are float; the cached path chooses points from them without a copy.
    const std::vector<double> x = {-3.0, -2.0, -1.0, 0.0};
    const std::vector<float> y = {1.0F, 2.0F, 3.0F, 4.0F};
    std::vector<ReducedPoint> points;
    reduceAlignedPoints<float>(x, {std::span<const float>(y)}, LINE_PLOT_MAX_POINTS_DENSE, 0.0, points);
    ASSERT_EQ(points.size(), 4U);
    EXPECT_EQ(points.back(), (ReducedPoint{.index = 3, .gap = false}));

    ReduceFixture f;
    std::vector<float> longY(ReduceFixture::COUNT, 10.0F);
    longY[1234] = 70.0F;
    reduceAlignedPoints<float>(f.x, {std::span<const float>(longY)}, LINE_PLOT_MAX_POINTS_DENSE, 1000.0, points);
    ASSERT_LE(points.size(), static_cast<std::size_t>(LINE_PLOT_MAX_POINTS_DENSE));
    EXPECT_TRUE(std::ranges::any_of(points, [](const ReducedPoint& p) { return p.index == 1234; }));
}

TEST(SeriesFingerprintTest, DependsOnContentNotAddress)
{
    // #1139 review: a series copied into a buffer rebuilt every frame has a new address each frame,
    // so the cache key must come from the samples, or that chart never hits the cache.
    const std::vector<float> a{1.0F, 2.0F, 3.0F};
    const std::vector<float> copy = a; // same samples, different storage
    EXPECT_NE(a.data(), copy.data());
    EXPECT_EQ(UI::Widgets::seriesFingerprint(a.data(), 3), UI::Widgets::seriesFingerprint(copy.data(), 3));

    const std::vector<float> otherEnd{1.0F, 2.0F, 4.0F};
    const std::vector<float> swapped{3.0F, 2.0F, 1.0F};
    EXPECT_NE(UI::Widgets::seriesFingerprint(a.data(), 3), UI::Widgets::seriesFingerprint(otherEnd.data(), 3));
    EXPECT_NE(UI::Widgets::seriesFingerprint(a.data(), 3), UI::Widgets::seriesFingerprint(swapped.data(), 3));
    EXPECT_EQ(UI::Widgets::seriesFingerprint(a.data(), 0), 0U);

    const std::vector<double> d{1.0, 2.0, 3.0};
    EXPECT_EQ(UI::Widgets::seriesFingerprint(d.data(), 3), UI::Widgets::seriesFingerprint(a.data(), 3)); // float widened
}

TEST(ReducedPointsCacheTest, RebuildsOnlyWhenTheKeyChanges)
{
    ReducedPointsCache cache;
    int rebuilds = 0;
    const auto rebuild = [&rebuilds](std::vector<ReducedPoint>& out)
    {
        ++rebuilds;
        out.assign({ReducedPoint{.index = 0, .gap = false}, ReducedPoint{.index = rebuilds, .gap = false}});
    };
    const ReducedPointsCache::Key key{.generation = 7, .dataId = 1, .count = 100, .maxOut = 720};

    EXPECT_EQ(cache.points(key, rebuild)[1].index, 1);
    EXPECT_EQ(cache.points(key, rebuild)[1].index, 1); // same data: replayed, not rebuilt
    EXPECT_EQ(rebuilds, 1);

    // Each part of the key names the data: a change to any one rebuilds.
    auto newGeneration = key;
    newGeneration.generation = 8;
    EXPECT_EQ(cache.points(newGeneration, rebuild)[1].index, 2);
    auto otherSeries = newGeneration;
    otherSeries.dataId = 2;
    EXPECT_EQ(cache.points(otherSeries, rebuild)[1].index, 3);
    auto longer = otherSeries;
    longer.count = 101;
    EXPECT_EQ(cache.points(longer, rebuild)[1].index, 4);
    auto smallerBudget = longer;
    smallerBudget.maxOut = 360;
    EXPECT_EQ(cache.points(smallerBudget, rebuild)[1].index, 5);
    EXPECT_EQ(cache.points(smallerBudget, rebuild)[1].index, 5);
    EXPECT_EQ(cache.rebuildCount(), 5U);

    cache.invalidate();
    EXPECT_EQ(cache.points(smallerBudget, rebuild)[1].index, 6);
}

TEST(ReducedPointsCacheTest, GenerationZeroIsNeverCached)
{
    // A chart that names no data generation keeps the old behaviour: reduced on every call.
    ReducedPointsCache cache;
    int rebuilds = 0;
    const auto rebuild = [&rebuilds](std::vector<ReducedPoint>& out)
    {
        ++rebuilds;
        out.clear();
    };
    const ReducedPointsCache::Key uncached{.generation = 0, .dataId = 1, .count = 100, .maxOut = 720};
    std::ignore = cache.points(uncached, rebuild);
    std::ignore = cache.points(uncached, rebuild);
    EXPECT_EQ(rebuilds, 2);
}

TEST(ReducedPointsCacheTest, NewDataGenerationsAreNonZeroAndNeverRepeat)
{
    const std::uint64_t first = nextChartDataGeneration();
    const std::uint64_t second = nextChartDataGeneration();
    EXPECT_NE(first, 0U);
    EXPECT_GT(second, first);
}

TEST(HistoryChartConfigTest, DataGenerationDefaultsToUncachedAndCanBeSet)
{
    const HistoryChartConfig plain = percentHistoryConfig("##Test", -60.0, 0.0);
    EXPECT_EQ(plain.dataGeneration, 0U);
    EXPECT_EQ(withDataGeneration(plain, 42).dataGeneration, 42U);
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
    EXPECT_EQ(std::string(buf), "100.0 B/s");

    len = formatAxisBytesPerSec(2048.0, buf, static_cast<int>(sizeof(buf)), nullptr);
    EXPECT_GT(len, 0);
    EXPECT_EQ(std::string(buf), "2.0 KB/s");
}

TEST(ChartWidgetsFormattersTest, FormatAxisBytesUsesBinaryUnitsWithoutRateSuffix)
{
    // Matches UI::Format::formatBytes (binary units) so the GPU Memory axis agrees with its tooltip (#1023).
    char buf[32]{};
    int len = formatAxisBytes(512.0, buf, static_cast<int>(sizeof(buf)), nullptr);
    EXPECT_GT(len, 0);
    EXPECT_EQ(std::string(buf), "512.0 B");

    len = formatAxisBytes(1536.0, buf, static_cast<int>(sizeof(buf)), nullptr);
    EXPECT_GT(len, 0);
    EXPECT_EQ(std::string(buf), "1.5 KB");

    len = formatAxisBytes(1.5 * 1024.0 * 1024.0 * 1024.0, buf, static_cast<int>(sizeof(buf)), nullptr);
    EXPECT_GT(len, 0);
    EXPECT_EQ(std::string(buf), "1.5 GB");

    len = formatAxisBytes(-0.1, buf, static_cast<int>(sizeof(buf)), nullptr);
    EXPECT_GT(len, 0);
    EXPECT_EQ(std::string(buf), "0.0 B");
}

TEST(ChartWidgetsFormattersTest, FormatAxisBytesPerSecClampsTinyNegativeToZero)
{
    char buf[32]{};
    const int len = formatAxisBytesPerSec(-0.1, buf, static_cast<int>(sizeof(buf)), nullptr);
    EXPECT_GT(len, 0);
    EXPECT_EQ(std::string(buf), "0.0 B/s");
}

TEST(ChartWidgetsFormattersTest, FormatAxisWattsUsesWAndMilliwatts)
{
    char buf[32]{};
    int len = formatAxisWatts(10.0, buf, static_cast<int>(sizeof(buf)), nullptr);
    EXPECT_GT(len, 0);
    EXPECT_EQ(std::string(buf), "10.0 W");

    len = formatAxisWatts(0.5, buf, static_cast<int>(sizeof(buf)), nullptr);
    EXPECT_GT(len, 0);
    EXPECT_EQ(std::string(buf), "500.0 mW");
}

TEST(ChartWidgetsFormattersTest, FormatAxisWattsClampsTinyNegativeToZeroWatts)
{
    char buf[32]{};
    const int len = formatAxisWatts(-0.00001, buf, static_cast<int>(sizeof(buf)), nullptr);
    EXPECT_GT(len, 0);
    EXPECT_EQ(std::string(buf), "0.0 W");
}

TEST(ChartWidgetsFormattersTest, FormatAxisPercentIsWholeFromTenPercent)
{
    char buf[32]{};
    const int len = formatAxisPercent(12.34, buf, static_cast<int>(sizeof(buf)), nullptr);
    EXPECT_GT(len, 0);
    EXPECT_EQ(std::string(buf), "12%");
}

TEST(ChartWidgetsFormattersTest, FormatAxisPercentClampsTinyNegativeToZero)
{
    char buf[32]{};
    const int len = formatAxisPercent(-0.01, buf, static_cast<int>(sizeof(buf)), nullptr);
    EXPECT_GT(len, 0);
    EXPECT_EQ(std::string(buf), "0%");
}

// #1195: a percent axis can scale down to 5 %, so a small tick must keep its value.
TEST(ChartWidgetsFormattersTest, FormatAxisPercentKeepsSmallTicks)
{
    char buf[32]{};
    const int len = formatAxisPercent(0.2, buf, static_cast<int>(sizeof(buf)), nullptr);
    EXPECT_GT(len, 0);
    EXPECT_EQ(std::string(buf), "0.2%");
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
    EXPECT_EQ(std::string(buf), "5.0 MB/s");

    len = formatAxisBytesPerSec(2.0 * 1024.0 * 1024.0 * 1024.0, buf, static_cast<int>(sizeof(buf)), nullptr);
    EXPECT_GT(len, 0);
    EXPECT_EQ(std::string(buf), "2.0 GB/s");
}

// #1202: every axis formatter is the value formatter for its quantity, so an axis tick reads
// exactly like the tooltip and table cell beside it.
TEST(ChartWidgetsFormattersTest, AxisFormattersMatchValueFormatters)
{
    std::array<char, 32> buf{};
    const auto axis = [&buf](ImPlotFormatter formatter, double value, void* userData = nullptr)
    {
        EXPECT_GT(formatter(value, buf.data(), static_cast<int>(buf.size()), userData), 0);
        return std::string(buf.data());
    };
    for (const double bytes : {1.0, 512.0, 1536.0, 3.25 * 1024.0 * 1024.0, 1.5 * 1024.0 * 1024.0 * 1024.0})
    {
        EXPECT_EQ(axis(formatAxisBytes, bytes), Format::formatBytes(bytes));
        EXPECT_EQ(axis(formatAxisBytesPerSec, bytes), Format::formatBytesPerSec(bytes));
    }
    for (const double watts : {0.0, 0.0125, 0.5, 1.0, 45.0, 123.45})
    {
        EXPECT_EQ(axis(formatAxisWatts, watts), Format::formatWatts(watts));
    }
    for (const double percent : {0.0, 0.04, -0.04, 0.2, 2.5, 9.9, 10.0, 42.0, 100.0})
    {
        EXPECT_EQ(axis(formatAxisPercent, percent), Format::formatPercent(percent));
    }
}

// A byte axis is labelled in the one unit its step is in: 0.5 GB, not 512.0 MB, on a 0-2 GB axis.
TEST(ChartWidgetsFormattersTest, ByteAxisUsesTheUnitItIsGiven)
{
    std::array<char, 32> buf{};
    const int size = static_cast<int>(buf.size());
    constexpr double GIB = 1024.0 * 1024.0 * 1024.0;
    EXPECT_GT(formatAxisBytes(0.5 * GIB, buf.data(), size, byteAxisUserData(Format::BYTE_UNIT_GB)), 0);
    EXPECT_EQ(std::string(buf.data()), "0.5 GB");
    EXPECT_GT(formatAxisBytesPerSec(0.0, buf.data(), size, byteAxisUserData(Format::BYTE_UNIT_MB)), 0);
    EXPECT_EQ(std::string(buf.data()), "0.0 MB/s");
    // Without a unit each tick picks its own, as before.
    EXPECT_GT(formatAxisBytes(0.5 * GIB, buf.data(), size, nullptr), 0);
    EXPECT_EQ(std::string(buf.data()), "512.0 MB");
}

TEST(ChartWidgetsFormattersTest, OnlyTheByteFormattersStepInBinaryUnits)
{
    EXPECT_TRUE(isByteAxisFormatter(formatAxisBytes));
    EXPECT_TRUE(isByteAxisFormatter(formatAxisBytesPerSec));
    EXPECT_FALSE(isByteAxisFormatter(formatAxisWatts));
    EXPECT_FALSE(isByteAxisFormatter(formatAxisPercent));
    EXPECT_FALSE(isByteAxisFormatter(formatAxisLocalized));
}

// ========== Tooltip rows (#1008, #1020) ==========

TEST(ChartWidgetsTest, TooltipRowIsLabelColonValue)
{
    EXPECT_EQ(formatTooltipRow("Read", "1.5 MB/s"), "Read: 1.5 MB/s");
    EXPECT_EQ(formatTooltipRow("Page Faults/s", "12/s"), "Page Faults/s: 12/s");
}

TEST(ChartWidgetsTest, NowBarTooltipRowReadsLikeTheTooltipRow)
{
    // #1171: a NowBar's tooltip is held in place; it must say exactly what the std::string row did.
    EXPECT_EQ(tooltipRowText("Read", "1.5 MB/s").view(), formatTooltipRow("Read", "1.5 MB/s"));
    const NowBar bar{.valueText = "45%",
                     .label = "Memory",
                     .tooltipText = tooltipRowText("Memory", "45% (3.6 GB / 8.0 GB)"),
                     .value01 = 0.45,
                     .color = {}};
    EXPECT_EQ(selectNowBarTooltip(bar), "Memory: 45% (3.6 GB / 8.0 GB)");
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

TEST(ChartWidgetsTest, FiniteRunsAcceptAMutableCallback)
{
    // A stateful callback with a non-const operator() must still bind (#1224 review).
    const std::vector<float> values{1.0F, std::numeric_limits<float>::quiet_NaN(), 2.0F};
    int runCount = 0;
    UI::Widgets::forEachFiniteRun(values.data(),
                                  static_cast<int>(values.size()),
                                  [count = 0, &runCount](int /*start*/, int /*length*/) mutable { runCount = ++count; });
    EXPECT_EQ(runCount, 2);
}

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
    const auto axis = buildTimeAxis(timestamps, 3, 25.0);

    ASSERT_EQ(axis.size(), 3U);
    EXPECT_DOUBLE_EQ(axis[0], -15.0);
    EXPECT_DOUBLE_EQ(axis[1], -5.0);
    EXPECT_DOUBLE_EQ(axis[2], 5.0);
}

TEST(ChartWidgetsTimeAxisTest, BuildTimeAxisDoublesRespectsDesiredCount)
{
    const std::vector<double> timestamps{1.0, 3.0, 7.0, 9.0};
    const auto axis = buildTimeAxis(timestamps, 2, 10.0);

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

// ========== historyChartBeginPlotFlags (#1198: the value strip is every chart's only key) ==========

TEST(ChartWidgetsTest, DefaultPlotFlagsHideImPlotsMouseReadout)
{
    // Every history chart has its own tooltip; ImPlot's raw cursor coordinates were a second,
    // unlabelled readout of the same point (#1039).
    EXPECT_TRUE((PLOT_FLAGS_DEFAULT & ImPlotFlags_NoMouseText) != 0);
}

// ========== Motion-driven frame requests (#1125) ==========

TEST(HistoryChartMotionTest, EaseFramesAreRequestedOnlyForAVisibleChartThisFrame)
{
    // #1281 review: an axis eased before its chart is drawn must not keep the app at full rate when the
    // chart turns out to be clipped (BeginPlot false), nor carry a stale request to a later frame.
    using UI::Widgets::shouldRequestEaseFrames;
    EXPECT_TRUE(shouldRequestEaseFrames(42, 42, true));
    EXPECT_FALSE(shouldRequestEaseFrames(42, 42, false)); // clipped chart
    EXPECT_FALSE(shouldRequestEaseFrames(41, 42, true));  // from an earlier frame
    EXPECT_FALSE(shouldRequestEaseFrames(-1, 42, true));  // nothing pending
}

TEST(HistoryChartMotionTest, ScrollSpeedIsPlotWidthOverWindowSeconds)
{
    // The default 300 s window across 1000 px scrolls ~3.3 px/s: far too slow to need 60 fps.
    EXPECT_NEAR(historyChartScrollPixelsPerSecond(1000.0, -300.0, 0.0), 1000.0 / 300.0, 1e-12);
    // A 10 s window over the same width scrolls 100 px/s.
    EXPECT_DOUBLE_EQ(historyChartScrollPixelsPerSecond(1000.0, -10.0, 0.0), 100.0);
    // A scrolled-back window moves at the same speed as one ending at "now".
    EXPECT_DOUBLE_EQ(historyChartScrollPixelsPerSecond(1000.0, -70.0, -60.0), 100.0);
    // An empty or inverted span, or no width, asks for nothing.
    EXPECT_DOUBLE_EQ(historyChartScrollPixelsPerSecond(1000.0, 0.0, 0.0), 0.0);
    EXPECT_DOUBLE_EQ(historyChartScrollPixelsPerSecond(1000.0, 0.0, -5.0), 0.0);
    EXPECT_DOUBLE_EQ(historyChartScrollPixelsPerSecond(0.0, -300.0, 0.0), 0.0);
}

TEST(NowBarMotionTest, SettledBarsStopAskingForFrames)
{
    // A NowBar 100 px tall easing 0.30 -> 0.36 in a 16 ms frame moves 375 px/s.
    EXPECT_NEAR(nowBarMotionPixelsPerSecond(0.30, 0.36, 100.0, 0.016), 375.0, 1e-9);
    // Falling counts the same as rising.
    EXPECT_NEAR(nowBarMotionPixelsPerSecond(0.36, 0.30, 100.0, 0.016), 375.0, 1e-9);
    // Converged: no motion, no frames (it used to hold the loop at 60 fps forever).
    EXPECT_DOUBLE_EQ(nowBarMotionPixelsPerSecond(0.5, 0.5, 100.0, 0.016), 0.0);
    // Unknown frame time or height: nothing to go on, so nothing asked.
    EXPECT_DOUBLE_EQ(nowBarMotionPixelsPerSecond(0.3, 0.6, 100.0, 0.0), 0.0);
    EXPECT_DOUBLE_EQ(nowBarMotionPixelsPerSecond(0.3, 0.6, 0.0, 0.016), 0.0);
}

TEST(HistoryChartConfigTest, BeginPlotFlagsNeverShowImPlotsLegend)
{
    // The value strip above every chart is its only key (#1198).
    EXPECT_EQ(historyChartBeginPlotFlags(PLOT_FLAGS_DEFAULT), PLOT_FLAGS_DEFAULT | ImPlotFlags_NoLegend);
}

TEST(HistoryChartConfigTest, BeginPlotFlagsPreserveOtherConfiguredBits)
{
    const ImPlotFlags configured = PLOT_FLAGS_DEFAULT | ImPlotFlags_NoTitle;
    const ImPlotFlags result = historyChartBeginPlotFlags(configured);
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

// ========== Series encoding (#1198) ==========

TEST(SeriesStyleTest, OnlyThePrimaryFills)
{
    EXPECT_TRUE(seriesStyle(SeriesRole::Primary).fill);
    EXPECT_FALSE(seriesStyle(SeriesRole::Reference).fill);
    for (std::size_t i = 0; i < 6; ++i)
    {
        EXPECT_FALSE(seriesStyle(SeriesRole::Secondary, i).fill) << i;
    }
}

TEST(SeriesStyleTest, RolesDifferByWeightNotJustColour)
{
    const SeriesStyle primary = seriesStyle(SeriesRole::Primary);
    const SeriesStyle secondary = seriesStyle(SeriesRole::Secondary);
    const SeriesStyle reference = seriesStyle(SeriesRole::Reference);
    EXPECT_GT(primary.lineWeightPx, secondary.lineWeightPx);
    EXPECT_GT(secondary.lineWeightPx, reference.lineWeightPx);
    EXPECT_EQ(primary.marker, ImPlotMarker_None);
}

// #1301 review: the two Network totals are both references; they must differ by more than colour.
TEST(SeriesStyleTest, EachReferenceHasItsOwnMarkerUnlikeAnySecondaries)
{
    const SeriesStyle sent = seriesStyle(SeriesRole::Reference, 0);
    const SeriesStyle received = seriesStyle(SeriesRole::Reference, 1);
    EXPECT_NE(sent.marker, ImPlotMarker_None);
    EXPECT_NE(received.marker, ImPlotMarker_None);
    EXPECT_NE(sent.marker, received.marker);
    EXPECT_NE(sent.markerPhase, received.markerPhase);
    for (std::size_t i = 0; i < SECONDARY_SERIES_MARKERS.size(); ++i)
    {
        const SeriesStyle secondary = seriesStyle(SeriesRole::Secondary, i);
        EXPECT_NE(sent.marker, secondary.marker) << i;
        EXPECT_NE(received.marker, secondary.marker) << i;
        EXPECT_NE(sent.markerPhase, secondary.markerPhase) << i;
        EXPECT_NE(received.markerPhase, secondary.markerPhase) << i;
    }
}

TEST(SeriesStyleTest, EachSecondaryOfAChartHasItsOwnMarkerAndPhase)
{
    for (std::size_t i = 0; i < SECONDARY_SERIES_MARKERS.size(); ++i)
    {
        const SeriesStyle a = seriesStyle(SeriesRole::Secondary, i);
        EXPECT_NE(a.marker, ImPlotMarker_None);
        EXPECT_GE(a.markerPhase, 0.0);
        EXPECT_LT(a.markerPhase, 1.0);
        for (std::size_t j = i + 1; j < SECONDARY_SERIES_MARKERS.size(); ++j)
        {
            const SeriesStyle b = seriesStyle(SeriesRole::Secondary, j);
            EXPECT_NE(a.marker, b.marker) << i << " vs " << j;
            EXPECT_NE(a.markerPhase, b.markerPhase) << i << " vs " << j;
        }
    }
}

namespace
{
std::vector<int> markerSamples(std::span<const double> x, std::span<const double> y, double anchor, double interval, double phase)
{
    std::vector<int> out;
    forEachMarkerSample(x.data(), y.data(), UI::Format::checkedCount(x.size()), anchor, interval, phase, [&](int i) { out.push_back(i); });
    return out;
}
} // namespace

TEST(ForEachMarkerSampleTest, MarksTheFirstSampleAfterEachBoundaryButNotTheOldestBucket)
{
    // One sample a second from -10 s to 0 s; boundaries every 4 s of absolute time (anchor 100).
    std::vector<double> x;
    for (int s = -10; s <= 0; ++s)
    {
        x.push_back(static_cast<double>(s));
    }
    const std::vector<double> y(x.size(), 1.0);
    // Absolute times 90..100: boundaries at 92, 96 and 100 -> indices 2, 6 and 10.
    EXPECT_EQ(markerSamples(x, y, 100.0, 4.0, 0.0), (std::vector<int>{2, 6, 10}));
}

TEST(ForEachMarkerSampleTest, StaysOnTheSameSamplesAsTheChartScrolls)
{
    // The same absolute samples seen one frame later: x shifts by -0.5 s, the anchor by +0.5 s.
    const std::vector<double> x0{-6.0, -5.0, -4.0, -3.0, -2.0, -1.0, 0.0};
    std::vector<double> x1;
    x1.reserve(x0.size());
    for (const double v : x0)
    {
        x1.push_back(v - 0.5);
    }
    const std::vector<double> y(x0.size(), 1.0);
    EXPECT_EQ(markerSamples(x0, y, 50.0, 3.0, 0.25), markerSamples(x1, y, 50.5, 3.0, 0.25));
}

TEST(ForEachMarkerSampleTest, PhaseShiftsTheGrid)
{
    const std::vector<double> x{0.0, 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0};
    const std::vector<double> y(x.size(), 1.0);
    EXPECT_EQ(markerSamples(x, y, 0.0, 4.0, 0.0), (std::vector<int>{4}));
    // A phase of a half moves the boundaries to 2 and 6.
    EXPECT_EQ(markerSamples(x, y, 0.0, 4.0, 0.5), (std::vector<int>{2, 6}));
}

TEST(ForEachMarkerSampleTest, SkipsGapsAndDegenerateIntervals)
{
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const std::vector<double> x{0.0, 1.0, 2.0, 3.0, 4.0, 5.0};
    const std::vector<double> y{1.0, 1.0, nan, 1.0, 1.0, 1.0};
    // The boundary at 2 falls on a gap: the next finite sample takes the marker.
    EXPECT_EQ(markerSamples(x, y, 0.0, 2.0, 0.0), (std::vector<int>{3, 4}));
    EXPECT_TRUE(markerSamples(x, y, 0.0, 0.0, 0.0).empty());
    EXPECT_TRUE(markerSamples(x, y, 0.0, nan, 0.0).empty());
    EXPECT_TRUE(markerSamples({}, {}, 0.0, 2.0, 0.0).empty());
    const std::vector<double> allGaps(x.size(), nan);
    EXPECT_TRUE(markerSamples(x, allGaps, 0.0, 2.0, 0.0).empty());
}

// #1301 review: a history that has not crossed a boundary yet still gets one marker, on its oldest
// finite sample, so a new chart's secondaries are told apart from their first samples.
TEST(ForEachMarkerSampleTest, AHistoryWithinOneBucketPlacesNoneButHasAFallbackSample)
{
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const std::vector<double> x{0.0, 1.0, 2.0, 3.0};
    const std::vector<double> y{nan, 1.0, 1.0, 1.0};
    EXPECT_TRUE(markerSamples(x, y, 0.0, 30.0, 0.0).empty());
    EXPECT_EQ(fallbackMarkerSample(y.data(), UI::Format::checkedCount(y.size())), 1);
    const std::vector<double> allGaps(x.size(), nan);
    EXPECT_EQ(fallbackMarkerSample(allGaps.data(), UI::Format::checkedCount(allGaps.size())), -1);
    EXPECT_EQ(fallbackMarkerSample(y.data(), 0), -1);
}

// #1301 review: the boundaries are found by binary search instead of a walk of the whole history.
// The marked samples are those of the walk: the first finite sample whose bucket differs from the
// previous finite sample's.
TEST(ForEachMarkerSampleTest, MatchesAWalkOfEverySample)
{
    const double nan = std::numeric_limits<double>::quiet_NaN();
    std::vector<double> x;
    std::vector<double> y;
    for (int i = 0; i < 2000; ++i)
    {
        x.push_back((static_cast<double>(i) * 0.1) - 200.0);
        // Gaps of varying length, some spanning a boundary.
        y.push_back(((i % 97) < 13 || (i % 211) < 40) ? nan : 1.0);
    }
    for (const double phase : {0.0, 0.25, 0.7})
    {
        for (const double interval : {0.05, 1.0, 7.3, 30.0, 500.0})
        {
            std::vector<int> walk;
            std::optional<std::int64_t> last;
            for (int i = 0; i < static_cast<int>(x.size()); ++i)
            {
                const auto index = static_cast<std::size_t>(i);
                if (!std::isfinite(y[index]))
                {
                    continue;
                }
                const auto bucket = static_cast<std::int64_t>(std::floor(((x[index] + 1234.5) / interval) + phase));
                if (last.has_value() && bucket != *last)
                {
                    walk.push_back(i);
                }
                last = bucket;
            }
            EXPECT_EQ(markerSamples(x, y, 1234.5, interval, phase), walk) << "interval " << interval << " phase " << phase;
        }
    }
}

// ========== Legend layout (#1275) ==========

// #1301 review: one entry built from an OS adapter description can be wider than the frame on its
// own, so such names are fitted to the chart width for series labels.
namespace
{
// 10 px per code point, like a monospace font.
float tenPerCodePoint(std::string_view text)
{
    std::size_t codePoints = 0;
    for (const char c : text)
    {
        codePoints += ((static_cast<unsigned char>(c) & 0xC0U) != 0x80U) ? 1U : 0U;
    }
    return 10.0F * static_cast<float>(codePoints);
}
} // namespace

// #1301: the value strip is the chart's only key, so each swatch shows its series' marker shape,
// looked up by label among the markers the chart recorded this frame.
TEST(StripMarkersTest, ASeriesMarkerIsFoundByLabel)
{
    const std::array markers{Detail::SeriesMarker{.label = "User", .marker = ImPlotMarker_Circle},
                             Detail::SeriesMarker{.label = "System", .marker = ImPlotMarker_Square}};
    EXPECT_EQ(Detail::markerForLabel(markers, "User"), ImPlotMarker_Circle);
    EXPECT_EQ(Detail::markerForLabel(markers, "System"), ImPlotMarker_Square);
    // A series without a marker (the chart's primary), or one the chart did not draw.
    EXPECT_EQ(Detail::markerForLabel(markers, "Total"), ImPlotMarker_None);
    EXPECT_EQ(Detail::markerForLabel({}, "User"), ImPlotMarker_None);
}

TEST(StripSlotsTest, StaleLayoutsAreDroppedOnlyOnceThereAreMany)
{
    std::unordered_map<ImGuiID, Detail::StripSlots> byLayout;
    for (ImGuiID id = 1; id <= Detail::STRIP_SLOTS_PRUNE_ABOVE; ++id)
    {
        byLayout[id].lastUsed = 0.0;
    }
    // At the limit nothing is dropped, however old.
    Detail::pruneStaleStripSlots(byLayout, 1000.0);
    EXPECT_EQ(byLayout.size(), Detail::STRIP_SLOTS_PRUNE_ABOVE);
    // Past it, layouts unused for longer than the stale time go; recently used ones stay.
    byLayout[1000].lastUsed = 1000.0;
    byLayout[1001].lastUsed = 1000.0 - Detail::STRIP_SLOTS_STALE_SECONDS + 1.0;
    Detail::pruneStaleStripSlots(byLayout, 1000.0);
    EXPECT_EQ(byLayout.size(), 2U);
    EXPECT_TRUE(byLayout.contains(1000));
    EXPECT_TRUE(byLayout.contains(1001));
}

// #1301: a right-aligned strip moved every time a value's text changed width.
TEST(StripSlotTest, AWiderValueWidensItsSlotAtOnce)
{
    Detail::StripSlot slot;
    EXPECT_FLOAT_EQ(Detail::settleStripSlot(slot, 80.0F, 0.0, 3.0), 80.0F);
    EXPECT_FLOAT_EQ(Detail::settleStripSlot(slot, 95.0F, 0.1, 3.0), 95.0F);
}

TEST(StripSlotTest, ANarrowerValueKeepsItsSlotUntilTheDelayHasPassed)
{
    Detail::StripSlot slot;
    static_cast<void>(Detail::settleStripSlot(slot, 95.0F, 0.0, 3.0));
    EXPECT_FLOAT_EQ(Detail::settleStripSlot(slot, 80.0F, 1.0, 3.0), 95.0F);
    EXPECT_FLOAT_EQ(Detail::settleStripSlot(slot, 80.0F, 3.9, 3.0), 95.0F);
    EXPECT_FLOAT_EQ(Detail::settleStripSlot(slot, 80.0F, 4.0, 3.0), 80.0F);
}

TEST(StripSlotTest, AValueThatWidensAgainRestartsTheDelay)
{
    Detail::StripSlot slot;
    static_cast<void>(Detail::settleStripSlot(slot, 95.0F, 0.0, 3.0));
    static_cast<void>(Detail::settleStripSlot(slot, 80.0F, 1.0, 3.0));      // narrower from 1 s
    static_cast<void>(Detail::settleStripSlot(slot, 95.0F, 2.0, 3.0));      // back to full width
    EXPECT_FLOAT_EQ(Detail::settleStripSlot(slot, 80.0F, 3.0, 3.0), 95.0F); // narrower again from 3 s
    EXPECT_FLOAT_EQ(Detail::settleStripSlot(slot, 80.0F, 5.0, 3.0), 95.0F);
    EXPECT_FLOAT_EQ(Detail::settleStripSlot(slot, 80.0F, 6.0, 3.0), 80.0F);
}

TEST(FitSeriesNameTest, ANameThatFitsIsKeptWhole)
{
    EXPECT_EQ(fitSeriesName("Wi-Fi", 50.0F, tenPerCodePoint), "Wi-Fi");
    EXPECT_EQ(fitSeriesName("", 0.0F, tenPerCodePoint), "");
}

TEST(FitSeriesNameTest, AWiderNameIsCutToTheWidestPrefixThatFitsWithItsEllipsis)
{
    // 60 px for "abcdefgh" (80 px): "abcde…" is 60 px.
    EXPECT_EQ(fitSeriesName("abcdefgh", 60.0F, tenPerCodePoint), "abcde\u2026");
    EXPECT_EQ(fitSeriesName("abcdefgh", 65.0F, tenPerCodePoint), "abcde\u2026");
    EXPECT_EQ(fitSeriesName("abcdefgh", 79.0F, tenPerCodePoint), "abcdef\u2026");
    EXPECT_EQ(fitSeriesName("abcdefgh", 80.0F, tenPerCodePoint), "abcdefgh");
    // Room for nothing but the ellipsis, or not even that.
    EXPECT_EQ(fitSeriesName("abcdefgh", 15.0F, tenPerCodePoint), "\u2026");
    EXPECT_EQ(fitSeriesName("abcdefgh", -5.0F, tenPerCodePoint), "\u2026");
}

TEST(FitSeriesNameTest, CutsOnlyAtCodePointBoundaries)
{
    EXPECT_EQ(fitSeriesName("\u00e9\u00e9\u00e9\u00e9\u00e9", 30.0F, tenPerCodePoint), "\u00e9\u00e9\u2026");
    EXPECT_EQ(fitSeriesName("\u00e9\u00e9\u00e9", 30.0F, tenPerCodePoint), "\u00e9\u00e9\u00e9");
}

// ========== Grid cells' time axis (#1206) ==========

TEST(HistoryChartXAxisFlagsTest, GridCellsDropTheTimeTickLabelsOnly)
{
    EXPECT_EQ(historyChartXAxisFlags(true), X_AXIS_FLAGS_DEFAULT);
    const ImPlotAxisFlags cell = historyChartXAxisFlags(false);
    EXPECT_NE(cell & ImPlotAxisFlags_NoTickLabels, 0);
    // Gridlines and tick marks stay, so a cell's samples can still be placed in time.
    EXPECT_EQ(cell & (ImPlotAxisFlags_NoGridLines | ImPlotAxisFlags_NoTickMarks), 0);
    EXPECT_EQ(cell & ~ImPlotAxisFlags_NoTickLabels, X_AXIS_FLAGS_DEFAULT);
}

TEST(HistoryChartConfigTest, ChartsLabelTheirTimeAxisByDefault)
{
    EXPECT_TRUE(HistoryChartConfig{}.timeAxisLabels);
}
} // namespace
} // namespace UI::Widgets
