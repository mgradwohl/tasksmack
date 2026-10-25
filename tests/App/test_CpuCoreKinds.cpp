/// @file test_CpuCoreKinds.cpp
/// @brief Tests for App::CpuCoresSection::classifyCoreKinds(): which CPU Cores charts carry a P-core,
///        E-core or low-power E-core marker on a hybrid CPU (#1536)

#include "App/Panels/CpuCoreKinds.h"
#include "Platform/CpuDetails.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace
{

using App::CpuCoresSection::classifyCoreKinds;
using App::CpuCoresSection::CoreKind;
using App::CpuCoresSection::coreKindDescription;
using App::CpuCoresSection::coreKindFor;
using Platform::UNKNOWN_EFFICIENCY_CLASS;

[[nodiscard]] std::vector<CoreKind> kindsOf(const std::vector<std::uint8_t>& classes)
{
    std::vector<CoreKind> kinds;
    classifyCoreKinds(classes, kinds);
    return kinds;
}

TEST(CpuCoreKindsTest, NoClassesMeansNoMarkers)
{
    EXPECT_TRUE(kindsOf({}).empty());
}

TEST(CpuCoreKindsTest, OneClassIsHomogeneousAndGetsNoMarkers)
{
    EXPECT_TRUE(kindsOf({0, 0, 0, 0}).empty());
    EXPECT_TRUE(kindsOf({3, 3}).empty());
}

TEST(CpuCoreKindsTest, OnlyUnknownClassesGetNoMarkers)
{
    EXPECT_TRUE(kindsOf({UNKNOWN_EFFICIENCY_CLASS, UNKNOWN_EFFICIENCY_CLASS}).empty());
    // One known class besides the unknowns is still not hybrid.
    EXPECT_TRUE(kindsOf({1, UNKNOWN_EFFICIENCY_CLASS, 1}).empty());
}

TEST(CpuCoreKindsTest, TwoClassesArePerformanceAndEfficiency)
{
    // Alder Lake style: class 1 P-cores, class 0 E-cores, not in id order.
    EXPECT_EQ(kindsOf({1, 1, 0, 0, 1}),
              (std::vector<CoreKind>{
                  CoreKind::Performance, CoreKind::Performance, CoreKind::Efficiency, CoreKind::Efficiency, CoreKind::Performance}));
}

TEST(CpuCoreKindsTest, ThreeClassesAddLowPowerForTheLowest)
{
    // Core Ultra 7 255H: EfficiencyClass 2/1/0 for P, E and LP E-cores.
    EXPECT_EQ(kindsOf({2, 2, 1, 1, 0, 0}),
              (std::vector<CoreKind>{CoreKind::Performance,
                                     CoreKind::Performance,
                                     CoreKind::Efficiency,
                                     CoreKind::Efficiency,
                                     CoreKind::LowPower,
                                     CoreKind::LowPower}));
}

TEST(CpuCoreKindsTest, FourClassesMarkTheMiddleOnesAsEfficiency)
{
    EXPECT_EQ(kindsOf({3, 2, 1, 0}),
              (std::vector<CoreKind>{CoreKind::Performance, CoreKind::Efficiency, CoreKind::Efficiency, CoreKind::LowPower}));
}

TEST(CpuCoreKindsTest, UnknownCoresOnAHybridCpuGetNoMarker)
{
    EXPECT_EQ(kindsOf({1, UNKNOWN_EFFICIENCY_CLASS, 0}),
              (std::vector<CoreKind>{CoreKind::Performance, CoreKind::None, CoreKind::Efficiency}));
}

TEST(CpuCoreKindsTest, OutputIsReplacedNotAppended)
{
    std::vector<CoreKind> kinds;
    classifyCoreKinds({1, 0}, kinds);
    classifyCoreKinds({0, 0}, kinds); // Now homogeneous
    EXPECT_TRUE(kinds.empty());
}

TEST(CpuCoreKindsTest, LookupPastTheEndIsNone)
{
    const std::vector<CoreKind> kinds{CoreKind::Performance};
    EXPECT_EQ(coreKindFor(kinds, 0), CoreKind::Performance);
    EXPECT_EQ(coreKindFor(kinds, 1), CoreKind::None);
    EXPECT_EQ(coreKindFor({}, 0), CoreKind::None);
}

TEST(CpuCoreKindsTest, DescriptionsNameEachKind)
{
    EXPECT_STREQ(coreKindDescription(CoreKind::Performance), "Performance core");
    EXPECT_STREQ(coreKindDescription(CoreKind::Efficiency), "Efficiency core");
    EXPECT_STREQ(coreKindDescription(CoreKind::LowPower), "Low-power efficiency core");
    EXPECT_STREQ(coreKindDescription(CoreKind::None), "");
}

} // namespace
