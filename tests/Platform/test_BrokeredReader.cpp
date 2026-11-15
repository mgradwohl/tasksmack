// Tests for BrokeredReader's traits (#3782): what each read kind's traits struct says a refusal and a failed read
// through the helper are, and which request and reply it pairs. The decorators themselves are tested through the
// BrokeredProcessActions aliases in test_BrokeredProcessActions.cpp.

#include "Platform/BrokerProtocol.h"
#include "Platform/BrokeredReader.h"
#include "Platform/IProcessConnections.h"
#include "Platform/IProcessEnvironment.h"
#include "Platform/IProcessMemoryMap.h"
#include "Platform/IProcessModules.h"
#include "Platform/IProcessOpenFiles.h"
#include "Platform/IProcessThreads.h"

#include <gtest/gtest.h>

#include <string>
#include <type_traits>

namespace Platform
{
namespace
{

static_assert(BrokeredReadTraits<EnvironmentReadTraits>);
static_assert(BrokeredReadTraits<ModulesReadTraits>);
static_assert(BrokeredReadTraits<OpenFilesReadTraits>);
static_assert(BrokeredReadTraits<ConnectionsReadTraits>);
static_assert(BrokeredReadTraits<ThreadsReadTraits>);
static_assert(BrokeredReadTraits<MemoryMapReadTraits>);

// Each kind asks the helper with its own request and takes only its own reply.
static_assert(std::is_same_v<EnvironmentReadTraits::Request, Broker::EnvironmentRequest>);
static_assert(std::is_same_v<EnvironmentReadTraits::Reply, Broker::EnvironmentReply>);
static_assert(std::is_same_v<ModulesReadTraits::Request, Broker::ModulesRequest>);
static_assert(std::is_same_v<ModulesReadTraits::Reply, Broker::ModulesReply>);
static_assert(std::is_same_v<OpenFilesReadTraits::Request, Broker::OpenFilesRequest>);
static_assert(std::is_same_v<OpenFilesReadTraits::Reply, Broker::OpenFilesReply>);
static_assert(std::is_same_v<ConnectionsReadTraits::Request, Broker::ConnectionsRequest>);
static_assert(std::is_same_v<ConnectionsReadTraits::Reply, Broker::ConnectionsReply>);
static_assert(std::is_same_v<ThreadsReadTraits::Request, Broker::ThreadsRequest>);
static_assert(std::is_same_v<ThreadsReadTraits::Reply, Broker::ThreadsReply>);
static_assert(std::is_same_v<MemoryMapReadTraits::Request, Broker::MemoryMapRequest>);
static_assert(std::is_same_v<MemoryMapReadTraits::Reply, Broker::MemoryMapReply>);

/// A kind's refusal is its PermissionDenied with nothing read, and its failure is Failed with @p detail.
template<typename Traits> void expectRefusedAndFailed()
{
    const typename Traits::Result refused = Traits::refused();
    EXPECT_EQ(refused.status, Traits::PERMISSION_DENIED);
    const typename Traits::Result failed = Traits::failed("why");
    EXPECT_NE(failed.status, Traits::PERMISSION_DENIED) << "a failed read through the helper is never retried";
}

TEST(BrokeredReaderTraitsTest, EachKindsRefusalIsItsPermissionDenied)
{
    expectRefusedAndFailed<EnvironmentReadTraits>();
    expectRefusedAndFailed<ModulesReadTraits>();
    expectRefusedAndFailed<OpenFilesReadTraits>();
    expectRefusedAndFailed<ConnectionsReadTraits>();
    expectRefusedAndFailed<ThreadsReadTraits>();
    expectRefusedAndFailed<MemoryMapReadTraits>();
}

TEST(BrokeredReaderTraitsTest, TheRefusalCarriesNothingRead)
{
    EXPECT_TRUE(EnvironmentReadTraits::refused().variables.empty());
    EXPECT_TRUE(ModulesReadTraits::refused().modules.empty());
    EXPECT_TRUE(ModulesReadTraits::refused().detail.empty());
    EXPECT_TRUE(OpenFilesReadTraits::refused().files.empty());
    EXPECT_TRUE(OpenFilesReadTraits::refused().detail.empty());
    EXPECT_TRUE(ConnectionsReadTraits::refused().connections.empty());
    EXPECT_TRUE(ConnectionsReadTraits::refused().detail.empty());
    EXPECT_TRUE(ThreadsReadTraits::refused().threads.empty());
    EXPECT_TRUE(ThreadsReadTraits::refused().detail.empty());
    EXPECT_TRUE(MemoryMapReadTraits::refused().mappings.empty());
    EXPECT_TRUE(MemoryMapReadTraits::refused().detail.empty());
    EXPECT_FALSE(MemoryMapReadTraits::refused().truncated);
}

TEST(BrokeredReaderTraitsTest, AFailedReadSaysWhyWhereItsResultCan)
{
    EXPECT_EQ(EnvironmentReadTraits::failed("why").status, EnvironmentReadStatus::Failed);
    EXPECT_TRUE(EnvironmentReadTraits::failed("why").variables.empty()) << "the environment's result has no detail";
    EXPECT_EQ(ModulesReadTraits::failed("why").status, ModulesReadStatus::Failed);
    EXPECT_EQ(ModulesReadTraits::failed("why").detail, "why");
    EXPECT_EQ(OpenFilesReadTraits::failed("why").status, OpenFilesReadStatus::Failed);
    EXPECT_EQ(OpenFilesReadTraits::failed("why").detail, "why");
    EXPECT_EQ(ConnectionsReadTraits::failed("why").status, ConnectionsReadStatus::Failed);
    EXPECT_EQ(ConnectionsReadTraits::failed("why").detail, "why");
    EXPECT_EQ(ThreadsReadTraits::failed("why").status, ThreadsReadStatus::Failed);
    EXPECT_EQ(ThreadsReadTraits::failed("why").detail, "why");
    EXPECT_EQ(MemoryMapReadTraits::failed("why").status, MemoryMapReadStatus::Failed);
    EXPECT_EQ(MemoryMapReadTraits::failed("why").detail, "why");
    EXPECT_FALSE(MemoryMapReadTraits::failed("why").truncated);
}

TEST(BrokeredReaderTraitsTest, TheHelpersFailureWordingIsUnchanged)
{
    EXPECT_EQ(std::string(HELPER_COULDNT_BE_ASKED), "The administrator helper couldn't be asked: ");
    EXPECT_EQ(std::string(HELPER_UNEXPECTED_ANSWER), "The administrator helper gave an unexpected answer");
}

} // namespace
} // namespace Platform
