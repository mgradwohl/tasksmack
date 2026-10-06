// NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
#include "Domain/Numeric.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>

namespace Domain::Numeric
{
namespace
{

// ========== toDouble Tests ==========

TEST(NumericTest, ToDoubleFromInt)
{
    EXPECT_DOUBLE_EQ(toDouble(42), 42.0);
    EXPECT_DOUBLE_EQ(toDouble(-42), -42.0);
    EXPECT_DOUBLE_EQ(toDouble(0), 0.0);
}

TEST(NumericTest, ToDoubleFromUint64)
{
    EXPECT_DOUBLE_EQ(toDouble(std::uint64_t{1000000}), 1000000.0);
    EXPECT_DOUBLE_EQ(toDouble(std::uint64_t{0}), 0.0);
}

TEST(NumericTest, ToDoubleFromFloat)
{
    EXPECT_DOUBLE_EQ(toDouble(3.14F), static_cast<double>(3.14F));
    EXPECT_DOUBLE_EQ(toDouble(-1.5F), -1.5);
}

// ========== counterDelta/counterRate Tests ==========

TEST(NumericTest, CounterDeltaHandlesIncreaseAndReset)
{
    EXPECT_EQ(counterDelta(std::uint64_t{125}, std::uint64_t{100}), 25);
    EXPECT_EQ(counterDelta(std::uint64_t{75}, std::uint64_t{100}), 0);
}

TEST(NumericTest, CounterRateUsesElapsedTime)
{
    EXPECT_DOUBLE_EQ(counterRate(std::uint64_t{150}, std::uint64_t{100}, 2.0), 25.0);
}

TEST(NumericTest, CounterRateReturnsZeroForResetOrInvalidElapsedTime)
{
    EXPECT_DOUBLE_EQ(counterRate(std::uint64_t{50}, std::uint64_t{100}, 1.0), 0.0);
    EXPECT_DOUBLE_EQ(counterRate(std::uint64_t{150}, std::uint64_t{100}, 0.0), 0.0);
    EXPECT_DOUBLE_EQ(counterRate(std::uint64_t{150}, std::uint64_t{100}, -1.0), 0.0);
}

// ========== clampPercentToFloat Tests ==========

TEST(NumericTest, ClampPercentToFloatInRange)
{
    EXPECT_FLOAT_EQ(clampPercentToFloat(50.0), 50.0F);
    EXPECT_FLOAT_EQ(clampPercentToFloat(0.0), 0.0F);
    EXPECT_FLOAT_EQ(clampPercentToFloat(100.0), 100.0F);
}

TEST(NumericTest, ClampPercentToFloatAboveMax)
{
    EXPECT_FLOAT_EQ(clampPercentToFloat(150.0), 100.0F);
    EXPECT_FLOAT_EQ(clampPercentToFloat(1000.0), 100.0F);
}

TEST(NumericTest, ClampPercentToFloatBelowMin)
{
    EXPECT_FLOAT_EQ(clampPercentToFloat(-50.0), 0.0F);
    EXPECT_FLOAT_EQ(clampPercentToFloat(-1.0), 0.0F);
}

// ========== narrowOr Tests ==========

TEST(NumericTest, NarrowOrInRangeValue)
{
    // Value fits in target type - returns value
    EXPECT_EQ(narrowOr<int>(100, -1), 100);
    EXPECT_EQ(narrowOr<std::int32_t>(std::int64_t{1000}, -1), 1000);
    EXPECT_EQ(narrowOr<std::uint8_t>(200, std::uint8_t{0}), 200);
}

TEST(NumericTest, NarrowOrOverflowReturnsDefault)
{
    // Value too large for target type - returns fallback
    constexpr std::int64_t largeValue = static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max()) + 1;
    EXPECT_EQ(narrowOr<std::int32_t>(largeValue, -999), -999);

    // uint8_t max is 255, so 300 should overflow
    EXPECT_EQ(narrowOr<std::uint8_t>(300, std::uint8_t{42}), 42);
}

TEST(NumericTest, NarrowOrUnderflowReturnsDefault)
{
    // Negative value to unsigned - returns fallback
    EXPECT_EQ(narrowOr<std::uint32_t>(-1, 999U), 999U);
    EXPECT_EQ(narrowOr<std::uint8_t>(-100, std::uint8_t{0}), 0);

    // Value too small for signed target
    constexpr std::int64_t smallValue = static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::min()) - 1;
    EXPECT_EQ(narrowOr<std::int32_t>(smallValue, -1), -1);
}

TEST(NumericTest, NarrowOrNegativeToSigned)
{
    // Negative value to signed type that can hold it - returns value
    EXPECT_EQ(narrowOr<int>(-50, 0), -50);
    EXPECT_EQ(narrowOr<std::int16_t>(std::int32_t{-1000}, std::int16_t{0}), -1000);
}

TEST(NumericTest, NarrowOrZero)
{
    // Zero should always fit
    EXPECT_EQ(narrowOr<int>(0, -1), 0);
    EXPECT_EQ(narrowOr<std::uint8_t>(0, std::uint8_t{255}), 0);
    EXPECT_EQ(narrowOr<std::int8_t>(std::int64_t{0}, std::int8_t{-1}), 0);
}

TEST(NumericTest, NarrowOrBoundaryValues)
{
    // Test at exact boundaries
    constexpr auto int32Max = std::numeric_limits<std::int32_t>::max();
    constexpr auto int32Min = std::numeric_limits<std::int32_t>::min();

    // Exactly at int32 max - should fit
    EXPECT_EQ(narrowOr<std::int32_t>(static_cast<std::int64_t>(int32Max), -1), int32Max);

    // Exactly at int32 min - should fit
    EXPECT_EQ(narrowOr<std::int32_t>(static_cast<std::int64_t>(int32Min), 0), int32Min);

    // One past max - should use fallback
    EXPECT_EQ(narrowOr<std::int32_t>(static_cast<std::int64_t>(int32Max) + 1, -1), -1);

    // One past min - should use fallback
    EXPECT_EQ(narrowOr<std::int32_t>(static_cast<std::int64_t>(int32Min) - 1, 0), 0);
}

TEST(NumericTest, NarrowOrUint8Boundaries)
{
    // uint8_t range is 0-255
    EXPECT_EQ(narrowOr<std::uint8_t>(0, std::uint8_t{99}), 0);
    EXPECT_EQ(narrowOr<std::uint8_t>(255, std::uint8_t{99}), 255);
    EXPECT_EQ(narrowOr<std::uint8_t>(256, std::uint8_t{99}), 99); // overflow
    EXPECT_EQ(narrowOr<std::uint8_t>(-1, std::uint8_t{99}), 99);  // underflow
}

TEST(NumericTest, NarrowOrSameTypeSameValue)
{
    // Same type conversion should always succeed
    EXPECT_EQ(narrowOr<int>(42, -1), 42);
    EXPECT_EQ(narrowOr<std::uint64_t>(std::uint64_t{1000}, std::uint64_t{0}), 1000);
}

// ========== counterDelta at the top of the range (#1135) ==========

TEST(NumericTest, CounterDeltaAtTheTopOfTheRange)
{
    // A counter that reached its maximum and reset reads as no change rather than a huge delta.
    EXPECT_EQ(counterDelta(std::uint64_t{0}, std::numeric_limits<std::uint64_t>::max()), 0U);
    EXPECT_EQ(counterDelta(std::numeric_limits<std::uint64_t>::max(), std::uint64_t{0}), std::numeric_limits<std::uint64_t>::max());
}

// ========== wrappingCounterDelta/wrappingCounterRate Tests (#1184) ==========

TEST(NumericTest, WrappingCounterDeltaIsThePlainDeltaWithoutAWrap)
{
    EXPECT_EQ(wrappingCounterDelta(std::uint64_t{125}, std::uint64_t{100}, 32), 25U);
    EXPECT_EQ(wrappingCounterDelta(std::uint64_t{100}, std::uint64_t{100}, 32), 0U);
}

TEST(NumericTest, WrappingCounterDeltaCountsThroughA32BitWrap)
{
    // Windows' ULONG page-fault count: 10 below the top, then 5 past 0 -- 16 faults, not 0.
    constexpr std::uint64_t MAX32 = 0xFFFF'FFFFULL;
    EXPECT_EQ(wrappingCounterDelta(std::uint64_t{5}, MAX32 - 10, 32), 16U);
    EXPECT_EQ(wrappingCounterDelta(std::uint64_t{0}, MAX32, 32), 1U);
    EXPECT_DOUBLE_EQ(wrappingCounterRate(std::uint64_t{5}, MAX32 - 10, 2.0, 32), 8.0);
}

TEST(NumericTest, WrappingCounterDeltaWithoutAWidthIsCounterDelta)
{
    // 64 bits (Linux) or 0: a decrease is a reset, not a wrap.
    EXPECT_EQ(wrappingCounterDelta(std::uint64_t{5}, std::uint64_t{100}, 64), 0U);
    EXPECT_EQ(wrappingCounterDelta(std::uint64_t{5}, std::uint64_t{100}, 0), 0U);
}

TEST(NumericTest, WrappingCounterDeltaRejectsReadingsTooBigForTheWidth)
{
    // A previous reading above 2^32 - 1 cannot have come from a 32-bit counter.
    EXPECT_EQ(wrappingCounterDelta(std::uint64_t{5}, std::uint64_t{0x1'0000'0000ULL}, 32), 0U);
    EXPECT_DOUBLE_EQ(wrappingCounterRate(std::uint64_t{5}, std::uint64_t{0x1'0000'0000ULL}, 1.0, 32), 0.0);
    EXPECT_DOUBLE_EQ(wrappingCounterRate(std::uint64_t{5}, std::uint64_t{1}, 0.0, 32), 0.0);
}

} // namespace
} // namespace Domain::Numeric
// NOLINTEND(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
