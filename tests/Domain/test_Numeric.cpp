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

} // namespace
} // namespace Domain::Numeric
// NOLINTEND(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
