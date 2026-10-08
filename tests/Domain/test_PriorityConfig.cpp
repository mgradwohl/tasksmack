// NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
#include "Domain/PriorityConfig.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string_view>
#include <utility>

namespace Domain::Priority
{
namespace
{

// ========== clampNice Tests ==========

TEST(PriorityConfigTest, ClampNiceInRangeValues)
{
    // Values within the valid range should be unchanged
    EXPECT_EQ(clampNice(0), 0);
    EXPECT_EQ(clampNice(-10), -10);
    EXPECT_EQ(clampNice(10), 10);
    EXPECT_EQ(clampNice(-5), -5);
    EXPECT_EQ(clampNice(5), 5);
}

TEST(PriorityConfigTest, ClampNiceBoundaryValues)
{
    // Boundary values should remain unchanged
    EXPECT_EQ(clampNice(MIN_NICE), MIN_NICE); // -20
    EXPECT_EQ(clampNice(MAX_NICE), MAX_NICE); // 19
}

TEST(PriorityConfigTest, ClampNiceBelowMinimum)
{
    // Values below MIN_NICE should clamp to MIN_NICE
    EXPECT_EQ(clampNice(-21), MIN_NICE);
    EXPECT_EQ(clampNice(-100), MIN_NICE);
    EXPECT_EQ(clampNice(-1000), MIN_NICE);
}

TEST(PriorityConfigTest, ClampNiceAboveMaximum)
{
    // Values above MAX_NICE should clamp to MAX_NICE
    EXPECT_EQ(clampNice(20), MAX_NICE);
    EXPECT_EQ(clampNice(100), MAX_NICE);
    EXPECT_EQ(clampNice(1000), MAX_NICE);
}

TEST(PriorityConfigTest, ClampNiceWithInt32)
{
    // Test with explicit int32_t type
    EXPECT_EQ(clampNice(std::int32_t{0}), std::int32_t{0});
    EXPECT_EQ(clampNice(std::int32_t{-20}), std::int32_t{-20});
    EXPECT_EQ(clampNice(std::int32_t{19}), std::int32_t{19});
    EXPECT_EQ(clampNice(std::int32_t{-25}), std::int32_t{-20});
    EXPECT_EQ(clampNice(std::int32_t{25}), std::int32_t{19});
}

TEST(PriorityConfigTest, ClampNiceWithInt64)
{
    // Test with larger integer type
    EXPECT_EQ(clampNice(std::int64_t{0}), std::int64_t{0});
    EXPECT_EQ(clampNice(std::int64_t{-20}), std::int64_t{-20});
    EXPECT_EQ(clampNice(std::int64_t{19}), std::int64_t{19});
    EXPECT_EQ(clampNice(std::int64_t{-1000}), std::int64_t{-20});
    EXPECT_EQ(clampNice(std::int64_t{1000}), std::int64_t{19});
}

TEST(PriorityConfigTest, ClampNiceWithInt16)
{
    // Test with smaller integer type
    EXPECT_EQ(clampNice(std::int16_t{0}), std::int16_t{0});
    EXPECT_EQ(clampNice(std::int16_t{-20}), std::int16_t{-20});
    EXPECT_EQ(clampNice(std::int16_t{19}), std::int16_t{19});
    EXPECT_EQ(clampNice(std::int16_t{-30}), std::int16_t{-20});
    EXPECT_EQ(clampNice(std::int16_t{30}), std::int16_t{19});
}

// ========== getPriorityLabel Tests ==========

TEST(PriorityConfigTest, GetPriorityLabelHigh)
{
    // Values below HIGH_THRESHOLD (-10) should return "High"
    EXPECT_EQ(getPriorityLabel(-20), "High");
    EXPECT_EQ(getPriorityLabel(-15), "High");
    EXPECT_EQ(getPriorityLabel(-11), "High");
}

TEST(PriorityConfigTest, GetPriorityLabelAboveNormal)
{
    // Values from HIGH_THRESHOLD (-10) up to (but not including) ABOVE_NORMAL_THRESHOLD (-5) should return "Above Normal"
    EXPECT_EQ(getPriorityLabel(-10), "Above Normal");
    EXPECT_EQ(getPriorityLabel(-9), "Above Normal");
    EXPECT_EQ(getPriorityLabel(-6), "Above Normal");
}

TEST(PriorityConfigTest, GetPriorityLabelNormal)
{
    // Values from ABOVE_NORMAL_THRESHOLD (-5) up to (but not including) BELOW_NORMAL_THRESHOLD (5) should return "Normal"
    EXPECT_EQ(getPriorityLabel(-5), "Normal");
    EXPECT_EQ(getPriorityLabel(-1), "Normal");
    EXPECT_EQ(getPriorityLabel(0), "Normal");
    EXPECT_EQ(getPriorityLabel(1), "Normal");
    EXPECT_EQ(getPriorityLabel(4), "Normal");
}

TEST(PriorityConfigTest, GetPriorityLabelBelowNormal)
{
    // Values from BELOW_NORMAL_THRESHOLD (5) up to (but not including) IDLE_THRESHOLD (15) should return "Below Normal"
    EXPECT_EQ(getPriorityLabel(5), "Below Normal");
    EXPECT_EQ(getPriorityLabel(10), "Below Normal");
    EXPECT_EQ(getPriorityLabel(14), "Below Normal");
}

TEST(PriorityConfigTest, GetPriorityLabelIdle)
{
    // Values at or above IDLE_THRESHOLD (15) should return "Idle"
    EXPECT_EQ(getPriorityLabel(15), "Idle");
    EXPECT_EQ(getPriorityLabel(19), "Idle");
    EXPECT_EQ(getPriorityLabel(100), "Idle");
}

TEST(PriorityConfigTest, GetPriorityLabelBoundaryValues)
{
    // Test exact threshold values to ensure correct classification
    EXPECT_EQ(getPriorityLabel(HIGH_THRESHOLD), "Above Normal");         // -10
    EXPECT_EQ(getPriorityLabel(ABOVE_NORMAL_THRESHOLD), "Normal");       // -5
    EXPECT_EQ(getPriorityLabel(BELOW_NORMAL_THRESHOLD), "Below Normal"); // 5
    EXPECT_EQ(getPriorityLabel(IDLE_THRESHOLD), "Idle");                 // 15
}

TEST(PriorityConfigTest, GetPriorityLabelBoundaryMinusOne)
{
    // Test one less than each threshold
    EXPECT_EQ(getPriorityLabel(HIGH_THRESHOLD - 1), "High");                 // -11
    EXPECT_EQ(getPriorityLabel(ABOVE_NORMAL_THRESHOLD - 1), "Above Normal"); // -6
    EXPECT_EQ(getPriorityLabel(BELOW_NORMAL_THRESHOLD - 1), "Normal");       // 4
    EXPECT_EQ(getPriorityLabel(IDLE_THRESHOLD - 1), "Below Normal");         // 14
}

TEST(PriorityConfigTest, GetPriorityLabelExtremeValues)
{
    // Test extreme values outside normal range
    EXPECT_EQ(getPriorityLabel(-1000), "High");
    EXPECT_EQ(getPriorityLabel(1000), "Idle");
}

// ========== Combined Tests ==========

TEST(PriorityConfigTest, ClampAndLabelConsistency)
{
    // Verify that clamped values produce expected labels
    EXPECT_EQ(getPriorityLabel(clampNice(-100)), "High"); // Clamped to -20
    EXPECT_EQ(getPriorityLabel(clampNice(100)), "Idle");  // Clamped to 19
    EXPECT_EQ(getPriorityLabel(clampNice(0)), "Normal");  // Unchanged at 0
}

TEST(PriorityConfigTest, ConstantsRelationship)
{
    // Verify that constants are in the expected order
    EXPECT_LT(MIN_NICE, HIGH_THRESHOLD);
    EXPECT_LT(HIGH_THRESHOLD, ABOVE_NORMAL_THRESHOLD);
    EXPECT_LT(ABOVE_NORMAL_THRESHOLD, NORMAL_NICE);
    EXPECT_LT(NORMAL_NICE, BELOW_NORMAL_THRESHOLD);
    EXPECT_LT(BELOW_NORMAL_THRESHOLD, IDLE_THRESHOLD);
    EXPECT_LT(IDLE_THRESHOLD, MAX_NICE);
}

// ========== Priority classes (#1280) ==========

TEST(PriorityConfigTest, IoLevelsAreZeroToSeven)
{
    EXPECT_EQ(MIN_IO_LEVEL, 0);
    EXPECT_EQ(MAX_IO_LEVEL, 7);
    EXPECT_EQ(clampIoLevel(-1), 0);
    EXPECT_EQ(clampIoLevel(3), 3);
    EXPECT_EQ(clampIoLevel(8), 7);
}

TEST(PriorityConfigTest, IoLevelForNiceIsTheKernelDerivation)
{
    // (nice + 20) / 5: nice 0 is best-effort 4, the ends are 0 and 7, out-of-range nice is clamped first.
    struct Case
    {
        int32_t nice;
        int32_t level;
    };
    constexpr std::array<Case, 8> CASES{{
        {.nice = -20, .level = 0},
        {.nice = -16, .level = 0},
        {.nice = -15, .level = 1},
        {.nice = 0, .level = 4},
        {.nice = 5, .level = 5},
        {.nice = 19, .level = 7},
        {.nice = -100, .level = 0},
        {.nice = 100, .level = 7},
    }};
    for (const Case& testCase : CASES)
    {
        SCOPED_TRACE(testCase.nice);
        EXPECT_EQ(ioLevelForNice(testCase.nice), testCase.level);
    }
}

TEST(PriorityClassTest, ClassLabelsMatchTheNiceLabelsAndNameRealtime)
{
    EXPECT_EQ(getPriorityClassLabel(PriorityClass::Idle), getPriorityLabel(MAX_NICE));
    EXPECT_EQ(getPriorityClassLabel(PriorityClass::BelowNormal), getPriorityLabel(BELOW_NORMAL_THRESHOLD));
    EXPECT_EQ(getPriorityClassLabel(PriorityClass::Normal), getPriorityLabel(NORMAL_NICE));
    EXPECT_EQ(getPriorityClassLabel(PriorityClass::AboveNormal), getPriorityLabel(HIGH_THRESHOLD));
    EXPECT_EQ(getPriorityClassLabel(PriorityClass::High), getPriorityLabel(MIN_NICE));
    EXPECT_EQ(getPriorityClassLabel(PriorityClass::Realtime), "Realtime");
    EXPECT_TRUE(getPriorityClassLabel(PriorityClass::None).empty());
}

TEST(PriorityClassTest, ProcessLabelUsesTheClassWhereThereIsOne)
{
    // Windows reports Realtime at MIN_NICE, which the nice scale calls High.
    EXPECT_EQ(getProcessPriorityLabel(PriorityClass::Realtime, MIN_NICE), "Realtime");
    EXPECT_EQ(getProcessPriorityLabel(PriorityClass::High, -15), "High");
    EXPECT_EQ(getProcessPriorityLabel(PriorityClass::AboveNormal, -7), "Above Normal");
}

TEST(PriorityClassTest, ProcessLabelFallsBackToNiceWithoutAClass)
{
    for (int32_t nice = MIN_NICE; nice <= MAX_NICE; ++nice)
    {
        EXPECT_EQ(getProcessPriorityLabel(PriorityClass::None, nice), getPriorityLabel(nice)) << nice;
    }
}

TEST(PriorityClassTest, EveryProcessLabelIsListed)
{
    const auto listed = [](std::string_view label)
    {
        return std::ranges::find(PROCESS_PRIORITY_LABELS, label) != PROCESS_PRIORITY_LABELS.end();
    };
    for (int32_t nice = MIN_NICE; nice <= MAX_NICE; ++nice)
    {
        EXPECT_TRUE(listed(getProcessPriorityLabel(PriorityClass::None, nice))) << nice;
    }
    for (const auto priorityClass : {PriorityClass::Idle,
                                     PriorityClass::BelowNormal,
                                     PriorityClass::Normal,
                                     PriorityClass::AboveNormal,
                                     PriorityClass::High,
                                     PriorityClass::Realtime})
    {
        EXPECT_TRUE(listed(getProcessPriorityLabel(priorityClass, NORMAL_NICE))) << getPriorityClassLabel(priorityClass);
    }
}

TEST(PriorityClassTest, SortKeyOrdersHigherClassesFirst)
{
    // Smaller key = higher priority, as with nice. Realtime comes before High even at equal nice.
    EXPECT_LT(prioritySortKey(PriorityClass::Realtime, MIN_NICE), prioritySortKey(PriorityClass::High, -15));
    EXPECT_LT(prioritySortKey(PriorityClass::Realtime, -15), prioritySortKey(PriorityClass::High, -15));
    EXPECT_LT(prioritySortKey(PriorityClass::High, -15), prioritySortKey(PriorityClass::AboveNormal, -7));
    EXPECT_LT(prioritySortKey(PriorityClass::AboveNormal, -7), prioritySortKey(PriorityClass::Normal, 0));
    EXPECT_LT(prioritySortKey(PriorityClass::Normal, 0), prioritySortKey(PriorityClass::BelowNormal, 10));
    EXPECT_LT(prioritySortKey(PriorityClass::BelowNormal, 10), prioritySortKey(PriorityClass::Idle, MAX_NICE));
}

TEST(PriorityClassTest, SortKeyWithoutAClassIsNiceOrder)
{
    // Linux: every process has no class, so the key must order exactly as nice does.
    for (int32_t a = MIN_NICE; a <= MAX_NICE; ++a)
    {
        for (int32_t b = MIN_NICE; b <= MAX_NICE; ++b)
        {
            EXPECT_EQ(prioritySortKey(PriorityClass::None, a) < prioritySortKey(PriorityClass::None, b), a < b) << a << " vs " << b;
        }
    }
}

TEST(PriorityClassTest, ForNiceMatchesTheLabelBucket)
{
    for (int32_t nice = MIN_NICE; nice <= MAX_NICE; ++nice)
    {
        EXPECT_EQ(getPriorityClassLabel(priorityClassForNice(nice)), getPriorityLabel(nice)) << nice;
    }
}

TEST(PriorityClassTest, SortKeyPutsAnUnreadClassWithTheClassItsLabelNames)
{
    // The Windows probe's fallback for a class it could not read: no class, nice 0, shown "Normal".
    // It must sort with Normal -- above Below Normal and Idle, below Above Normal, High and Realtime --
    // not below Idle.
    const auto unread = prioritySortKey(PriorityClass::None, NORMAL_NICE);
    EXPECT_EQ(getProcessPriorityLabel(PriorityClass::None, NORMAL_NICE), "Normal");
    EXPECT_EQ(unread, prioritySortKey(PriorityClass::Normal, NORMAL_NICE));

    // Each class at the nice value the Windows probe reports for it.
    const std::array<std::pair<PriorityClass, int32_t>, 6> classes{{
        {PriorityClass::Realtime, MIN_NICE},
        {PriorityClass::High, -15},
        {PriorityClass::AboveNormal, -7},
        {PriorityClass::Normal, NORMAL_NICE},
        {PriorityClass::BelowNormal, 10},
        {PriorityClass::Idle, MAX_NICE},
    }};
    for (const auto& [priorityClass, nice] : classes)
    {
        const auto key = prioritySortKey(priorityClass, nice);
        const std::string_view name = getPriorityClassLabel(priorityClass);
        if (priorityClass > PriorityClass::Normal)
        {
            EXPECT_LT(key, unread) << name; // Higher priority sorts first ascending...
            EXPECT_GT(unread, key) << name; // ...and last descending.
        }
        else if (priorityClass < PriorityClass::Normal)
        {
            EXPECT_GT(key, unread) << name;
            EXPECT_LT(unread, key) << name;
        }
        else
        {
            EXPECT_EQ(key, unread) << name;
        }
    }
}

TEST(PriorityClassTest, SortKeyPutsAnyClasslessNiceInItsLabelsClass)
{
    // A classless row sorts among the class whose label it shows, against every class.
    const std::array<std::pair<PriorityClass, int32_t>, 6> classes{{
        {PriorityClass::Realtime, MIN_NICE},
        {PriorityClass::High, -15},
        {PriorityClass::AboveNormal, -7},
        {PriorityClass::Normal, NORMAL_NICE},
        {PriorityClass::BelowNormal, 10},
        {PriorityClass::Idle, MAX_NICE},
    }};
    for (int32_t nice = MIN_NICE; nice <= MAX_NICE; ++nice)
    {
        const PriorityClass bucket = priorityClassForNice(nice);
        const auto classless = prioritySortKey(PriorityClass::None, nice);
        for (const auto& [priorityClass, classNice] : classes)
        {
            const auto key = prioritySortKey(priorityClass, classNice);
            if (priorityClass > bucket)
            {
                EXPECT_LT(key, classless) << nice << " vs " << getPriorityClassLabel(priorityClass);
            }
            else if (priorityClass < bucket)
            {
                EXPECT_GT(key, classless) << nice << " vs " << getPriorityClassLabel(priorityClass);
            }
        }
    }
}

} // namespace
} // namespace Domain::Priority
// NOLINTEND(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
