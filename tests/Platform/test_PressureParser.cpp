// Linux pressure stall information parsing (#3062).

#include "Platform/Linux/PressureParser.h"
#include "Platform/SystemTypes.h"

#include <gtest/gtest.h>

namespace Platform
{
namespace
{

TEST(PressureParserTest, ReadsSomeAndFullAverages)
{
    const ResourcePressure pressure = PressureParser::parse("some avg10=1.25 avg60=0.50 avg300=0.05 total=123456\n"
                                                            "full avg10=0.30 avg60=0.10 avg300=0.00 total=789\n");
    ASSERT_TRUE(pressure.some.has_value());
    ASSERT_TRUE(pressure.full.has_value());
    EXPECT_DOUBLE_EQ(pressure.some->avg10, 1.25);
    EXPECT_DOUBLE_EQ(pressure.some->avg60, 0.50);
    EXPECT_DOUBLE_EQ(pressure.some->avg300, 0.05);
    EXPECT_DOUBLE_EQ(pressure.full->avg10, 0.30);
    EXPECT_TRUE(pressure.read());
}

TEST(PressureParserTest, CpuBeforeLinux513HasOnlySome)
{
    const ResourcePressure pressure = PressureParser::parse("some avg10=2.00 avg60=1.00 avg300=0.50 total=1\n");
    ASSERT_TRUE(pressure.some.has_value());
    EXPECT_FALSE(pressure.full.has_value());
    EXPECT_DOUBLE_EQ(pressure.some->avg10, 2.00);
}

TEST(PressureParserTest, MalformedOrEmptyTextReadsAsNothing)
{
    EXPECT_FALSE(PressureParser::parse("").read());
    EXPECT_FALSE(PressureParser::parse("garbage\n").read());
    EXPECT_FALSE(PressureParser::parse("some avg10=x avg60=1 avg300=1 total=0\n").some.has_value()) << "a value that isn't a number";
    EXPECT_FALSE(PressureParser::parse("some avg60=1 avg300=1 total=0\n").some.has_value()) << "a missing average";
    // No trailing newline, and an unknown line, are fine.
    const ResourcePressure pressure = PressureParser::parse("other avg10=9\nsome avg10=0.10 avg60=0.20 avg300=0.30 total=5");
    ASSERT_TRUE(pressure.some.has_value());
    EXPECT_DOUBLE_EQ(pressure.some->avg300, 0.30);
}

TEST(PressureParserTest, AKeyIsMatchedOnlyAsAWholeField)
{
    EXPECT_FALSE(PressureParser::field("xavg10=5", "avg10").has_value());
    EXPECT_DOUBLE_EQ(PressureParser::field("xavg10=5 avg10=7", "avg10").value_or(-1.0), 7.0);
}

} // namespace
} // namespace Platform
