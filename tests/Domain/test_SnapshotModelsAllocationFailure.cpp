// The snapshot models under allocation failure (#3342 step 4): ServiceModel, StartupModel and UsersModel build
// each sample's publication aside and commit it once at the end. A std::bad_alloc anywhere in a sample must
// leave the last whole publication in place, never a half-built one, and the next sample must publish the new
// read in full. These pin that before the v0.11.0 refactors (#3345) restructure how models publish.
//
// The failure is real, not simulated: AllocationFailureHook makes the Nth allocation the current thread makes
// from now throw std::bad_alloc. The models let it escape sample() (BackgroundSampler catches and logs it), so
// each sweep catches it and asks the hook whether it fired.

#include "AllocationFailureHook.h"
#include "Domain/ServiceModel.h"
#include "Domain/StartupModel.h"
#include "Domain/UsersModel.h"
#include "Mocks/MockServiceProbe.h"
#include "Mocks/MockStartupProbe.h"
#include "Platform/IUserSessions.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

using TestSupport::FailAllocationAfter;

// Outside the anonymous namespace: it's made with std::make_shared (.github/copilot-instructions.md, Testing).
namespace SnapshotModelsAllocationFailureFakes
{

/// logind's sessions, read from a list the test changes.
class ListedSessions final : public Platform::IUserSessions
{
  public:
    std::vector<Platform::UserSession> sessions;

    [[nodiscard]] bool hasSessions() const override
    {
        return true;
    }
    [[nodiscard]] Platform::UserSessionsRead read() override
    {
        return {.status = Platform::UserSessionsStatus::Ok, .sessions = sessions, .detail = {}};
    }
};

} // namespace SnapshotModelsAllocationFailureFakes

namespace
{

constexpr std::int64_t MAX_ALLOCATIONS = 100'000; // a bound on each sweep, far above one sample's

/// Samples the model once with the hook set to fail allocation @p failAt; true when that failure fired.
template<typename Model> bool sampleFailingAt(Model& model, std::int64_t failAt)
{
    const FailAllocationAfter guard(failAt);
    try
    {
        model.sample();
    }
    catch (const std::bad_alloc&)
    {
        // the failure the sweep injects; the hook says whether it fired
    }
    return !TestSupport::allocationFailurePending();
}

/// Fails each allocation of one sample in turn. @p make builds a model that has published its first read and
/// will read the second next; @p expectFirst / @p expectSecond check a publication holds that whole read.
/// After every failure the publication is the first read's (untouched) or the second's (whole), and the next
/// sample publishes the second read in full.
template<typename Make, typename ExpectFirst, typename ExpectSecond>
void sweep(const Make& make, const ExpectFirst& expectFirst, const ExpectSecond& expectSecond)
{
    std::int64_t failed = 0;
    for (std::int64_t failAt = 0; failAt < MAX_ALLOCATIONS; ++failAt)
    {
        SCOPED_TRACE("failing allocation " + std::to_string(failAt));
        auto model = make();
        const auto before = model->publication();
        if (!sampleFailingAt(*model, failAt))
        {
            break; // the sample made fewer than failAt + 1 allocations: every one has been failed
        }
        ++failed;

        const auto afterFailure = model->publication();
        if (afterFailure->version == before->version)
        {
            EXPECT_EQ(afterFailure, before); // nothing half built replaced it
            expectFirst(*afterFailure);
        }
        else
        {
            EXPECT_EQ(afterFailure->version, before->version + 1);
            expectSecond(*afterFailure);
        }

        model->sample();
        const auto next = model->publication();
        EXPECT_GT(next->version, before->version);
        expectSecond(*next);
        if (::testing::Test::HasFailure())
        {
            return; // one failing allocation point is enough to report
        }
    }
    EXPECT_GT(failed, 3); // the sample did allocate, and those allocations were exercised
}

Platform::ServiceInfo service(const char* name, std::uint32_t pid)
{
    Platform::ServiceInfo info;
    info.name = name;
    info.displayName = std::string(name) + " (a display name longer than small-string storage)";
    info.state = pid == 0 ? Platform::ServiceState::Stopped : Platform::ServiceState::Running;
    info.pid = pid;
    return info;
}

TEST(SnapshotModelsAllocationFailureTest, AFailedServiceSampleKeepsTheLastWholeListAndItsPidIndex)
{
#if defined(TASKSMACK_NO_ALLOCATOR_HOOK)
    GTEST_SKIP() << TASKSMACK_NO_ALLOCATOR_HOOK;
#endif
    sweep(
        []
        {
            auto probe = std::make_unique<Mocks::MockServiceProbe>();
            auto* raw = probe.get();
            raw->setServices({service("Winmgmt", 1234), service("bits", 0)});
            auto model = std::make_unique<Domain::ServiceModel>(std::move(probe));
            model->sample();
            raw->setServices({service("Winmgmt", 1234), service("bits", 0), service("EventLog", 42), service("Audiosrv", 1234)});
            return model;
        },
        [](const Domain::ServicePublication& publication)
        {
            ASSERT_EQ(publication.services.size(), 2U);
            EXPECT_EQ(publication.services[0].name, "bits");
            EXPECT_EQ(publication.servicesByPid.size(), 1U);
        },
        [](const Domain::ServicePublication& publication)
        {
            ASSERT_EQ(publication.services.size(), 4U);
            EXPECT_EQ(publication.services[0].name, "Audiosrv");
            EXPECT_EQ(publication.services[3].name, "Winmgmt");
            EXPECT_EQ(publication.servicesByPid.size(), 2U); // 1234 (two services) and 42
            EXPECT_FALSE(publication.stale);
        });
}

Platform::StartupEntry entry(const char* name)
{
    Platform::StartupEntry startup;
    startup.name = name;
    startup.command = std::string(R"("C:\Apps\)") + name + R"(.exe" --a-command-longer-than-small-string-storage)";
    startup.enabled = true;
    return startup;
}

TEST(SnapshotModelsAllocationFailureTest, AFailedStartupSampleKeepsTheLastWholeList)
{
#if defined(TASKSMACK_NO_ALLOCATOR_HOOK)
    GTEST_SKIP() << TASKSMACK_NO_ALLOCATOR_HOOK;
#endif
    sweep(
        []
        {
            auto probe = std::make_unique<Mocks::MockStartupProbe>();
            auto* raw = probe.get();
            raw->setEntries({entry("Zoom"), entry("Discord")});
            auto model = std::make_unique<Domain::StartupModel>(std::move(probe));
            model->sample();
            raw->setEntries({entry("Zoom"), entry("Discord"), entry("OneDrive"), entry("Teams")});
            return model;
        },
        [](const Domain::StartupPublication& publication)
        {
            ASSERT_EQ(publication.entries.size(), 2U);
            EXPECT_EQ(publication.entries[0].name, "Discord");
        },
        [](const Domain::StartupPublication& publication)
        {
            ASSERT_EQ(publication.entries.size(), 4U);
            EXPECT_EQ(publication.entries[0].name, "Discord");
            EXPECT_EQ(publication.entries[1].name, "OneDrive");
            EXPECT_EQ(publication.entries[3].name, "Zoom");
        });
}

Platform::UserSession session(const char* id, const char* user)
{
    Platform::UserSession result;
    result.id = id;
    result.user = user;
    result.seat = "seat0";
    result.type = "wayland";
    result.state = "active";
    result.service = "gdm-password-a-service-name-longer-than-small-string-storage";
    return result;
}

TEST(SnapshotModelsAllocationFailureTest, AFailedUsersSampleKeepsTheLastWholeList)
{
#if defined(TASKSMACK_NO_ALLOCATOR_HOOK)
    GTEST_SKIP() << TASKSMACK_NO_ALLOCATOR_HOOK;
#endif
    sweep(
        []
        {
            auto sessions = std::make_shared<SnapshotModelsAllocationFailureFakes::ListedSessions>();
            sessions->sessions = {session("3", "zoe"), session("2", "alex")};
            auto model = std::make_unique<Domain::UsersModel>(sessions);
            model->sample();
            sessions->sessions = {session("3", "zoe"), session("2", "alex"), session("c1", "Bea"), session("4", "alex")};
            return model;
        },
        [](const Domain::UsersPublication& publication)
        {
            ASSERT_EQ(publication.sessions.size(), 2U);
            EXPECT_EQ(publication.sessions[0].user, "alex");
        },
        [](const Domain::UsersPublication& publication)
        {
            EXPECT_EQ(publication.status, Platform::UserSessionsStatus::Ok);
            ASSERT_EQ(publication.sessions.size(), 4U);
            EXPECT_EQ(publication.sessions[0].id, "2");
            EXPECT_EQ(publication.sessions[1].id, "4");
            EXPECT_EQ(publication.sessions[2].user, "Bea");
            EXPECT_EQ(publication.sessions[3].user, "zoe");
        });
}

} // namespace
