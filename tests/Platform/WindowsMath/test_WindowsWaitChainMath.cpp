/// @file test_WindowsWaitChainMath.cpp
/// @brief Windows' "Analyze wait chain" (#3084), its pure parts: wct.h's object types and statuses as
/// Platform's, a node's thread or object fields, a chain decoded from GetThreadWaitChain's nodes (a cycle, a
/// chain cut at Windows' limit, a node never filled in, an unknown type), what a read reports when threads
/// were refused, a failed chain's wording, the read's budget and its flags. Pure, so built everywhere.

#include "Platform/WaitChain.h"
#include "Platform/Windows/WindowsWaitChainMath.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace Platform::WaitChainMath
{
namespace
{

[[nodiscard]] RawWaitNode threadNode(std::uint32_t pid, std::uint32_t tid, std::uint32_t status)
{
    return {.objectName = {},
            .objectType = WCT_THREAD_TYPE,
            .objectStatus = status,
            .processId = pid,
            .threadId = tid,
            .waitTime = 250,
            .contextSwitches = 17};
}

[[nodiscard]] RawWaitNode objectNode(std::uint32_t type, std::uint32_t status, std::string name)
{
    return {.objectName = std::move(name),
            .objectType = type,
            .objectStatus = status,
            .processId = 0,
            .threadId = 0,
            .waitTime = 0,
            .contextSwitches = 0};
}

TEST(WaitChainMathTest, EveryWctTypeHasItsPlatformTypeAndAnythingElseIsUnknown)
{
    EXPECT_EQ(objectType(WCT_CRITICAL_SECTION_TYPE), WaitObjectType::CriticalSection);
    EXPECT_EQ(objectType(WCT_SEND_MESSAGE_TYPE), WaitObjectType::SendMessageCall);
    EXPECT_EQ(objectType(WCT_MUTEX_TYPE), WaitObjectType::Mutex);
    EXPECT_EQ(objectType(WCT_ALPC_TYPE), WaitObjectType::Alpc);
    EXPECT_EQ(objectType(WCT_COM_TYPE), WaitObjectType::Com);
    EXPECT_EQ(objectType(WCT_THREAD_WAIT_TYPE), WaitObjectType::ThreadWait);
    EXPECT_EQ(objectType(WCT_PROCESS_WAIT_TYPE), WaitObjectType::ProcessWait);
    EXPECT_EQ(objectType(WCT_THREAD_TYPE), WaitObjectType::Thread);
    EXPECT_EQ(objectType(WCT_COM_ACTIVATION_TYPE), WaitObjectType::ComActivation);
    EXPECT_EQ(objectType(WCT_SOCKET_IO_TYPE), WaitObjectType::SocketIo);
    EXPECT_EQ(objectType(WCT_SMB_IO_TYPE), WaitObjectType::SmbIo);
    EXPECT_EQ(objectType(WCT_UNKNOWN_TYPE), WaitObjectType::Unknown);
    EXPECT_EQ(objectType(0), WaitObjectType::Unknown);
    EXPECT_EQ(objectType(13), WaitObjectType::Unknown) << "a type newer than this build";
}

TEST(WaitChainMathTest, EveryWctStatusHasItsPlatformStatusAndAnythingElseIsUnknown)
{
    EXPECT_EQ(nodeStatus(WCT_STATUS_NO_ACCESS), WaitNodeStatus::NoAccess);
    EXPECT_EQ(nodeStatus(WCT_STATUS_RUNNING), WaitNodeStatus::Running);
    EXPECT_EQ(nodeStatus(WCT_STATUS_BLOCKED), WaitNodeStatus::Blocked);
    EXPECT_EQ(nodeStatus(WCT_STATUS_PID_ONLY), WaitNodeStatus::PidOnly);
    EXPECT_EQ(nodeStatus(WCT_STATUS_PID_ONLY_RPCSS), WaitNodeStatus::PidOnlyRpcss);
    EXPECT_EQ(nodeStatus(WCT_STATUS_OWNED), WaitNodeStatus::Owned);
    EXPECT_EQ(nodeStatus(WCT_STATUS_NOT_OWNED), WaitNodeStatus::NotOwned);
    EXPECT_EQ(nodeStatus(WCT_STATUS_ABANDONED), WaitNodeStatus::Abandoned);
    EXPECT_EQ(nodeStatus(WCT_STATUS_ERROR), WaitNodeStatus::Error);
    EXPECT_EQ(nodeStatus(WCT_STATUS_UNKNOWN), WaitNodeStatus::Unknown);
    EXPECT_EQ(nodeStatus(0), WaitNodeStatus::Unknown);
    EXPECT_EQ(nodeStatus(42), WaitNodeStatus::Unknown);
}

TEST(WaitChainMathTest, AThreadNodeKeepsItsIdsAndAnObjectNodeItsName)
{
    const WaitChainNode thread = decodeNode(threadNode(4242, 77, WCT_STATUS_BLOCKED));
    EXPECT_EQ(thread.type, WaitObjectType::Thread);
    EXPECT_EQ(thread.status, WaitNodeStatus::Blocked);
    EXPECT_EQ(thread.pid, 4242);
    EXPECT_EQ(thread.tid, 77);
    EXPECT_EQ(thread.waitTime, 250U);
    EXPECT_EQ(thread.contextSwitches, 17U);
    EXPECT_TRUE(thread.objectName.empty());

    // An object's name; its (unused) ThreadObject fields are left alone, never read as IDs.
    RawWaitNode raw = objectNode(WCT_MUTEX_TYPE, WCT_STATUS_OWNED, R"(\Sessions\1\BaseNamedObjects\Lock)");
    raw.processId = 99;
    const WaitChainNode mutex = decodeNode(raw);
    EXPECT_EQ(mutex.type, WaitObjectType::Mutex);
    EXPECT_EQ(mutex.status, WaitNodeStatus::Owned);
    EXPECT_EQ(mutex.objectName, R"(\Sessions\1\BaseNamedObjects\Lock)");
    EXPECT_EQ(mutex.pid, 0);
    EXPECT_EQ(mutex.rawType, WCT_MUTEX_TYPE);

    // A thread only known by its process (an ALPC server's): PID, no TID.
    const WaitChainNode pidOnly = decodeNode(threadNode(500, 0, WCT_STATUS_PID_ONLY));
    EXPECT_EQ(pidOnly.status, WaitNodeStatus::PidOnly);
    EXPECT_EQ(pidOnly.pid, 500);
    EXPECT_EQ(pidOnly.tid, 0);
}

TEST(WaitChainMathTest, AnIdPastInt32IsUnknown)
{
    EXPECT_EQ(toId(0), 0);
    EXPECT_EQ(toId(4), 4);
    EXPECT_EQ(toId(0x7FFFFFFFU), 0x7FFFFFFF);
    EXPECT_EQ(toId(0x80000000U), 0);
}

// A deadlock: thread 10 waits on a critical section thread 20 owns, which waits on a mutex thread 10 owns.
TEST(WaitChainMathTest, ACycleIsADeadlockWithEveryNodeKept)
{
    const std::vector<RawWaitNode> nodes{
        threadNode(7, 10, WCT_STATUS_BLOCKED),
        objectNode(WCT_CRITICAL_SECTION_TYPE, WCT_STATUS_OWNED, ""),
        threadNode(7, 20, WCT_STATUS_BLOCKED),
        objectNode(WCT_MUTEX_TYPE, WCT_STATUS_OWNED, "Lock"),
        threadNode(7, 10, WCT_STATUS_BLOCKED),
    };
    const ThreadWaitChain chain = decodeChain(10, nodes, true, false);
    EXPECT_EQ(chain.tid, 10);
    EXPECT_TRUE(chain.isCycle);
    EXPECT_FALSE(chain.truncated);
    EXPECT_TRUE(chain.error.empty());
    ASSERT_EQ(chain.nodes.size(), 5U);
    EXPECT_EQ(chain.nodes[1].type, WaitObjectType::CriticalSection);
    EXPECT_EQ(chain.nodes[2].tid, 20);
    EXPECT_EQ(chain.nodes[3].objectName, "Lock");
    EXPECT_EQ(chain.nodes[4].tid, 10);
}

TEST(WaitChainMathTest, ARunningThreadIsAChainOfOne)
{
    const std::vector<RawWaitNode> nodes{threadNode(7, 10, WCT_STATUS_RUNNING)};
    const ThreadWaitChain chain = decodeChain(10, nodes, false, false);
    ASSERT_EQ(chain.nodes.size(), 1U);
    EXPECT_EQ(chain.nodes.front().status, WaitNodeStatus::Running);
    EXPECT_FALSE(chain.isCycle);
}

TEST(WaitChainMathTest, AChainLongerThanWindowsFollowsIsCutAndSaysSo)
{
    std::vector<RawWaitNode> nodes;
    for (std::uint32_t i = 0; i < MAX_NODE_COUNT + 3; ++i)
    {
        nodes.push_back((i % 2 == 0) ? threadNode(7, 100 + i, WCT_STATUS_BLOCKED) : objectNode(WCT_MUTEX_TYPE, WCT_STATUS_OWNED, "m"));
    }
    const ThreadWaitChain longer = decodeChain(100, nodes, false, false);
    EXPECT_TRUE(longer.truncated);
    EXPECT_EQ(longer.nodes.size(), MAX_NODE_COUNT);

    // Windows said there was more (ERROR_MORE_DATA), though its array was full.
    const std::vector<RawWaitNode> full(nodes.begin(), nodes.begin() + MAX_NODE_COUNT);
    const ThreadWaitChain told = decodeChain(100, full, false, true);
    EXPECT_TRUE(told.truncated);
    EXPECT_EQ(told.nodes.size(), MAX_NODE_COUNT);
}

TEST(WaitChainMathTest, ANodeNeverFilledInEndsTheChain)
{
    const std::vector<RawWaitNode> nodes{
        threadNode(7, 10, WCT_STATUS_BLOCKED),
        objectNode(WCT_SEND_MESSAGE_TYPE, WCT_STATUS_BLOCKED, ""),
        RawWaitNode{}, // zeroed: type 0
        threadNode(7, 30, WCT_STATUS_RUNNING),
    };
    const ThreadWaitChain chain = decodeChain(10, nodes, false, true);
    ASSERT_EQ(chain.nodes.size(), 2U);
    EXPECT_EQ(chain.nodes[1].type, WaitObjectType::SendMessageCall);
}

TEST(WaitChainMathTest, AnUnknownTypeIsKeptWithItsRawValue)
{
    const std::vector<RawWaitNode> nodes{threadNode(7, 10, WCT_STATUS_BLOCKED), objectNode(13, 0, "future")};
    const ThreadWaitChain chain = decodeChain(10, nodes, false, false);
    ASSERT_EQ(chain.nodes.size(), 2U);
    EXPECT_EQ(chain.nodes[1].type, WaitObjectType::Unknown);
    EXPECT_EQ(chain.nodes[1].status, WaitNodeStatus::Unknown);
    EXPECT_EQ(chain.nodes[1].rawType, 13U);
    EXPECT_EQ(chain.nodes[1].objectName, "future");
}

TEST(WaitChainMathTest, EveryThreadRefusedIsAccessDeniedAndAnythingLessIsOk)
{
    EXPECT_EQ(readStatus(4, 4), WaitChainReadStatus::PermissionDenied);
    EXPECT_EQ(readStatus(4, 3), WaitChainReadStatus::Ok);
    EXPECT_EQ(readStatus(4, 0), WaitChainReadStatus::Ok);
    EXPECT_EQ(readStatus(0, 0), WaitChainReadStatus::Ok) << "no thread listed: nothing to show, not a refusal";
}

TEST(WaitChainMathTest, AFailedChainSaysWhyInPlainWords)
{
    EXPECT_EQ(chainErrorText(5, "Access is denied.\r\n"), "Access denied");
    EXPECT_EQ(chainErrorText(87, "The parameter is incorrect.\r\n"), "The thread has exited");
    EXPECT_EQ(chainErrorText(4312, "The object identifier does not represent a valid object.\r\n"), "The thread has exited");
    EXPECT_EQ(chainErrorText(1460, "This operation returned because the timeout period expired.\r\n"),
              "This operation returned because the timeout period expired");
    EXPECT_EQ(chainErrorText(31, ""), "Error 31");
    EXPECT_EQ(chainErrorText(31, ".\r\n"), "Error 31");
}

TEST(WaitChainMathTest, AReadStopsAtItsThreadCountOrItsTimeBudget)
{
    EXPECT_FALSE(budgetSpent(0, 0));
    EXPECT_FALSE(budgetSpent(WAIT_CHAIN_MAX_THREADS - 1, WAIT_CHAIN_TIME_BUDGET_MS - 1));
    EXPECT_TRUE(budgetSpent(WAIT_CHAIN_MAX_THREADS, 0));
    EXPECT_TRUE(budgetSpent(0, WAIT_CHAIN_TIME_BUDGET_MS));
}

TEST(WaitChainMathTest, ComCallsAreFollowedOnlyWhenItsCallbacksAreRegistered)
{
    EXPECT_EQ(chainFlags(true), GETINFO_ALL_FLAGS | NETWORK_IO_FLAG) << "every flag, network I/O too";
    EXPECT_EQ(chainFlags(true), 0xFU);
    EXPECT_EQ(chainFlags(false) & OUT_OF_PROC_COM_FLAG, 0U);
    EXPECT_EQ(chainFlags(false) | OUT_OF_PROC_COM_FLAG, chainFlags(true));
    EXPECT_EQ(GETINFO_ALL_FLAGS, 0x7U);
}

} // namespace
} // namespace Platform::WaitChainMath
