/// @file test_Factory.cpp
/// @brief Tests that Platform::Factory functions return valid non-null objects.
///
/// Each make*() function is a one-liner factory. These tests ensure every factory
/// function is exercised so coverage accounts for all factory implementations, on Linux and
/// Windows alike (#1566).

#include "Platform/Factory.h"
// NOLINTBEGIN(misc-include-cleaner) - complete types for the returned unique_ptrs to destroy
#include "Platform/IDiskProbe.h"
#include "Platform/IGPUProbe.h"
#include "Platform/IPathProvider.h"
#include "Platform/IPowerProbe.h"
#include "Platform/IProcessActions.h"
#include "Platform/IProcessConnections.h"
#include "Platform/IProcessEnvironment.h"
#include "Platform/IProcessModules.h"
#include "Platform/IProcessProbe.h"
#include "Platform/IProcessSecurity.h"
#include "Platform/IServiceProbe.h"
#include "Platform/IStartupProbe.h"
#include "Platform/ISystemProbe.h"
// NOLINTEND(misc-include-cleaner)

#include <gtest/gtest.h>

#include <memory>

namespace Platform
{
namespace
{

TEST(FactoryTest, MakeProcessProbeReturnsNonNull)
{
    const auto probe = makeProcessProbe();
    EXPECT_NE(probe, nullptr);
}

TEST(FactoryTest, MakeProcessActionsReturnsNonNull)
{
    const auto actions = makeProcessActions();
    EXPECT_NE(actions, nullptr);
}

TEST(FactoryTest, MakeSystemProbeReturnsNonNull)
{
    const auto probe = makeSystemProbe();
    EXPECT_NE(probe, nullptr);
}

TEST(FactoryTest, MakeDiskProbeReturnsNonNull)
{
    const auto probe = makeDiskProbe();
    EXPECT_NE(probe, nullptr);
}

TEST(FactoryTest, MakePathProviderReturnsNonNull)
{
    const auto provider = makePathProvider();
    EXPECT_NE(provider, nullptr);
}

TEST(FactoryTest, MakePowerProbeReturnsNonNull)
{
    const auto probe = makePowerProbe();
    EXPECT_NE(probe, nullptr);
}

TEST(FactoryTest, MakeGPUProbeReturnsNonNull)
{
    const auto probe = makeGPUProbe();
    EXPECT_NE(probe, nullptr);
}

TEST(FactoryTest, MakeProcessEnvironmentReaderReturnsNonNull)
{
    const auto reader = makeProcessEnvironmentReader();
    ASSERT_NE(reader, nullptr);
#ifdef _WIN32
    // Not implemented on Windows yet (#179): the Environment section is hidden.
    EXPECT_NE(dynamic_cast<UnsupportedProcessEnvironmentReader*>(reader.get()), nullptr);
#endif
}

TEST(FactoryTest, MakeProcessConnectionsReaderReturnsNonNull)
{
    const auto reader = makeProcessConnectionsReader();
    ASSERT_NE(reader, nullptr);
#ifdef _WIN32
    // Not wired up on Windows yet (#1489): the Connections section is hidden.
    EXPECT_NE(dynamic_cast<UnsupportedProcessConnectionsReader*>(reader.get()), nullptr);
#endif
}

TEST(FactoryTest, MakeProcessModulesReaderListsModules)
{
    const auto reader = makeProcessModulesReader();
    ASSERT_NE(reader, nullptr);
    EXPECT_TRUE(reader->hasModules()); // both platforms (#802)
}

TEST(FactoryTest, MakeProcessSecurityReaderMatchesThePlatform)
{
    const auto reader = makeProcessSecurityReader();
    ASSERT_NE(reader, nullptr);
#ifdef _WIN32
    EXPECT_FALSE(reader->hasSecurity()); // the token reader is the Windows lane's follow-up (#1526)
#else
    EXPECT_TRUE(reader->hasSecurity());
#endif
}

TEST(FactoryTest, MakeStartupProbeReturnsNonNull)
{
    const auto probe = makeStartupProbe();
    EXPECT_NE(probe, nullptr);
}

TEST(FactoryTest, MakeServiceProbeReturnsNonNull)
{
    const auto probe = makeServiceProbe();
    EXPECT_NE(probe, nullptr);
}

} // namespace
} // namespace Platform
