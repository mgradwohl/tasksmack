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
#include "Platform/IProcessOpenFiles.h"
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
    // Both platforms read environments (Linux: /proc/[pid]/environ, #179; Windows: the PEB, #2569), so
    // the Environment section shows on both.
    EXPECT_TRUE(reader->hasEnvironment());
    EXPECT_EQ(dynamic_cast<UnsupportedProcessEnvironmentReader*>(reader.get()), nullptr);
}

TEST(FactoryTest, MakeProcessConnectionsReaderReturnsNonNull)
{
    const auto reader = makeProcessConnectionsReader();
    ASSERT_NE(reader, nullptr);
    // Linux (#799) and Windows (#1489) both list a process's sockets.
    EXPECT_TRUE(reader->hasConnections());
}

TEST(FactoryTest, MakeProcessModulesReaderListsModules)
{
    const auto reader = makeProcessModulesReader();
    ASSERT_NE(reader, nullptr);
    EXPECT_TRUE(reader->hasModules()); // both platforms (#802)
}

TEST(FactoryTest, MakeProcessOpenFilesReaderListsOpenFiles)
{
    const auto reader = makeProcessOpenFilesReader();
    ASSERT_NE(reader, nullptr);
    EXPECT_TRUE(reader->hasOpenFiles()); // both platforms (#183)
}

TEST(FactoryTest, MakeProcessSecurityReaderReadsSecurity)
{
    const auto reader = makeProcessSecurityReader();
    ASSERT_NE(reader, nullptr);
    EXPECT_TRUE(reader->hasSecurity()); // both platforms: /proc on Linux, the process token on Windows (#1526)
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

// #3093: the update check's HTTPS client is WinHTTP on Windows; Linux has none yet, so the check isn't
// offered there and nothing is sent.
TEST(FactoryTest, MakeHttpsClientIsWinHttpOnWindowsAndNoneOnLinux)
{
    const auto client = makeHttpsClient();
#ifdef _WIN32
    EXPECT_NE(client, nullptr);
#else
    EXPECT_EQ(client, nullptr);
#endif
}

} // namespace
} // namespace Platform
