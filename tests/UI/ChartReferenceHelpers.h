#pragma once

/// @file ChartReferenceHelpers.h
/// @brief Reference implementations of the history-chart reductions and hold-to-now, for tests only (#3116).
///
/// The charts draw from source indices a ReducedPointsCache keeps (reduceSeriesMinMaxPoints(),
/// reduceAlignedPoints()) and hold their series to "now" in two halves (appendHeldValues(),
/// holdTimeToNow()). These helpers do the same work the older way, writing or compacting values
/// straight into buffers, through the same point selection (forEachStrideReducedPoint(),
/// forEachMinMaxReducedPoint(), forEachAlignedReducedPoint()). They used to live in ChartWidgets.h,
/// which no longer calls them; the tests keep them as reference semantics, so the peak, gap,
/// alignment and hold assertions stay readable and the cached drawing path can be checked against
/// them (#2937). Nothing under src/ may include this header.

#include "UI/ChartWidgets.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <initializer_list>
#include <limits>
#include <span>
#include <type_traits>
#include <vector>

namespace UI::Widgets::TestReference
{

/// Writes the point a reduction kept as output point `written` of `outX`/`outY`: the source sample,
/// or NaN in y for a gap point.
template<typename TX, typename TY>
inline void writeReducedPoint(const TX* xData, const TY* yData, int sourceIdx, bool gap, int written, TX* outX, TY* outY)
{
    outX[written] = xData[sourceIdx];
    if constexpr (std::is_floating_point_v<TY>)
    {
        outY[written] = gap ? std::numeric_limits<TY>::quiet_NaN() : yData[sourceIdx];
    }
    else
    {
        outY[written] = yData[sourceIdx];
    }
}

/// Stride-reduce `count` samples to `outCount` (> 1, < count) points in `outX`/`outY`, keeping gaps:
/// forEachStrideReducedPoint()'s points, with NaN in y at a gap point.
template<typename TX, typename TY>
inline void reduceSeriesKeepingGaps(const TX* xData, const TY* yData, int count, int outCount, TX* outX, TY* outY)
{
    int written = 0;
    forEachStrideReducedPoint(yData,
                              count,
                              outCount,
                              [&](int sourceIdx, bool gap)
                              {
                                  writeReducedPoint(xData, yData, sourceIdx, gap, written, outX, outY);
                                  ++written;
                              });
}

/// Reduce `count` samples to at most `maxOut` points in `outX`/`outY` (each must hold `maxOut`),
/// keeping peaks and gaps; see forEachMinMaxReducedPoint() for how the points are chosen.
///
/// @return Points written to outX/outY.
template<typename TX, typename TY>
[[nodiscard]] inline int reduceSeriesMinMax(const TX* xData, const TY* yData, int count, int maxOut, double xOffset, TX* outX, TY* outY)
{
    int written = 0;
    return forEachMinMaxReducedPoint(xData,
                                     yData,
                                     count,
                                     maxOut,
                                     xOffset,
                                     [&](int sourceIdx, bool gap)
                                     {
                                         writeReducedPoint(xData, yData, sourceIdx, gap, written, outX, outY);
                                         ++written;
                                     });
}

/// Reduce series that share one x axis to at most `maxOut` common points, in place: the points
/// forEachAlignedReducedPoint() keeps, applied to `x`, every `keyed` series and every `carried`
/// series (drawn alongside but not used to choose points), with NaN in every series at a gap point.
/// Series no longer than `maxOut` are left unchanged. Every series must be as long as `x`.
inline void reduceAlignedSeries(std::vector<double>& x,
                                std::initializer_list<std::vector<double>*> keyed,
                                std::initializer_list<std::vector<double>*> carried,
                                int maxOut,
                                double xOffset)
{
    if (!alignedReductionApplies(x.size(), maxOut, keyed.size()))
    {
        return;
    }

    std::array<std::span<const double>, MAX_ALIGNED_KEY_SERIES> keyedSpans{};
    std::size_t keyedCount = 0;
    for (const auto* series : keyed)
    {
        keyedSpans[keyedCount++] = *series;
    }

    // Keeps source index `pick` as output point `written`. Picks ascend and each is at or after its
    // output slot, so compacting in place never overwrites a sample still to be read.
    // With `asGap`, every series gets NaN there instead of its sample.
    int written = 0;
    const auto keep = [&](int pick, bool asGap)
    {
        const auto copy = [&](std::vector<double>& series)
        {
            series[static_cast<std::size_t>(written)] =
                asGap ? std::numeric_limits<double>::quiet_NaN() : series[static_cast<std::size_t>(pick)];
        };
        x[static_cast<std::size_t>(written)] = x[static_cast<std::size_t>(pick)];
        for (auto* series : keyed)
        {
            copy(*series);
        }
        for (auto* series : carried)
        {
            copy(*series);
        }
        ++written;
    };
    forEachAlignedReducedPoint<double>(x, std::span<const std::span<const double>>(keyedSpans.data(), keyedCount), maxOut, xOffset, keep);

    x.resize(static_cast<std::size_t>(written));
    for (auto* series : keyed)
    {
        series->resize(static_cast<std::size_t>(written));
    }
    for (auto* series : carried)
    {
        series->resize(static_cast<std::size_t>(written));
    }
}

/// holdLastValueToNow() for series drawn together on one time axis @p x -- a stacked chart's band
/// edges -- so they all reach "now", or none does. Each series gets its own last value repeated
/// (a trailing gap stays a gap: NaN repeated); nothing is added if any series is not as long as x.
/// The reference semantics for the two halves the charts use, appendHeldValues() and holdTimeToNow()
/// (#843, #2937).
template<typename T> inline void holdLastValuesToNow(std::vector<T>& x, std::initializer_list<std::vector<T>*> ys, double maxHoldSeconds)
{
    if (x.empty() || !lastSampleHoldsToNow(static_cast<double>(x.back()), maxHoldSeconds) ||
        std::ranges::any_of(ys, [&x](const std::vector<T>* y) { return y->size() != x.size(); }))
    {
        return;
    }
    x.push_back(T{0});
    for (std::vector<T>* y : ys)
    {
        y->push_back(y->back());
    }
}

} // namespace UI::Widgets::TestReference
