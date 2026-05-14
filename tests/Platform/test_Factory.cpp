/// @file test_Factory.cpp
/// @brief Tests that Platform::Factory functions return valid non-null objects.
///
/// Each make*() function is a one-liner factory. These tests ensure every factory
/// function is exercised so coverage accounts for all factory implementations.

#include "Platform/Factory.h"
#include "Platform/IDiskProbe.h"
#include "Platform/IGPUProbe.h"
#include "Platform/IPathProvider.h"
#include "Platform/IPowerProbe.h"
#include "Platform/IProcessActions.h"
#include "Platform/IProcessProbe.h"
#include "Platform/ISystemProbe.h"

#include <gtest/gtest.h>

#include <memory>

namespace Platform
{
namespace
{

TEST(FactoryTest, MakeProcessProbeReturnsNonNull)
{
    auto probe = makeProcessProbe();
    EXPECT_NE(probe, nullptr);
}

TEST(FactoryTest, MakeProcessActionsReturnsNonNull)
{
    auto actions = makeProcessActions();
    EXPECT_NE(actions, nullptr);
}

TEST(FactoryTest, MakeSystemProbeReturnsNonNull)
{
    auto probe = makeSystemProbe();
    EXPECT_NE(probe, nullptr);
}

TEST(FactoryTest, MakeDiskProbeReturnsNonNull)
{
    auto probe = makeDiskProbe();
    EXPECT_NE(probe, nullptr);
}

TEST(FactoryTest, MakePathProviderReturnsNonNull)
{
    auto provider = makePathProvider();
    EXPECT_NE(provider, nullptr);
}

TEST(FactoryTest, MakePowerProbeReturnsNonNull)
{
    auto probe = makePowerProbe();
    EXPECT_NE(probe, nullptr);
}

TEST(FactoryTest, MakeGPUProbeReturnsNonNull)
{
    auto probe = makeGPUProbe();
    EXPECT_NE(probe, nullptr);
}

} // namespace
} // namespace Platform
