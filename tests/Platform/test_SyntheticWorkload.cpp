/// @file test_SyntheticWorkload.cpp
/// @brief Platform::Synthetic (#1413): the synthetic machine is deterministic for a seed, has the
/// requested shape, a consistent process tree and monotonic counters, and its probes read it.

#include "Platform/IProcessActions.h"
#include "Platform/ProcessTypes.h"
#include "Platform/StorageTypes.h"
#include "Platform/Synthetic/SyntheticProbes.h"
#include "Platform/Synthetic/SyntheticWorkload.h"
#include "Platform/SystemTypes.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace
{

using Platform::Synthetic::Workload;
using Platform::Synthetic::WorkloadSpec;

constexpr std::uint64_t BOOT_UNIX_SECONDS = 1'790'000'000;
[[nodiscard]] std::chrono::steady_clock::time_point epoch()
{
    return std::chrono::steady_clock::time_point{} + std::chrono::hours(1);
}
constexpr double NOW = Workload::UPTIME_AT_EPOCH_SECONDS;

[[nodiscard]] Workload makeWorkload(const WorkloadSpec& spec)
{
    return {spec, epoch(), BOOT_UNIX_SECONDS};
}

[[nodiscard]] WorkloadSpec spec(std::size_t processes, std::uint64_t seed = 1413)
{
    return WorkloadSpec{.processes = processes, .cores = 8, .disks = 3, .interfaces = 4, .seed = seed};
}

[[nodiscard]] std::vector<Platform::ProcessCounters> processesAt(const Workload& workload, double uptime)
{
    std::vector<Platform::ProcessCounters> processes;
    workload.processesAt(uptime, processes);
    return processes;
}

/// Identity (pid, start) -> counters.
[[nodiscard]] std::map<std::pair<std::int32_t, std::uint64_t>, Platform::ProcessCounters>
byIdentity(const std::vector<Platform::ProcessCounters>& processes)
{
    std::map<std::pair<std::int32_t, std::uint64_t>, Platform::ProcessCounters> result;
    for (const auto& process : processes)
    {
        result.emplace(std::pair{process.pid, process.startTimeTicks}, process);
    }
    return result;
}

} // namespace

TEST(SyntheticWorkloadTest, SameSeedGivesTheSameMachine)
{
    const Workload a = makeWorkload(spec(500));
    const Workload b = makeWorkload(spec(500));
    for (const double uptime : {NOW - 1800.0, NOW - 0.1, NOW, NOW + 3600.0})
    {
        SCOPED_TRACE(uptime);
        const auto pa = processesAt(a, uptime);
        const auto pb = processesAt(b, uptime);
        ASSERT_EQ(pa.size(), pb.size());
        for (std::size_t i = 0; i < pa.size(); ++i)
        {
            EXPECT_EQ(pa[i].pid, pb[i].pid);
            EXPECT_EQ(pa[i].parentPid, pb[i].parentPid);
            EXPECT_EQ(pa[i].name, pb[i].name);
            EXPECT_EQ(pa[i].command, pb[i].command);
            EXPECT_EQ(pa[i].userTime, pb[i].userTime);
            EXPECT_EQ(pa[i].rssBytes, pb[i].rssBytes);
            EXPECT_EQ(pa[i].netReceivedBytes, pb[i].netReceivedBytes);
        }

        Platform::SystemCounters sa;
        Platform::SystemCounters sb;
        a.systemCountersAt(uptime, sa);
        b.systemCountersAt(uptime, sb);
        EXPECT_EQ(sa.cpuTotal.total(), sb.cpuTotal.total());
        EXPECT_EQ(sa.netRxBytes, sb.netRxBytes);
        EXPECT_EQ(sa.memory.availableBytes, sb.memory.availableBytes);

        Platform::SystemDiskCounters da;
        Platform::SystemDiskCounters db;
        a.diskCountersAt(uptime, da);
        b.diskCountersAt(uptime, db);
        EXPECT_EQ(da.totalReadBytes(), db.totalReadBytes());
    }
}

TEST(SyntheticWorkloadTest, DifferentSeedsGiveDifferentMachines)
{
    const auto a = processesAt(makeWorkload(spec(300, 1)), NOW);
    const auto b = processesAt(makeWorkload(spec(300, 2)), NOW);
    ASSERT_EQ(a.size(), b.size());
    std::size_t differing = 0;
    for (std::size_t i = 0; i < a.size(); ++i)
    {
        differing += (a[i].userTime != b[i].userTime) ? 1U : 0U;
    }
    EXPECT_GT(differing, a.size() / 2);
}

TEST(SyntheticWorkloadTest, HasTheRequestedShape)
{
    for (const std::size_t processes :
         {std::size_t{1}, std::size_t{2}, std::size_t{10}, std::size_t{137}, std::size_t{2000}, std::size_t{5000}})
    {
        SCOPED_TRACE(processes);
        const Workload workload = makeWorkload(spec(processes));
        EXPECT_EQ(processesAt(workload, NOW).size(), processes);
    }

    const Workload workload = makeWorkload(WorkloadSpec{.processes = 50, .cores = 64, .disks = 12, .interfaces = 30, .seed = 3});
    Platform::SystemCounters system;
    workload.systemCountersAt(NOW, system);
    EXPECT_EQ(system.cpuPerCore.size(), 64U);
    EXPECT_EQ(system.cpuCoreCount, 64U);
    EXPECT_EQ(system.networkInterfaces.size(), 30U);
    Platform::SystemDiskCounters disks;
    workload.diskCountersAt(NOW, disks);
    ASSERT_EQ(disks.disks.size(), 12U);
    std::set<std::string> diskNames;
    for (const auto& disk : disks.disks)
    {
        diskNames.insert(disk.deviceName);
    }
    EXPECT_EQ(diskNames.size(), 12U);
}

TEST(SyntheticWorkloadTest, ClampsTheSpec)
{
    const Workload tiny = makeWorkload(WorkloadSpec{.processes = 0, .cores = 0, .disks = 0, .interfaces = 0});
    EXPECT_EQ(tiny.spec().processes, Platform::Synthetic::MIN_PROCESSES);
    EXPECT_EQ(tiny.spec().cores, Platform::Synthetic::MIN_CORES);
    const Workload huge = makeWorkload(WorkloadSpec{.processes = 1'000'000, .cores = 100'000, .disks = 1000, .interfaces = 1000});
    EXPECT_EQ(huge.spec().processes, Platform::Synthetic::MAX_PROCESSES);
    EXPECT_EQ(huge.spec().cores, Platform::Synthetic::MAX_CORES);
    EXPECT_EQ(huge.spec().disks, Platform::Synthetic::MAX_DISKS);
    EXPECT_EQ(huge.spec().interfaces, Platform::Synthetic::MAX_INTERFACES);
}

TEST(SyntheticWorkloadTest, ProcessTreeIsConsistent)
{
    const auto processes = processesAt(makeWorkload(spec(3000)), NOW);
    std::map<std::int32_t, const Platform::ProcessCounters*> byPid;
    for (const auto& process : processes)
    {
        EXPECT_TRUE(byPid.emplace(process.pid, &process).second) << "duplicate pid " << process.pid;
        EXPECT_NE(process.startTimeTicks, 0U);
        EXPECT_FALSE(process.name.empty());
    }
    std::size_t maxDepth = 0;
    for (const auto& process : processes)
    {
        if (process.parentPid == 0)
        {
            continue;
        }
        const auto parent = byPid.find(process.parentPid);
        ASSERT_NE(parent, byPid.end()) << process.name << " (" << process.pid << ") has no parent " << process.parentPid;
        EXPECT_LE(parent->second->startTimeTicks, process.startTimeTicks) << process.name;
        std::size_t depth = 1;
        for (std::int32_t ppid = process.parentPid; ppid != 0 && depth < 64; ++depth)
        {
            ppid = byPid.at(ppid)->parentPid;
        }
        maxDepth = std::max(maxDepth, depth);
    }
    // A realistic tree: init -> systemd --user -> code -> zygote -> extension host -> language server.
    EXPECT_GE(maxDepth, 5U);
    EXPECT_LT(maxDepth, 64U);
}

TEST(SyntheticWorkloadTest, CountersNeverGoBackwards)
{
    const Workload workload = makeWorkload(spec(1500));
    double previous = NOW - 1800.0;
    auto before = byIdentity(processesAt(workload, previous));
    Platform::SystemCounters systemBefore;
    Platform::SystemDiskCounters disksBefore;
    workload.systemCountersAt(previous, systemBefore);
    workload.diskCountersAt(previous, disksBefore);
    for (const double step : {0.1, 1.0, 7.3, 60.0})
    {
        SCOPED_TRACE(step);
        const double uptime = previous + step;
        const auto after = byIdentity(processesAt(workload, uptime));
        std::size_t survivors = 0;
        for (const auto& [identity, counters] : after)
        {
            const auto it = before.find(identity);
            if (it == before.end())
            {
                continue;
            }
            ++survivors;
            EXPECT_GE(counters.userTime, it->second.userTime);
            EXPECT_GE(counters.systemTime, it->second.systemTime);
            EXPECT_GE(counters.readBytes, it->second.readBytes);
            EXPECT_GE(counters.writeBytes, it->second.writeBytes);
            EXPECT_GE(counters.netSentBytes, it->second.netSentBytes);
            EXPECT_GE(counters.netReceivedBytes, it->second.netReceivedBytes);
            EXPECT_GE(counters.pageFaultCount, it->second.pageFaultCount);
            EXPECT_GE(counters.energyMicrojoules, it->second.energyMicrojoules);
        }
        EXPECT_GT(survivors, after.size() * 9 / 10);

        Platform::SystemCounters system;
        workload.systemCountersAt(uptime, system);
        for (std::size_t core = 0; core < system.cpuPerCore.size(); ++core)
        {
            EXPECT_GE(system.cpuPerCore[core].idle, systemBefore.cpuPerCore[core].idle);
            EXPECT_GE(system.cpuPerCore[core].user, systemBefore.cpuPerCore[core].user);
            EXPECT_GE(system.cpuPerCore[core].iowait, systemBefore.cpuPerCore[core].iowait);
        }
        EXPECT_GE(system.netRxBytes, systemBefore.netRxBytes);
        Platform::SystemDiskCounters disks;
        workload.diskCountersAt(uptime, disks);
        EXPECT_GE(disks.totalReadBytes(), disksBefore.totalReadBytes());
        EXPECT_GE(disks.totalWriteBytes(), disksBefore.totalWriteBytes());

        before = after;
        systemBefore = system;
        disksBefore = disks;
        previous = uptime;
    }
}

TEST(SyntheticWorkloadTest, ProcessesChurnAtAFixedCount)
{
    const Workload workload = makeWorkload(spec(2000));
    const auto first = processesAt(workload, NOW);
    const auto later = processesAt(workload, NOW + 120.0);
    EXPECT_EQ(first.size(), later.size());
    std::set<std::int32_t> firstPids;
    for (const auto& process : first)
    {
        firstPids.insert(process.pid);
    }
    std::size_t newPids = 0;
    for (const auto& process : later)
    {
        newPids += firstPids.contains(process.pid) ? 0U : 1U;
    }
    EXPECT_GT(newPids, 0U);               // build jobs and tabs came and went
    EXPECT_LT(newPids, later.size() / 5); // but most of the machine stayed
}

namespace
{

/// CPU ticks all processes alive at both @p from and @p to ran in between.
[[nodiscard]] std::uint64_t processTicksBetween(const Workload& workload, double from, double to)
{
    const auto before = byIdentity(processesAt(workload, from));
    const auto after = byIdentity(processesAt(workload, to));
    std::uint64_t ticks = 0;
    for (const auto& [identity, counters] : after)
    {
        if (const auto it = before.find(identity); it != before.end())
        {
            ticks += (counters.userTime + counters.systemTime) - (it->second.userTime + it->second.systemTime);
        }
    }
    return ticks;
}

/// Busy (active) and total ticks of every core between two system readings.
[[nodiscard]] std::pair<std::uint64_t, std::uint64_t> systemTicksBetween(const Workload& workload, double from, double to)
{
    Platform::SystemCounters before;
    Platform::SystemCounters after;
    workload.systemCountersAt(from, before);
    workload.systemCountersAt(to, after);
    std::uint64_t busy = 0;
    std::uint64_t total = 0;
    for (std::size_t core = 0; core < after.cpuPerCore.size(); ++core)
    {
        busy += after.cpuPerCore[core].active() - before.cpuPerCore[core].active();
        total += after.cpuPerCore[core].total() - before.cpuPerCore[core].total();
    }
    return {busy, total};
}

} // namespace

TEST(SyntheticWorkloadTest, SystemBusyCoversProcessWorkWithinCapacity)
{
    // Over any interval, the cores' busy time includes every process's CPU work (the Overview must not
    // show less CPU than the process table adds up to) and never exceeds the cores' capacity.
    constexpr double WINDOW_SECONDS = 1800.0;
    constexpr int POINTS = 6; // spread over the window; each covers three interval lengths
    for (const std::size_t processes : {std::size_t{300}, std::size_t{2000}})
    {
        for (const std::uint64_t seed : {std::uint64_t{1}, std::uint64_t{1413}, std::uint64_t{11828}})
        {
            for (const std::size_t cores : {std::size_t{4}, std::size_t{16}, std::size_t{64}})
            {
                SCOPED_TRACE(std::to_string(processes) + " processes, seed " + std::to_string(seed) + ", " + std::to_string(cores) +
                             " cores");
                const Workload workload =
                    makeWorkload(WorkloadSpec{.processes = processes, .cores = cores, .disks = 1, .interfaces = 1, .seed = seed});
                // Each counter is rounded to a tick on its own: allow a few ticks per process and core.
                const auto roundingTicks = static_cast<std::uint64_t>((4 * processes) + (16 * cores));
                for (int point = 0; point < POINTS; ++point)
                {
                    const double from = NOW - WINDOW_SECONDS + ((WINDOW_SECONDS / POINTS) * point);
                    for (const double interval : {0.1, 1.0, WINDOW_SECONDS / POINTS})
                    {
                        SCOPED_TRACE("from " + std::to_string(from) + " for " + std::to_string(interval) + " s");
                        const double to = from + interval;
                        const std::uint64_t processTicks = processTicksBetween(workload, from, to);
                        const auto [busy, total] = systemTicksBetween(workload, from, to);
                        EXPECT_GE(busy + roundingTicks, processTicks);
                        EXPECT_LE(busy, total);
                        const auto capacity = static_cast<double>(cores) * interval * static_cast<double>(Workload::TICKS_PER_SECOND);
                        EXPECT_NEAR(static_cast<double>(total), capacity, static_cast<double>(16 * cores));
                    }
                }
            }
        }
    }
}

TEST(SyntheticWorkloadTest, InterfaceNamesAreUnique)
{
    const auto namesOf = [](std::uint64_t seed)
    {
        const Workload workload = makeWorkload(
            WorkloadSpec{.processes = 1, .cores = 16, .disks = 0, .interfaces = Platform::Synthetic::MAX_INTERFACES, .seed = seed});
        Platform::SystemCounters system;
        workload.systemCountersAt(NOW, system);
        std::set<std::string> names;
        for (const auto& iface : system.networkInterfaces)
        {
            names.insert(iface.name);
            EXPECT_LE(iface.name.size(), 15U) << iface.name; // IFNAMSIZ - 1
        }
        return std::pair{names.size(), system.networkInterfaces.size()};
    };

    // Seed 11828 once named interfaces 252 and 254 both veth9481e92.
    const auto [unique, total] = namesOf(11828);
    EXPECT_EQ(total, Platform::Synthetic::MAX_INTERFACES);
    EXPECT_EQ(unique, total);

    for (std::uint64_t seed = 0; seed < 200; ++seed)
    {
        SCOPED_TRACE(seed);
        const auto [seedUnique, seedTotal] = namesOf(seed);
        EXPECT_EQ(seedUnique, seedTotal);
    }
}

TEST(SyntheticWorkloadTest, MachineTotalsAreSane)
{
    const Workload workload = makeWorkload(spec(2000));
    constexpr double INTERVAL = 1.0;
    const auto before = byIdentity(processesAt(workload, NOW));
    const auto after = byIdentity(processesAt(workload, NOW + INTERVAL));
    double cpuTicks = 0.0;
    double rss = 0.0;
    for (const auto& [identity, counters] : after)
    {
        rss += static_cast<double>(counters.rssBytes);
        if (const auto it = before.find(identity); it != before.end())
        {
            cpuTicks += static_cast<double>((counters.userTime + counters.systemTime) - (it->second.userTime + it->second.systemTime));
        }
    }
    const auto totalTicks = static_cast<double>(workload.totalCpuTicksAt(NOW + INTERVAL) - workload.totalCpuTicksAt(NOW));
    EXPECT_GT(cpuTicks, 0.0);
    EXPECT_LT(cpuTicks, totalTicks);
    EXPECT_LT(rss, static_cast<double>(workload.totalMemoryBytes()));

    Platform::SystemCounters system;
    workload.systemCountersAt(NOW, system);
    EXPECT_GT(system.memory.availableBytes, 0U);
    EXPECT_LT(system.memory.availableBytes, system.memory.totalBytes);
    EXPECT_GT(system.netRxBytes, 0U);
}

TEST(SyntheticWorkloadTest, ProcessTotalsMatchTheProcesses)
{
    const Workload workload = makeWorkload(spec(800));
    const auto processes = processesAt(workload, NOW);
    double threads = 0.0;
    double handles = 0.0;
    for (const auto& process : processes)
    {
        threads += process.threadCount;
        handles += process.handleCount;
    }
    const auto totals = workload.processTotalsAt(NOW);
    EXPECT_DOUBLE_EQ(totals.threadCount, threads);
    EXPECT_DOUBLE_EQ(totals.handleCount, handles);

    // Rates match the counters' movement over a short interval, as ProcessModel would compute them.
    constexpr double INTERVAL = 0.01;
    const auto before = byIdentity(processes);
    const auto after = byIdentity(processesAt(workload, NOW + INTERVAL));
    double received = 0.0;
    double faults = 0.0;
    double energy = 0.0;
    for (const auto& [identity, counters] : after)
    {
        if (const auto it = before.find(identity); it != before.end())
        {
            received += static_cast<double>(counters.netReceivedBytes - it->second.netReceivedBytes);
            faults += static_cast<double>(counters.pageFaultCount - it->second.pageFaultCount);
            energy += static_cast<double>(counters.energyMicrojoules - it->second.energyMicrojoules);
        }
    }
    const auto mid = workload.processTotalsAt(NOW + (INTERVAL / 2.0));
    EXPECT_NEAR(received / INTERVAL, mid.netReceivedBytesPerSec, mid.netReceivedBytesPerSec * 0.05);
    EXPECT_NEAR(faults / INTERVAL, mid.pageFaultsPerSec, mid.pageFaultsPerSec * 0.05);
    EXPECT_NEAR(energy / INTERVAL / 1e6, mid.powerWatts, mid.powerWatts * 0.05);
}

TEST(SyntheticWorkloadTest, WaveIntegralMatchesItsRate)
{
    Workload::Wave wave;
    wave.base = 3.0;
    wave.harmonics[0] = {.amplitude = 0.5, .phase = 0.3, .band = 2};
    wave.harmonics[1] = {.amplitude = 0.3, .phase = 1.1, .band = 6};
    constexpr double H = 1e-4;
    for (const double t : {0.0, 10.0, 1234.5, Workload::UPTIME_AT_EPOCH_SECONDS})
    {
        SCOPED_TRACE(t);
        EXPECT_GT(wave.rate(t), 0.0);
        EXPECT_NEAR((wave.integral(t + H) - wave.integral(t - H)) / (2.0 * H), wave.rate(t), 1e-3);
    }
    EXPECT_DOUBLE_EQ(wave.integral(0.0), 0.0);
}

TEST(SyntheticProbesTest, ProbesReadTheWorkload)
{
    const auto workload = std::make_shared<const Workload>(spec(250));
    Platform::Synthetic::SyntheticProcessProbe processProbe(workload);
    const auto processes = processProbe.enumerate();
    EXPECT_EQ(processes.size(), 250U);
    EXPECT_GT(processProbe.totalCpuTime(), 0U);
    EXPECT_EQ(processProbe.ticksPerSecond(), Workload::TICKS_PER_SECOND);
    EXPECT_EQ(processProbe.systemTotalMemory(), workload->totalMemoryBytes());
    EXPECT_TRUE(processProbe.capabilities().hasNetworkCounters);
    EXPECT_FALSE(processProbe.readPackageEnergy().has_value());

    Platform::Synthetic::SyntheticSystemProbe systemProbe(workload);
    EXPECT_EQ(systemProbe.read().cpuPerCore.size(), 8U);
    EXPECT_TRUE(systemProbe.capabilities().hasPerCoreCpu);

    Platform::Synthetic::SyntheticDiskProbe diskProbe(workload);
    EXPECT_EQ(diskProbe.read().disks.size(), 3U);
    EXPECT_TRUE(diskProbe.capabilities().hasDiskStats);
}

TEST(SyntheticProbesTest, ProcessActionsAreRefused)
{
    Platform::Synthetic::SyntheticProcessActions actions;
    const auto caps = actions.actionCapabilities();
    EXPECT_FALSE(caps.canTerminate || caps.canKill || caps.canStop || caps.canContinue || caps.canSetPriority);
    const Platform::ProcessTarget target{.pid = 1, .startTimeTicks = 100};
    EXPECT_FALSE(actions.terminate(target).success);
    EXPECT_FALSE(actions.kill(target).success);
    EXPECT_FALSE(actions.stop(target).success);
    EXPECT_FALSE(actions.resume(target).success);
    const auto result = actions.setPriority(target, 10);
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.errorMessage.empty());
}
