/// @file test_ProcessServices.cpp
/// @brief Which services each process hosts (#1880): the service list's PID index (several services in
/// one process, none in a stopped one), the names merged into each snapshot (Windows from the list,
/// Linux from the process's own systemd unit), the age past which a list is no longer current, and,
/// through Domain::ProcessModel, a PID reused by a later process -- a new start time -- not inheriting
/// the services of the process that had it when the list was read.

#include "Domain/ProcessModel.h"
#include "Domain/ProcessServices.h"
#include "Domain/ProcessSnapshot.h"
#include "Domain/SamplingConfig.h"
#include "Domain/ServiceModel.h"
#include "Mocks/MockProbes.h"
#include "Mocks/MockServiceProbe.h"
#include "Platform/IServiceProbe.h"
#include "Platform/ProcessTypes.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace Domain
{
namespace
{

using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

[[nodiscard]] Platform::ServiceInfo
service(const char* name, std::uint32_t pid, Platform::ServiceState state = Platform::ServiceState::Running)
{
    Platform::ServiceInfo info;
    info.name = name;
    info.displayName = std::string(name) + " display";
    info.state = state;
    info.pid = pid;
    return info;
}

/// A publication as ServiceModel::sample() builds one, read at @p captureTime.
[[nodiscard]] ServicePublication publicationOf(std::vector<Platform::ServiceInfo> services, Clock::time_point captureTime = {})
{
    ServicePublication publication;
    publication.version = 1;
    publication.services = std::move(services);
    publication.servicesByPid = indexServicesByPid(publication.services);
    publication.captureTime = captureTime;
    return publication;
}

/// An svchost.exe hosting three services, a process hosting one, and a stopped service.
[[nodiscard]] ServicePublication svchostList(Clock::time_point captureTime = {})
{
    return publicationOf({service("BFE", 900),
                          service("Dhcp", 1200),
                          service("EventLog", 1200),
                          service("Spooler", 0, Platform::ServiceState::Stopped),
                          service("nsi", 1200)},
                         captureTime);
}

TEST(ProcessServicesTest, TheIndexListsEveryServiceAPidHostsInListOrder)
{
    const ServicePublication publication = svchostList();
    ASSERT_EQ(publication.servicesByPid.size(), 2U); // the stopped service (PID 0) is in no entry
    const std::span<const std::size_t> svchost = servicesHostedBy(publication, 1200);
    ASSERT_EQ(svchost.size(), 3U);
    EXPECT_EQ(publication.services[svchost[0]].name, "Dhcp");
    EXPECT_EQ(publication.services[svchost[1]].name, "EventLog");
    EXPECT_EQ(publication.services[svchost[2]].name, "nsi");
    ASSERT_EQ(servicesHostedBy(publication, 900).size(), 1U);
    EXPECT_TRUE(servicesHostedBy(publication, 4242).empty()); // a process hosting none
    EXPECT_TRUE(servicesHostedBy(publication, 0).empty());
    EXPECT_TRUE(indexServicesByPid({}).empty());
}

TEST(ProcessServicesTest, FindServiceMatchesTheExactName)
{
    const ServicePublication publication = svchostList();
    const Platform::ServiceInfo* found = findService(publication, "EventLog");
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->pid, 1200U);
    EXPECT_EQ(findService(publication, "eventlog"), nullptr);
    EXPECT_EQ(findService(publication, "Nope"), nullptr);
}

TEST(ProcessServicesTest, HostedNamesComeFromTheListByPid)
{
    const ServicePublication publication = svchostList();
    using ProcessServices::hostedServiceNames;
    EXPECT_EQ(hostedServiceNames("", &publication, 1200, true), (std::vector<std::string>{"Dhcp", "EventLog", "nsi"}));
    EXPECT_EQ(hostedServiceNames("", &publication, 900, true), (std::vector<std::string>{"BFE"}));
    EXPECT_TRUE(hostedServiceNames("", &publication, 4242, true).empty());
    EXPECT_TRUE(hostedServiceNames("", &publication, 0, true).empty()); // the idle process: no PID-0 match
    EXPECT_TRUE(hostedServiceNames("", nullptr, 1200, true).empty());   // no current list
}

TEST(ProcessServicesTest, AProcessListedAfterTheReadGetsNoneOfItsPidsEntries)
{
    // The list saw PID 1200 as an svchost; a process first listed since may have that PID reused.
    const ServicePublication publication = svchostList();
    EXPECT_TRUE(ProcessServices::hostedServiceNames("", &publication, 1200, /*listedAtCapture=*/false).empty());
}

TEST(ProcessServicesTest, TheLinuxUnitIsAlwaysTheProcesssAndNotRepeated)
{
    using ProcessServices::hostedServiceNames;
    EXPECT_EQ(hostedServiceNames("ssh.service", nullptr, 812, false), (std::vector<std::string>{"ssh.service"}));
    // A systemd list naming the same unit by PID adds nothing twice.
    const ServicePublication publication = publicationOf({service("ssh.service", 812), service("ssh-agent.service", 812)});
    EXPECT_EQ(hostedServiceNames("ssh.service", &publication, 812, true), (std::vector<std::string>{"ssh.service", "ssh-agent.service"}));
}

TEST(ProcessServicesTest, TheColumnTextJoinsTheNames)
{
    using ProcessServices::joinNames;
    EXPECT_EQ(joinNames(std::vector<std::string>{}), "");
    EXPECT_EQ(joinNames(std::vector<std::string>{"BFE"}), "BFE");
    EXPECT_EQ(joinNames(std::vector<std::string>{"Dhcp", "EventLog", "nsi"}), "Dhcp, EventLog, nsi");
}

TEST(ProcessServicesTest, AListIsCurrentForThreeRefreshes)
{
    const Clock::time_point read = Clock::time_point{} + std::chrono::hours(1);
    const ServicePublication publication = svchostList(read);
    using ProcessServices::isCurrent;
    EXPECT_TRUE(isCurrent(publication, read));
    EXPECT_TRUE(isCurrent(publication, read + milliseconds(Sampling::SERVICES_DATA_MAX_AGE_MS)));
    EXPECT_FALSE(isCurrent(publication, read + milliseconds(Sampling::SERVICES_DATA_MAX_AGE_MS + 1)));
    EXPECT_TRUE(isCurrent(publication, read - milliseconds(1))); // a later read than "now": still current
    EXPECT_FALSE(isCurrent(ServicePublication{}, read));         // never read
}

TEST(ProcessServicesTest, MergeFillsEverySnapshotByFirstSeen)
{
    const Clock::time_point read = Clock::time_point{} + std::chrono::hours(1);
    const ServicePublication publication = svchostList(read);
    std::vector<ProcessSnapshot> snapshots(3);
    snapshots[0].pid = 1200;
    snapshots[1].pid = 900;
    snapshots[2].pid = 77;
    std::vector<Platform::ProcessCounters> counters(3);
    counters[2].services = "cron.service";
    // 900 was first listed after the read: its entry may be another process's.
    const std::vector<Clock::time_point> firstSeen{read - milliseconds(10), read + milliseconds(10), read};

    ProcessServices::mergeInto(snapshots, counters, firstSeen, &publication);
    EXPECT_EQ(snapshots[0].services, "Dhcp, EventLog, nsi");
    EXPECT_TRUE(snapshots[1].serviceNames.empty());
    EXPECT_EQ(snapshots[1].services, "");
    EXPECT_EQ(snapshots[2].services, "cron.service");

    // No current list: only the units are left.
    ProcessServices::mergeInto(snapshots, counters, firstSeen, nullptr);
    EXPECT_EQ(snapshots[0].services, "");
    EXPECT_EQ(snapshots[2].services, "cron.service");
}

// ---- Through ProcessModel, with ServiceModel's own publication ------------------------------------

/// One steady clock both models read, so a capture time and a first-seen time compare.
class SharedClock
{
  public:
    [[nodiscard]] std::function<Clock::time_point()> now()
    {
        return [this]
        {
            return m_Time;
        };
    }

    void advance(Clock::duration duration)
    {
        m_Time += duration;
    }

  private:
    Clock::time_point m_Time = Clock::time_point{} + std::chrono::hours(1);
};

/// The model's snapshot of @p pid, or an empty one (PID 0) when it lists none.
[[nodiscard]] ProcessSnapshot snapshotOf(const ProcessModel& model, std::int32_t pid)
{
    return model.findSnapshot(pid).value_or(ProcessSnapshot{});
}

TEST(ProcessServicesModelTest, AReusedPidDoesNotInheritTheServicesOfThePidsEarlierProcess)
{
    SharedClock clock;
    auto processProbe = std::make_unique<TestMocks::MockProcessProbe>();
    TestMocks::MockProcessProbe* processes = processProbe.get();
    processes->withProcess(TestMocks::makeProcessCounters(1200, "svchost.exe", 'R', 100, 50, /*startTimeTicks=*/1000));
    ProcessModel model(std::move(processProbe), clock.now());
    model.refresh(); // svchost first listed

    auto serviceProbe = std::make_unique<Mocks::MockServiceProbe>();
    Mocks::MockServiceProbe* serviceList = serviceProbe.get();
    serviceList->setServices({service("Dhcp", 1200), service("EventLog", 1200)});
    const auto services = std::make_shared<ServiceModel>(std::move(serviceProbe), clock.now());
    model.setServiceModel(services);

    clock.advance(milliseconds(500));
    services->sample();
    clock.advance(milliseconds(500));
    model.refresh();
    ASSERT_EQ(snapshotOf(model, 1200).pid, 1200);
    EXPECT_EQ(snapshotOf(model, 1200).services, "Dhcp, EventLog");
    EXPECT_EQ(snapshotOf(model, 1200).serviceNames.size(), 2U);

    // The svchost exits and another process starts with its PID -- a new start time -- before the list
    // is read again: the old list's entry for 1200 is not this process's.
    processes->withProcess(TestMocks::makeProcessCounters(1200, "notepad.exe", 'R', 1, 1, /*startTimeTicks=*/5000));
    clock.advance(milliseconds(500));
    model.refresh();
    ASSERT_EQ(snapshotOf(model, 1200).pid, 1200);
    EXPECT_EQ(snapshotOf(model, 1200).name, "notepad.exe");
    EXPECT_TRUE(snapshotOf(model, 1200).serviceNames.empty());

    // A list read after it started speaks for it: the services moved to a new svchost, notepad hosts
    // nothing.
    serviceList->setServices({service("Dhcp", 1300), service("EventLog", 1300)});
    clock.advance(milliseconds(500));
    services->sample();
    clock.advance(milliseconds(500));
    model.refresh();
    EXPECT_TRUE(snapshotOf(model, 1200).serviceNames.empty());
}

TEST(ProcessServicesModelTest, AListNoLongerRefreshedStopsCounting)
{
    SharedClock clock;
    auto processProbe = std::make_unique<TestMocks::MockProcessProbe>();
    processProbe->withProcess(TestMocks::makeProcessCounters(900, "svchost.exe"));
    ProcessModel model(std::move(processProbe), clock.now());
    model.refresh();

    auto serviceProbe = std::make_unique<Mocks::MockServiceProbe>();
    serviceProbe->setServices({service("BFE", 900)});
    const auto services = std::make_shared<ServiceModel>(std::move(serviceProbe), clock.now());
    model.setServiceModel(services);
    services->sample();
    model.refresh();
    EXPECT_EQ(snapshotOf(model, 900).services, "BFE");

    // Nothing needs the list any more, so it is not read again; once too old it is not shown as current.
    clock.advance(milliseconds(Sampling::SERVICES_DATA_MAX_AGE_MS + 1));
    model.refresh();
    EXPECT_EQ(snapshotOf(model, 900).services, "");
}

TEST(ProcessServicesModelTest, AListWithPidsMakesTheCapabilityTrue)
{
    auto processProbe = std::make_unique<TestMocks::MockProcessProbe>();
    processProbe->withProcess(1, "init");
    ProcessModel model(std::move(processProbe));
    model.refresh();
    EXPECT_FALSE(model.capabilities().hasServices);

    Platform::ServiceCapabilities withPids;
    withPids.canEnumerate = true;
    withPids.hasPid = true;
    model.setServiceModel(std::make_shared<ServiceModel>(std::make_unique<Mocks::MockServiceProbe>(withPids)));
    model.refresh();
    EXPECT_TRUE(model.capabilities().hasServices);

    // A platform with no service list (Linux until systemd's is read) adds nothing.
    model.setServiceModel(std::make_shared<ServiceModel>(std::make_unique<Platform::UnsupportedServiceProbe>()));
    model.refresh();
    EXPECT_FALSE(model.capabilities().hasServices);
}

TEST(ProcessServicesModelTest, TheLinuxUnitNeedsNoServiceList)
{
    Platform::ProcessCounters sshd = TestMocks::makeProcessCounters(812, "sshd");
    sshd.services = "ssh.service";
    auto processProbe = std::make_unique<TestMocks::MockProcessProbe>();
    processProbe->withProcess(sshd);
    ProcessModel model(std::move(processProbe));
    model.refresh();
    EXPECT_EQ(snapshotOf(model, 812).services, "ssh.service");
}

} // namespace
} // namespace Domain
