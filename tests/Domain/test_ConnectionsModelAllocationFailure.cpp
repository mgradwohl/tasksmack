// ConnectionsModel under allocation failure (#3342 step 4, #3400): a std::bad_alloc anywhere in a sample must
// leave the published list whole and the rate state (the last reading's byte counters and its time) as it was
// or fully moved on, never part of one beside the other's time.
//
// The failure is real, not simulated: AllocationFailureHook makes the Nth allocation the current thread makes
// from now throw std::bad_alloc. ConnectionsModel::sample() lets it escape (BackgroundSampler catches and logs
// it), so the test catches it and asks the hook whether it fired.

#include "AllocationFailureHook.h"
#include "Domain/ConnectionsModel.h"
#include "Platform/IProcessConnections.h"
#include "Platform/ISystemConnections.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

using TestSupport::FailAllocationAfter;

// Outside the anonymous namespace: it's made with std::make_unique (.github/copilot-instructions.md, Testing).
namespace ConnectionsModelAllocationFailureFakes
{

/// The sockets the test sets, read as they are.
class ListedSockets final : public Platform::ISystemConnectionsReader
{
  public:
    explicit ListedSockets(std::shared_ptr<std::vector<Platform::SystemConnection>> sockets) : m_Sockets(std::move(sockets))
    {}
    [[nodiscard]] bool hasConnections() const override
    {
        return true;
    }
    [[nodiscard]] Platform::SystemConnectionsRead read() override
    {
        return {.status = Platform::SystemConnectionsStatus::Ok, .connections = *m_Sockets, .detail = {}};
    }

  private:
    std::shared_ptr<std::vector<Platform::SystemConnection>> m_Sockets;
};

} // namespace ConnectionsModelAllocationFailureFakes

namespace
{

using namespace std::chrono_literals;

/// Four TCP sockets held by "a".."d" (the table's order), each having sent and received @p bytes.
std::vector<Platform::SystemConnection> sockets(std::uint64_t bytes)
{
    std::vector<Platform::SystemConnection> result;
    for (std::int32_t i = 0; i < 4; ++i)
    {
        Platform::SystemConnection socket;
        socket.connection.protocol = Platform::ConnectionProtocol::Tcp;
        socket.connection.family = Platform::ConnectionFamily::IPv4;
        socket.connection.local.port = static_cast<std::uint16_t>(40000 + i);
        socket.connection.remote.port = 443;
        socket.pid = 100 + i;
        socket.processName = std::string(1, static_cast<char>('a' + i)) + "-a-process-name-longer-than-small-string-storage";
        socket.bytesSent = bytes;
        socket.bytesReceived = bytes * 2;
        socket.hasByteCounters = true;
        result.push_back(std::move(socket));
    }
    return result;
}

// Readings one second apart: 0 bytes, then 1000 (the sample whose allocations fail in turn), then 2000. Whether
// the failed sample's counters were kept (2000 - 1000 over 1 s) or not (2000 - 0 over 2 s), every socket's next
// rate is 1000 bytes/s sent and 2000 received. Before #3400 the failure could keep some of its counters beside
// the first reading's time: 1000 bytes over 2 s for those sockets, and no rate at all for the rest.
TEST(ConnectionsModelAllocationFailureTest, AFailedSampleLeavesTheNextRatesRightForEverySocket)
{
#if defined(TASKSMACK_NO_ALLOCATOR_HOOK)
    GTEST_SKIP() << TASKSMACK_NO_ALLOCATOR_HOOK;
#endif
    constexpr std::int64_t MAX_ALLOCATIONS = 100'000; // a bound on the loop, far above one sample's

    std::int64_t failed = 0;
    for (std::int64_t failAt = 0; failAt < MAX_ALLOCATIONS; ++failAt)
    {
        SCOPED_TRACE("failing allocation " + std::to_string(failAt));
        auto now = std::make_shared<Domain::ConnectionsModel::Clock::time_point>(std::chrono::hours(1));
        auto listed = std::make_shared<std::vector<Platform::SystemConnection>>(sockets(0));
        Domain::ConnectionsModel model(std::make_unique<ConnectionsModelAllocationFailureFakes::ListedSockets>(listed),
                                       [now] { return *now; });
        model.sample();
        const auto before = model.publication();

        *now += 1s;
        *listed = sockets(1000);
        bool fired = false;
        {
            const FailAllocationAfter guard(failAt);
            try
            {
                model.sample();
            }
            catch (const std::bad_alloc&)
            {
                // the failure this test injects; the hook says whether it fired
            }
            fired = !TestSupport::allocationFailurePending();
        }
        if (!fired)
        {
            break; // the sample made fewer than failAt + 1 allocations: every one has been failed
        }
        ++failed;

        // The last whole list, or the whole new one with a rate for every socket.
        const auto afterFailure = model.publication();
        if (afterFailure->version == before->version)
        {
            EXPECT_EQ(afterFailure, before);
        }
        else
        {
            EXPECT_EQ(afterFailure->version, before->version + 1);
            EXPECT_EQ(afterFailure->rates.size(), afterFailure->connections.size());
        }

        *now += 1s;
        *listed = sockets(2000);
        model.sample();
        const auto next = model.publication();
        ASSERT_EQ(next->connections.size(), 4U);
        ASSERT_EQ(next->rates.size(), 4U);
        for (std::size_t i = 0; i < next->rates.size(); ++i)
        {
            SCOPED_TRACE(next->connections[i].processName);
            EXPECT_TRUE(next->rates[i].known);
            EXPECT_DOUBLE_EQ(next->rates[i].sentPerSec, 1000.0);
            EXPECT_DOUBLE_EQ(next->rates[i].receivedPerSec, 2000.0);
        }
        if (HasFailure())
        {
            return; // one failing allocation point is enough to report
        }
    }
    EXPECT_GT(failed, 5); // the sample did allocate, and those allocations were exercised
}

} // namespace
