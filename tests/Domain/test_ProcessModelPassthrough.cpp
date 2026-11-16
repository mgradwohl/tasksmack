/// @file test_ProcessModelPassthrough.cpp
/// @brief Domain::ProcessModel: priority, publisher, type and GDI pass-through, thread safety, watched processes, a throwing GPU merge and
/// unread values. Split from test_ProcessModel.cpp by feature (#3850); the shared helpers are in ProcessModelTestSupport.h.

#include "Domain/GPUModel.h"
#include "Domain/PriorityConfig.h"
#include "Domain/ProcessModel.h"
#include "Domain/ProcessSnapshot.h"
#include "Domain/SamplingConfig.h"
#include "Mocks/MockGPUProbe.h"
#include "Mocks/MockProbes.h"
#include "Platform/GPUTypes.h"
#include "Platform/IProcessProbe.h"
#include "Platform/ImageSignature.h"
#include "Platform/ProcessTypes.h"
#include "Platform/SchedPolicy.h"
#include "ProcessModelTestSupport.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <future>
#include <latch>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// Use shared mock from TestMocks namespace
using ProcessModelTesting::makeCounter;
using ProcessModelTesting::ManualClock;
using ProcessModelTesting::OVERFLOW_TEST_MARGIN;
using ProcessModelTesting::systemHistories;
using TestMocks::makeProcessCounters;
using TestMocks::MockGPUProbe;
using TestMocks::MockProcessProbe;

// =============================================================================
// Priority class passthrough (#1280)
// =============================================================================

TEST(ProcessModelTest, EveryPlatformPriorityClassReachesTheSnapshot)
{
    const std::array<std::pair<Platform::PriorityClass, Domain::Priority::PriorityClass>, 7> cases{{
        {Platform::PriorityClass::None, Domain::Priority::PriorityClass::None},
        {Platform::PriorityClass::Idle, Domain::Priority::PriorityClass::Idle},
        {Platform::PriorityClass::BelowNormal, Domain::Priority::PriorityClass::BelowNormal},
        {Platform::PriorityClass::Normal, Domain::Priority::PriorityClass::Normal},
        {Platform::PriorityClass::AboveNormal, Domain::Priority::PriorityClass::AboveNormal},
        {Platform::PriorityClass::High, Domain::Priority::PriorityClass::High},
        {Platform::PriorityClass::Realtime, Domain::Priority::PriorityClass::Realtime},
    }};
    for (const auto& [platformClass, domainClass] : cases)
    {
        auto probe = std::make_unique<MockProcessProbe>();
        Platform::ProcessCounters counter = makeCounter(100, "app", 'R', 1000, 500);
        counter.nice = Domain::Priority::MIN_NICE;
        counter.priorityClass = platformClass;
        probe->setCounters({counter});
        probe->setTotalCpuTime(100000);

        Domain::ProcessModel model(std::move(probe));
        model.refresh();

        const auto snaps = model.snapshots();
        ASSERT_EQ(snaps.size(), 1);
        EXPECT_EQ(snaps[0].priorityClass, domainClass) << static_cast<int>(platformClass);
        EXPECT_EQ(snaps[0].nice, Domain::Priority::MIN_NICE);
    }
}

TEST(ProcessModelTest, RealtimeIsLabelledRealtimeNotHigh)
{
    auto probe = std::make_unique<MockProcessProbe>();
    Platform::ProcessCounters counter = makeCounter(100, "app", 'R', 1000, 500);
    counter.nice = Domain::Priority::MIN_NICE; // What the Windows probe reports for Realtime
    counter.priorityClass = Platform::PriorityClass::Realtime;
    probe->setCounters({counter});
    probe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    const auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1);
    EXPECT_EQ(Domain::Priority::getProcessPriorityLabel(snaps[0].priorityClass, snaps[0].nice), "Realtime");
}

// =============================================================================
// Publisher, Type, and GDI Object Passthrough Tests (Issues #184, #185, #195)
// =============================================================================

TEST(ProcessModelTest, WhenProbeReturnsPublisher_ThenSnapshotContainsPublisher)
{
    auto probe = std::make_unique<MockProcessProbe>();
    Platform::ProcessCounters counter = makeCounter(100, "app", 'R', 1000, 500);
    counter.publisher = "Test Corporation";
    probe->setCounters({counter});
    probe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    const auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1);
    EXPECT_EQ(snaps[0].publisher, "Test Corporation");
}

TEST(ProcessModelTest, WhenProbeReturnsEmptyPublisher_ThenSnapshotPublisherIsEmpty)
{
    auto probe = std::make_unique<MockProcessProbe>();
    Platform::ProcessCounters counter = makeCounter(100, "app", 'R', 1000, 500);
    counter.publisher = "";
    probe->setCounters({counter});
    probe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    const auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1);
    EXPECT_TRUE(snaps[0].publisher.empty());
}

TEST(ProcessModelTest, WhenProbeReturnsProcessType_ThenSnapshotContainsProcessType)
{
    auto probe = std::make_unique<MockProcessProbe>();
    Platform::ProcessCounters counter = makeCounter(100, "app", 'R', 1000, 500);
    counter.processType = "App";
    probe->setCounters({counter});
    probe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    const auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1);
    EXPECT_EQ(snaps[0].processType, "App");
}

TEST(ProcessModelTest, TheArchitectureIsPassedThroughToTheSnapshot)
{
    // #2394: the probe words it; Domain carries it to the snapshot as it is.
    auto probe = std::make_unique<MockProcessProbe>();
    Platform::ProcessCounters counter = makeCounter(100, "app", 'R', 1000, 500);
    counter.architecture = "x86 (WOW64)";
    probe->setCounters({counter});
    probe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    const auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1);
    EXPECT_EQ(snaps[0].architecture, "x86 (WOW64)");
}

TEST(ProcessModelTest, TheSignatureReachesTheSnapshotWithItsLabel)
{
    // #3070: the probe's signature check passes through; the column's text is worded from it where drawn (#3244).
    auto probe = std::make_unique<MockProcessProbe>();
    Platform::ProcessCounters counter = makeCounter(100, "explorer.exe", 'S', 1000, 500);
    counter.signature =
        Platform::SignatureCheck{.signature = Platform::ImageSignature::Embedded, .note = {}, .signer = "Microsoft Windows"};
    Platform::ProcessCounters unsignedCounter = makeCounter(101, "tool.exe", 'S', 1000, 500);
    unsignedCounter.signature = Platform::SignatureCheck{.signature = Platform::ImageSignature::Unsigned, .note = {}, .signer = {}};
    Platform::ProcessCounters pending = makeCounter(102, "new.exe", 'S', 1000, 500);
    probe->setCounters({counter, unsignedCounter, pending});
    probe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    const auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 3);
    const auto byPid = [&snaps](std::int32_t pid)
    {
        return *std::ranges::find(snaps, pid, &Domain::ProcessSnapshot::pid);
    };
    EXPECT_EQ(byPid(100).signature.get().signature, Platform::ImageSignature::Embedded);
    EXPECT_EQ(Platform::signatureLabel(byPid(100).signature.get()), "Microsoft Windows");
    EXPECT_EQ(Platform::signatureLabel(byPid(101).signature.get()), "Unsigned");
    EXPECT_EQ(Platform::signatureLabel(byPid(102).signature.get()), "") << "not checked yet";
}

TEST(ProcessModelTest, TheSchedulingPolicyReachesTheSnapshotWithItsLabel)
{
    // #3086: the policy and real-time priority pass through; the label is worded from them where drawn (#3244).
    auto probe = std::make_unique<MockProcessProbe>();
    Platform::ProcessCounters counter = makeCounter(100, "pipewire", 'S', 1000, 500);
    counter.schedPolicy = Platform::SchedPolicy::Fifo;
    counter.rtPriority = 50;
    probe->setCounters({counter});
    probe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    const auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1);
    EXPECT_EQ(snaps[0].schedPolicy, Platform::SchedPolicy::Fifo);
    EXPECT_EQ(snaps[0].rtPriority, 50U);
    EXPECT_EQ(Platform::schedPolicyText(snaps[0].schedPolicy, snaps[0].rtPriority).view(), "FIFO 50");
}

TEST(ProcessModelTest, TheIoPriorityReachesTheSnapshot)
{
    // #3551: the probe's I/O priority passes through as read, unknown included; it's worded where it's drawn.
    auto probe = std::make_unique<MockProcessProbe>();
    Platform::ProcessCounters known = makeCounter(100, "rsync", 'S', 1000, 500);
    known.ioPriority = Platform::IoPriority{.ioClass = Platform::IoPriorityClass::Idle, .level = 0};
    const Platform::ProcessCounters unknown = makeCounter(101, "kworker/0:1", 'I', 1000, 500);
    probe->setCounters({known, unknown});
    probe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    const auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 2U);
    const auto byPid = [&snaps](std::int32_t pid)
    {
        return *std::ranges::find(snaps, pid, &Domain::ProcessSnapshot::pid);
    };
    EXPECT_EQ(byPid(100).ioPriority, (Platform::IoPriority{.ioClass = Platform::IoPriorityClass::Idle, .level = 0}));
    EXPECT_FALSE(byPid(101).ioPriority.has_value());
}

TEST(ProcessModelTest, WhenProbeReturnsBackgroundProcessType_ThenSnapshotContainsIt)
{
    auto probe = std::make_unique<MockProcessProbe>();
    Platform::ProcessCounters counter = makeCounter(100, "svc", 'R', 1000, 500);
    counter.processType = "Background Process";
    probe->setCounters({counter});
    probe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    const auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1);
    EXPECT_EQ(snaps[0].processType, "Background Process");
}

TEST(ProcessModelTest, WhenProbeReturnsGdiObjectCount_ThenSnapshotContainsCount)
{
    auto probe = std::make_unique<MockProcessProbe>();
    Platform::ProcessCounters counter = makeCounter(100, "app", 'R', 1000, 500);
    counter.gdiObjectCount = std::int32_t{42};
    probe->setCounters({counter});
    probe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    const auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1);
    ASSERT_TRUE(snaps[0].gdiObjectCount.has_value());
    EXPECT_EQ(*snaps[0].gdiObjectCount, 42);
}

TEST(ProcessModelTest, WhenProbeReturnsZeroGdiObjectCount_ThenSnapshotCountIsZero)
{
    auto probe = std::make_unique<MockProcessProbe>();
    Platform::ProcessCounters counter = makeCounter(100, "proc", 'R', 1000, 500);
    counter.gdiObjectCount = std::int32_t{0};
    probe->setCounters({counter});
    probe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    const auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1);
    ASSERT_TRUE(snaps[0].gdiObjectCount.has_value());
    EXPECT_EQ(*snaps[0].gdiObjectCount, 0);
}

TEST(ProcessModelTest, ProcessTreeHierarchyIsCorrectlyBuilt)
{
    auto probe = std::make_unique<MockProcessProbe>();
    // proc_1 (pid=1, parentPid=0) -> root
    // proc_2 (pid=2, parentPid=1) -> child of proc_1
    // proc_3 (pid=3, parentPid=2) -> child of proc_2
    // proc_4 (pid=4, parentPid=1) -> child of proc_1
    // proc_5 (pid=5, parentPid=999) -> missing parent, acts as root
    probe->setCounters({makeCounter(1, "proc_1", 'R', 1000, 500, 1000, 1024, 0),
                        makeCounter(2, "proc_2", 'R', 1000, 500, 1000, 1024, 1),
                        makeCounter(3, "proc_3", 'R', 1000, 500, 1000, 1024, 2),
                        makeCounter(4, "proc_4", 'R', 1000, 500, 1000, 1024, 1),
                        makeCounter(5, "proc_5", 'R', 1000, 500, 1000, 1024, 999)});
    probe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    const auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 5);

    // Verify proc_1 has children (proc_2 and proc_4)
    EXPECT_EQ(snaps[0].pid, 1);
    EXPECT_EQ(snaps[0].childrenIndices.size(), 2);
    EXPECT_TRUE(std::find(snaps[0].childrenIndices.begin(), snaps[0].childrenIndices.end(), 1) !=
                snaps[0].childrenIndices.end()); // proc_2 is at index 1
    EXPECT_TRUE(std::find(snaps[0].childrenIndices.begin(), snaps[0].childrenIndices.end(), 3) !=
                snaps[0].childrenIndices.end()); // proc_4 is at index 3

    // Verify proc_2 has child (proc_3)
    EXPECT_EQ(snaps[1].pid, 2);
    EXPECT_EQ(snaps[1].childrenIndices.size(), 1);
    EXPECT_EQ(snaps[1].childrenIndices[0], 2); // proc_3 is at index 2

    // Verify proc_3, proc_4, proc_5 have no children
    EXPECT_EQ(snaps[2].pid, 3);
    EXPECT_TRUE(snaps[2].childrenIndices.empty());

    EXPECT_EQ(snaps[3].pid, 4);
    EXPECT_TRUE(snaps[3].childrenIndices.empty());

    EXPECT_EQ(snaps[4].pid, 5);
    EXPECT_TRUE(snaps[4].childrenIndices.empty());
}

TEST(ProcessModelTest, ProcessTreeHandlesPidReuseCorrectly)
{
    auto probe = std::make_unique<MockProcessProbe>();
    // A parent process dies, and its PID is reused by a completely unrelated process within the
    // same sampling interval. Child processes still pointing to the old parent PID must NOT be
    // linked to the new process, since that would misattach them to an unrelated process (#595).

    // pid=1 is the *new* process (start time 2000, i.e. it started after this snapshot's older
    // processes). proc_2 (start time 1000) reports parentPid=1, but since a real parent must
    // start at or before its child, pid=1's current holder cannot be proc_2's actual parent --
    // that was a different, already-exited process that used to hold PID 1.
    probe->setCounters(
        {makeCounter(1, "new_proc", 'R', 1000, 500, 2000, 1024, 0), makeCounter(2, "child_proc", 'R', 1000, 500, 1000, 1024, 1)});
    probe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    const auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 2);

    // new_proc must NOT have child_proc linked as a child -- the start-time ordering rules
    // out pid=1's current holder as the real parent, so child_proc is treated as unparented.
    EXPECT_EQ(snaps[0].pid, 1);
    EXPECT_TRUE(snaps[0].childrenIndices.empty());
}

TEST(ProcessModelTest, ProcessTreeLinksChildWhenParentStartedFirst)
{
    auto probe = std::make_unique<MockProcessProbe>();
    // Normal (non-reuse) case: parent genuinely started before its child, so the start-time
    // guard added for #595 must not reject a legitimate parent/child link.
    probe->setCounters(
        {makeCounter(1, "parent_proc", 'R', 1000, 500, 1000, 1024, 0), makeCounter(2, "child_proc", 'R', 1000, 500, 2000, 1024, 1)});
    probe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    const auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 2);

    EXPECT_EQ(snaps[0].pid, 1);
    ASSERT_EQ(snaps[0].childrenIndices.size(), 1);
    EXPECT_EQ(snaps[0].childrenIndices[0], 1);
}

TEST(ProcessModelTest, ProcessTreeLinksChildWhenStartTimesUnavailable)
{
    auto probe = std::make_unique<MockProcessProbe>();
    // When a probe can't report start times (startTimeTicks == 0 for both), the guard has no
    // data to validate against and must fall back to linking by PID alone rather than silently
    // dropping every parent/child relationship.
    probe->setCounters(
        {makeCounter(1, "parent_proc", 'R', 1000, 500, 0, 1024, 0), makeCounter(2, "child_proc", 'R', 1000, 500, 0, 1024, 1)});
    probe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    const auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 2);

    EXPECT_EQ(snaps[0].pid, 1);
    ASSERT_EQ(snaps[0].childrenIndices.size(), 1);
    EXPECT_EQ(snaps[0].childrenIndices[0], 1);
}

// A PID listed twice (two processes the probe saw holding it, told apart by start time) maps to its
// last row, as the std::unordered_map the tree build used before #2981 did: the child links there.
TEST(ProcessModelTest, ProcessTreeLinksAChildToTheLastRowOfARepeatedPid)
{
    auto probe = std::make_unique<MockProcessProbe>();
    probe->setCounters({makeCounter(10, "first_holder", 'R', 1000, 500, 500, 1024, 0),
                        makeCounter(10, "second_holder", 'R', 1000, 500, 600, 1024, 0),
                        makeCounter(20, "child_proc", 'R', 1000, 500, 2000, 1024, 10)});
    probe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    const auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 3);
    EXPECT_EQ(snaps[0].name, "first_holder");
    EXPECT_TRUE(snaps[0].childrenIndices.empty());
    EXPECT_EQ(snaps[1].name, "second_holder");
    ASSERT_EQ(snaps[1].childrenIndices.size(), 1);
    EXPECT_EQ(snaps[1].childrenIndices[0], 2);
    EXPECT_TRUE(snaps[2].childrenIndices.empty());
}

// A process reporting itself as its parent is never its own child; its real children still link.
TEST(ProcessModelTest, ProcessTreeNeverMakesAProcessItsOwnChild)
{
    auto probe = std::make_unique<MockProcessProbe>();
    probe->setCounters(
        {makeCounter(5, "self_parent", 'R', 1000, 500, 1000, 1024, 5), makeCounter(6, "child_proc", 'R', 1000, 500, 2000, 1024, 5)});
    probe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    const auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 2);
    ASSERT_EQ(snaps[0].childrenIndices.size(), 1);
    EXPECT_EQ(snaps[0].childrenIndices[0], 1);
    EXPECT_TRUE(snaps[1].childrenIndices.empty());
}

// A parent PID that no listed process holds links nothing, and a parentPid of 0 is a root.
TEST(ProcessModelTest, ProcessTreeLeavesChildrenOfMissingParentsUnlinked)
{
    auto probe = std::make_unique<MockProcessProbe>();
    probe->setCounters({makeCounter(1, "root_proc", 'R', 1000, 500, 1000, 1024, 0),
                        makeCounter(2, "orphan_a", 'R', 1000, 500, 2000, 1024, 999),
                        makeCounter(3, "orphan_b", 'R', 1000, 500, 2000, 1024, 4)});
    probe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    const auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 3);
    for (const auto& snap : snaps)
    {
        EXPECT_TRUE(snap.childrenIndices.empty()) << snap.name;
    }
}

// The PID-reuse guard across refreshes, with the PID index reused between them (#2981): a child
// linked to its parent stops being linked once a process that started after it holds that PID.
TEST(ProcessModelTest, ProcessTreeDropsTheLinkWhenTheParentPidIsReused)
{
    auto probe = std::make_unique<MockProcessProbe>();
    auto* probePtr = probe.get();
    probe->setCounters(
        {makeCounter(1, "parent_proc", 'R', 1000, 500, 1000, 1024, 0), makeCounter(2, "child_proc", 'R', 1000, 500, 2000, 1024, 1)});
    probe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();
    {
        const auto snaps = model.snapshots();
        ASSERT_EQ(snaps.size(), 2);
        ASSERT_EQ(snaps[0].childrenIndices.size(), 1);
        EXPECT_EQ(snaps[0].childrenIndices[0], 1);
    }

    // The parent exits and an unrelated process (started at 3000, after the child) takes PID 1.
    probePtr->setCounters(
        {makeCounter(1, "new_holder", 'R', 1000, 500, 3000, 1024, 0), makeCounter(2, "child_proc", 'R', 1100, 550, 2000, 1024, 1)});
    probePtr->setTotalCpuTime(200000);
    model.refresh();

    const auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 2);
    EXPECT_EQ(snaps[0].name, "new_holder");
    EXPECT_TRUE(snaps[0].childrenIndices.empty());
}

// Repeated refreshes of a stable 1,000-process tree, the case the reusable PID index exists for
// (#2981): every refresh gives each process exactly its children, none carried over from the last
// one; then a process leaving shifts the rows, and the links follow the new rows.
TEST(ProcessModelTest, ProcessTreeIsRebuiltExactlyOnEveryRefreshOfAStableSet)
{
    constexpr std::int32_t PROCESS_COUNT = 1000;
    constexpr std::int32_t FIRST_PID = 100;
    // pid FIRST_PID + i has parent FIRST_PID + (i - 1) / 2: a binary tree rooted at FIRST_PID.
    const auto makeTree = [&](std::int32_t skippedPid)
    {
        std::vector<Platform::ProcessCounters> counters;
        for (std::int32_t i = 0; i < PROCESS_COUNT; ++i)
        {
            const std::int32_t pid = FIRST_PID + i;
            if (pid == skippedPid)
            {
                continue;
            }
            const std::int32_t parentPid = i == 0 ? 0 : FIRST_PID + ((i - 1) / 2);
            counters.push_back(makeCounter(pid, "proc", 'S', 1000, 500, 1000 + static_cast<std::uint64_t>(i), 1024, parentPid));
        }
        return counters;
    };
    const auto expectTree = [](const std::vector<Domain::ProcessSnapshot>& snaps)
    {
        std::unordered_map<std::int32_t, std::size_t> rowOf;
        for (std::size_t row = 0; row < snaps.size(); ++row)
        {
            rowOf[snaps[row].pid] = row;
        }
        std::vector<std::vector<std::size_t>> expected(snaps.size());
        for (std::size_t row = 0; row < snaps.size(); ++row)
        {
            const auto parent = rowOf.find(snaps[row].parentPid);
            if (parent != rowOf.end())
            {
                expected[parent->second].push_back(row);
            }
        }
        for (std::size_t row = 0; row < snaps.size(); ++row)
        {
            ASSERT_EQ(snaps[row].childrenIndices, expected[row]) << "pid " << snaps[row].pid;
        }
    };

    auto probe = std::make_unique<MockProcessProbe>();
    auto* probePtr = probe.get();
    probe->setCounters(makeTree(0));
    probe->setTotalCpuTime(100000);
    Domain::ProcessModel model(std::move(probe));

    for (int refresh = 0; refresh < 3; ++refresh)
    {
        probePtr->setTotalCpuTime(100000 * static_cast<std::uint64_t>(refresh + 1));
        model.refresh();
        const auto snaps = model.snapshots();
        ASSERT_EQ(snaps.size(), static_cast<std::size_t>(PROCESS_COUNT));
        ASSERT_EQ(snaps[0].childrenIndices, (std::vector<std::size_t>{1, 2}));
        expectTree(snaps);
    }

    // PID 101 (row 1) exits: its children are left unlinked and every later row moves up one.
    probePtr->setCounters(makeTree(FIRST_PID + 1));
    probePtr->setTotalCpuTime(400000);
    model.refresh();
    const auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), static_cast<std::size_t>(PROCESS_COUNT - 1));
    EXPECT_EQ(snaps[0].childrenIndices, (std::vector<std::size_t>{1})); // PID 102, now row 1
    expectTree(snaps);
}

// =============================================================================
// Thread Safety Tests
// =============================================================================

TEST(ProcessModelTest, ConcurrentRefreshAndReadDoesNotCrash)
{
    auto probe = std::make_unique<MockProcessProbe>();
    probe->setCounters({makeCounter(1, "proc1", 'R', 100, 100, 1000, 1024, 0), makeCounter(2, "proc2", 'S', 200, 200, 1000, 2048, 1)});
    probe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));

    std::atomic<bool> keepRunning{true};

    // Writer thread rapidly refreshes the model
    std::thread writer(
        [&]()
        {
            while (keepRunning)
            {
                model.refresh();
            }
        });

    // Reader thread rapidly copies snapshots
    std::thread reader(
        [&]()
        {
            while (keepRunning)
            {
                auto snaps = model.snapshots();
                if (!snaps.empty())
                {
                    EXPECT_EQ(snaps[0].pid, 1);
                }
            }
        });

    // Let them fight for 100ms
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    keepRunning = false;

    writer.join();
    reader.join();
}

// #2156: a reader holds the system histories' views and reads every sample with no lock while the
// sampler keeps appending, compacting into new blocks and evicting the oldest samples. A view's
// samples never change after it was taken (a race here is what TSan builds would report), and the
// five series of a generation are always aligned.
//
// No deadline or sleeps (#2397, as #2207 and #1797): the refreshes start once the reader has read, and
// each one waits until the reader has seen the generation it published, so the reader sees every
// generation however a loaded machine schedules the two threads. Between generations the reader keeps
// re-reading the one it holds while the next refresh appends.
TEST(ProcessModelTest, SystemHistoryViewsStayUnchangedWhileTheSamplerAppends)
{
    auto probe = std::make_unique<MockProcessProbe>();
    probe->setCounters({makeCounter(1, "proc1", 'R', 100, 100, 1000, 1024, 0)});
    probe->setTotalCpuTime(100000);
    ManualClock clock; // advanced only by this thread, the one calling refresh()
    Domain::ProcessModel model(std::move(probe), clock.now());
    // The smallest window, so the histories reach their capacity and evict while the test runs.
    model.setMaxHistorySeconds(Domain::Sampling::HISTORY_SECONDS_MIN);

    // Three windows' worth of samples at the fastest cadence: the histories fill, compact into new
    // blocks and evict their oldest samples.
    constexpr auto REFRESH_INTERVAL = std::chrono::milliseconds(Domain::Sampling::REFRESH_INTERVAL_MIN_MS);
    constexpr int REFRESHES = 3 * Domain::Sampling::HISTORY_SECONDS_MIN * (1000 / Domain::Sampling::REFRESH_INTERVAL_MIN_MS);

    std::atomic<std::uint64_t> seenVersion{0};
    std::latch firstRead{1};
    Domain::ProcessSystemHistories held; // kept across generations, re-read on every pass
    int generations = 0;

    auto reader = std::jthread(
        [&](const std::stop_token& st)
        {
            std::vector<double> heldTimestamps;
            bool firstReadDone = false;
            while (!st.stop_requested())
            {
                Domain::ProcessSystemHistories latest;
                const bool newer = model.tryCopySystemHistoriesIfNewer(seenVersion.load(), latest);
                if (!firstReadDone)
                {
                    firstReadDone = true;
                    firstRead.count_down();
                }

                // The generation held from before still reads exactly as it did when it was taken.
                EXPECT_TRUE(std::ranges::equal(held.timestamps, heldTimestamps));
                if (!newer)
                {
                    continue;
                }

                ++generations;
                EXPECT_EQ(latest.power.size(), latest.timestamps.size());
                EXPECT_EQ(latest.pageFaults.size(), latest.timestamps.size());
                EXPECT_EQ(latest.threadCount.size(), latest.timestamps.size());
                EXPECT_EQ(latest.handleCount.size(), latest.timestamps.size());
                EXPECT_TRUE(std::ranges::is_sorted(latest.timestamps));

                held = std::move(latest);
                heldTimestamps.assign(held.timestamps.begin(), held.timestamps.end());
                seenVersion.store(held.version);
                seenVersion.notify_all();
            }
        });

    firstRead.wait();
    for (int i = 0; i < REFRESHES; ++i)
    {
        // The reader has seen the latest generation, so this changes once it has seen the next one.
        const std::uint64_t before = seenVersion.load();
        model.refresh();
        clock.advance(REFRESH_INTERVAL);
        seenVersion.wait(before);
    }

    reader.request_stop();
    reader.join();

    EXPECT_EQ(generations, REFRESHES);
    // The window evicted: the last generation holds about one window of samples, not every refresh's.
    EXPECT_LT(held.timestamps.size(), static_cast<std::size_t>(REFRESHES / 2));
    EXPECT_FALSE(held.timestamps.empty());
}

// =============================================================================
// Watched-process samples (#1098, #1172)
// =============================================================================

namespace
{

double steadySeconds(Domain::ProcessModel::Clock::time_point time)
{
    return std::chrono::duration<double>(time.time_since_epoch()).count();
}

} // namespace

TEST(ProcessModelTest, WatchedSamplesCarryEachGenerationsOwnSampleTime)
{
    // #1098: Process Details stamped history with the UI frame time that noticed
    // a new generation. Each sample now carries the time the model sampled it
    // (its NowFunction), the timebase of the Overview's process aggregates.
    auto probe = std::make_unique<MockProcessProbe>();
    probe->setCounters({makeCounter(100, "watched", 'R', 1000, 0, 5000)});
    probe->setTotalCpuTime(100000);
    ManualClock clock;
    clock.advance(std::chrono::seconds(1000));
    const auto now = clock.now();
    Domain::ProcessModel model(std::move(probe), now);
    model.watchProcess(100);

    model.refresh();
    const double firstTime = steadySeconds(now());
    clock.advance(std::chrono::milliseconds(250));
    model.refresh();
    const double secondTime = steadySeconds(now());

    std::vector<Domain::ProcessSample> samples;
    ASSERT_TRUE(model.watchedSamplesSince(0, samples));
    ASSERT_EQ(samples.size(), 2U);
    EXPECT_DOUBLE_EQ(samples[0].sampleTimeSeconds, firstTime);
    EXPECT_DOUBLE_EQ(samples[1].sampleTimeSeconds, secondTime);
    EXPECT_EQ(samples[0].version, 1U);
    EXPECT_EQ(samples[1].version, 2U);
    ASSERT_NE(samples[1].snapshot, nullptr);
    EXPECT_EQ(samples[1].snapshot->pid, 100);

    // The same timebase as the model's own history timestamps.
    const auto timestamps = systemHistories(model).timestamps;
    ASSERT_FALSE(timestamps.empty());
    EXPECT_DOUBLE_EQ(timestamps.back(), secondTime);
}

TEST(ProcessModelTest, WatchedSamplesKeepEveryGenerationPublishedBetweenPolls)
{
    // #1098: a reader that polls once per frame saw only the latest generation,
    // so two publishes between frames lost one. Every generation since the
    // reader's last poll is returned, oldest first.
    auto probe = std::make_unique<MockProcessProbe>();
    auto* rawProbe = probe.get();
    rawProbe->setCounters({makeCounter(100, "watched", 'R', 1000, 0, 5000)});
    rawProbe->setTotalCpuTime(100000);
    ManualClock clock;
    Domain::ProcessModel model(std::move(probe), clock.now());
    model.watchProcess(100);

    model.refresh();
    std::vector<Domain::ProcessSample> samples;
    ASSERT_TRUE(model.watchedSamplesSince(0, samples));
    ASSERT_EQ(samples.size(), 1U);
    const std::uint64_t lastSeen = samples.back().version;

    // Three generations before the next poll.
    for (std::uint64_t i = 1; i <= 3; ++i)
    {
        clock.advance(std::chrono::milliseconds(100));
        rawProbe->setCounters({makeCounter(100, "watched", 'R', 1000 + (100 * i), 0, 5000)});
        model.refresh();
    }

    samples.clear();
    ASSERT_TRUE(model.watchedSamplesSince(lastSeen, samples));
    ASSERT_EQ(samples.size(), 3U);
    for (std::size_t i = 0; i < samples.size(); ++i)
    {
        EXPECT_EQ(samples[i].version, lastSeen + 1 + i) << "consecutive generations, none skipped";
        EXPECT_NE(samples[i].snapshot, nullptr);
    }
    EXPECT_LT(samples[0].sampleTimeSeconds, samples[1].sampleTimeSeconds);
    EXPECT_LT(samples[1].sampleTimeSeconds, samples[2].sampleTimeSeconds);
}

TEST(ProcessModelTest, WatchedSamplesCarryTheirOwnGenerationsCounterSupport)
{
    // #1210: when the probe withdraws its network counters between two generations a reader takes in
    // one batch, the earlier sample must still say they were supported, so its reading is kept.
    auto probe = std::make_unique<MockProcessProbe>();
    auto* rawProbe = probe.get();
    Platform::ProcessCapabilities caps;
    caps.hasIoCounters = true;
    caps.hasNetworkCounters = true;
    rawProbe->setCapabilities(caps);
    rawProbe->setCounters({makeCounter(100, "watched", 'R', 1000, 0, 5000)});
    rawProbe->setTotalCpuTime(100000);
    ManualClock clock;
    Domain::ProcessModel model(std::move(probe), clock.now());
    model.watchProcess(100);

    model.refresh();
    clock.advance(std::chrono::milliseconds(100));
    Platform::ProcessCapabilities withdrawn = caps;
    withdrawn.hasNetworkCounters = false;
    rawProbe->switchCapabilitiesOnNextSocketRead(withdrawn);
    rawProbe->setCounters({makeCounter(100, "watched", 'R', 1100, 0, 5000)});
    model.refresh();

    std::vector<Domain::ProcessSample> samples;
    ASSERT_TRUE(model.watchedSamplesSince(0, samples));
    ASSERT_EQ(samples.size(), 2U);
    EXPECT_TRUE(samples[0].networkCountersSupported);
    EXPECT_FALSE(samples[1].networkCountersSupported);
    EXPECT_TRUE(samples[0].ioCountersSupported);
    EXPECT_TRUE(samples[1].ioCountersSupported);
}

TEST(ProcessModelTest, WatchedSamplesCarryTheirOwnGenerationsGpuSupport)
{
    // #1210: the GPU model can lose per-process utilization on re-enumeration, on its own sampler.
    // A reader taking both generations in one batch must see the earlier one as supported, so its
    // real utilization is kept, and the later one as not.
    auto processProbe = std::make_unique<MockProcessProbe>();
    processProbe->setCounters({makeCounter(100, "watched", 'R', 1000, 0, 5000)});
    processProbe->setTotalCpuTime(100000);
    auto* rawProcessProbe = processProbe.get();

    auto gpuProbe = std::make_unique<MockGPUProbe>();
    Platform::GPUCapabilities caps;
    caps.hasPerProcessMetrics = true;
    caps.hasPerProcessUtilization = true;
    gpuProbe->withCapabilities(caps);
    gpuProbe->withGPU("GPU0", "Test GPU", "TestVendor").withProcessGPU(100, "GPU0", 1024ULL * 1024);
    auto* rawGpuProbe = gpuProbe.get();
    auto gpuModel = std::make_shared<Domain::GPUModel>(std::move(gpuProbe));

    ManualClock clock;
    Domain::ProcessModel model(std::move(processProbe), clock.now());
    model.setGPUModel(gpuModel);
    model.watchProcess(100);

    gpuModel->refresh();
    model.refresh();

    // Re-enumeration finds the probe without per-process utilization (NVML only, say).
    Platform::GPUCapabilities withoutUtilization = caps;
    withoutUtilization.hasPerProcessUtilization = false;
    rawGpuProbe->withCapabilities(withoutUtilization).withRescanReportingChange();
    gpuModel->refresh();
    ASSERT_TRUE(gpuModel->perProcessUtilizationKnownUnsupported());

    clock.advance(std::chrono::milliseconds(100));
    rawProcessProbe->setCounters({makeCounter(100, "watched", 'R', 1100, 0, 5000)});
    model.refresh();

    std::vector<Domain::ProcessSample> samples;
    ASSERT_TRUE(model.watchedSamplesSince(0, samples));
    ASSERT_EQ(samples.size(), 2U);
    EXPECT_TRUE(samples[0].gpuPerProcessSupported);
    EXPECT_TRUE(samples[0].gpuUtilizationSupported);
    EXPECT_TRUE(samples[1].gpuPerProcessSupported);
    EXPECT_FALSE(samples[1].gpuUtilizationSupported);
}

TEST(ProcessModelTest, WatchedSamplesWithoutAGpuModelHaveNoGpuSupport)
{
    auto probe = std::make_unique<MockProcessProbe>();
    probe->setCounters({makeCounter(100, "watched", 'R', 1000, 0, 5000)});
    probe->setTotalCpuTime(100000);
    ManualClock clock;
    Domain::ProcessModel model(std::move(probe), clock.now());
    model.watchProcess(100);
    model.refresh();

    std::vector<Domain::ProcessSample> samples;
    ASSERT_TRUE(model.watchedSamplesSince(0, samples));
    ASSERT_EQ(samples.size(), 1U);
    EXPECT_FALSE(samples[0].gpuPerProcessSupported);
    EXPECT_FALSE(samples[0].gpuUtilizationSupported);
}

TEST(ProcessModelTest, WatchProcessSeedSampleCarriesTheCurrentCounterSupport)
{
    auto probe = std::make_unique<MockProcessProbe>();
    auto* rawProbe = probe.get();
    Platform::ProcessCapabilities caps;
    caps.hasIoCounters = true;
    caps.hasNetworkCounters = false; // e.g. Windows without EStats
    rawProbe->setCapabilities(caps);
    rawProbe->setCounters({makeCounter(100, "watched", 'R', 1000, 0, 5000)});
    rawProbe->setTotalCpuTime(100000);
    ManualClock clock;
    Domain::ProcessModel model(std::move(probe), clock.now());
    model.refresh();
    model.watchProcess(100); // Seeded from the generation already published

    std::vector<Domain::ProcessSample> samples;
    ASSERT_TRUE(model.watchedSamplesSince(0, samples));
    ASSERT_EQ(samples.size(), 1U);
    EXPECT_TRUE(samples[0].ioCountersSupported);
    EXPECT_FALSE(samples[0].networkCountersSupported);
}

TEST(ProcessModelTest, WatchedSamplesSinceCopiesNothingWhenNothingNewWasPublished)
{
    // #1172: the selected process was looked up and deep-copied twice every
    // frame, new data or not. With nothing new published the poll returns false
    // and leaves the output untouched, and a sample handed out twice is the same
    // shared snapshot, not a fresh copy.
    auto probe = std::make_unique<MockProcessProbe>();
    probe->setCounters({makeCounter(100, "watched", 'R', 1000, 0, 5000)});
    probe->setTotalCpuTime(100000);
    Domain::ProcessModel model(std::move(probe));
    model.watchProcess(100);
    model.refresh();

    std::vector<Domain::ProcessSample> first;
    ASSERT_TRUE(model.watchedSamplesSince(0, first));
    ASSERT_EQ(first.size(), 1U);

    std::vector<Domain::ProcessSample> unchanged;
    EXPECT_FALSE(model.watchedSamplesSince(first.back().version, unchanged));
    EXPECT_TRUE(unchanged.empty());

    std::vector<Domain::ProcessSample> again;
    ASSERT_TRUE(model.watchedSamplesSince(0, again));
    ASSERT_EQ(again.size(), 1U);
    EXPECT_EQ(again[0].snapshot.get(), first[0].snapshot.get()) << "shared, not deep-copied per poll";
}

TEST(ProcessModelTest, WatchProcessStartsFromTheCurrentGeneration)
{
    // Selecting a process shows it at once, from the generation already
    // published, rather than waiting for the next refresh.
    auto probe = std::make_unique<MockProcessProbe>();
    probe->setCounters({makeCounter(100, "first", 'R', 1000, 0, 5000), makeCounter(200, "second", 'R', 1000, 0, 6000)});
    probe->setTotalCpuTime(100000);
    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    model.watchProcess(200);
    std::vector<Domain::ProcessSample> samples;
    ASSERT_TRUE(model.watchedSamplesSince(0, samples));
    ASSERT_EQ(samples.size(), 1U);
    EXPECT_EQ(samples[0].version, model.snapshotVersion());
    ASSERT_NE(samples[0].snapshot, nullptr);
    EXPECT_EQ(samples[0].snapshot->pid, 200);

    // Switching the watch drops the old process's samples.
    model.watchProcess(100);
    samples.clear();
    ASSERT_TRUE(model.watchedSamplesSince(0, samples));
    ASSERT_EQ(samples.size(), 1U);
    ASSERT_NE(samples[0].snapshot, nullptr);
    EXPECT_EQ(samples[0].snapshot->pid, 100);
}

TEST(ProcessModelTest, WatchedSampleOfAGenerationWithoutTheProcessIsEmpty)
{
    // A generation that no longer lists the process still gets a sample, with no
    // snapshot, so the reader can tell "exited" from "missed generations".
    auto probe = std::make_unique<MockProcessProbe>();
    auto* rawProbe = probe.get();
    rawProbe->setCounters({makeCounter(100, "watched", 'R', 1000, 0, 5000)});
    rawProbe->setTotalCpuTime(100000);
    Domain::ProcessModel model(std::move(probe));
    model.watchProcess(100);
    model.refresh();
    rawProbe->setCounters({makeCounter(200, "other", 'R', 1000, 0, 6000)});
    model.refresh();

    std::vector<Domain::ProcessSample> samples;
    ASSERT_TRUE(model.watchedSamplesSince(0, samples));
    ASSERT_EQ(samples.size(), 2U);
    EXPECT_NE(samples[0].snapshot, nullptr);
    EXPECT_EQ(samples[1].snapshot, nullptr);
    EXPECT_EQ(samples[1].version, 2U);
}

TEST(ProcessModelTest, WatchedSamplesBeyondTheRingLeaveAVersionGap)
{
    // More generations than the ring holds between two polls: the oldest are
    // gone, and the first sample returned is not the one after the reader's last,
    // so it can mark the gap.
    auto probe = std::make_unique<MockProcessProbe>();
    probe->setCounters({makeCounter(100, "watched", 'R', 1000, 0, 5000)});
    probe->setTotalCpuTime(100000);
    Domain::ProcessModel model(std::move(probe));
    model.watchProcess(100);
    model.refresh();
    std::vector<Domain::ProcessSample> samples;
    ASSERT_TRUE(model.watchedSamplesSince(0, samples));
    const std::uint64_t lastSeen = samples.back().version;

    constexpr std::size_t EXTRA = 3;
    for (std::size_t i = 0; i < Domain::ProcessModel::WATCHED_SAMPLE_CAPACITY + EXTRA; ++i)
    {
        model.refresh();
    }

    samples.clear();
    ASSERT_TRUE(model.watchedSamplesSince(lastSeen, samples));
    ASSERT_EQ(samples.size(), Domain::ProcessModel::WATCHED_SAMPLE_CAPACITY);
    EXPECT_EQ(samples.front().version, lastSeen + 1 + EXTRA);
    EXPECT_EQ(samples.back().version, model.snapshotVersion());
}

TEST(ProcessModelTest, NoSamplesAreKeptWhileNothingIsWatched)
{
    auto probe = std::make_unique<MockProcessProbe>();
    probe->setCounters({makeCounter(100, "watched", 'R', 1000, 0, 5000)});
    probe->setTotalCpuTime(100000);
    Domain::ProcessModel model(std::move(probe));
    model.watchProcess(100);
    model.refresh();
    model.watchProcess(-1);
    model.refresh();

    std::vector<Domain::ProcessSample> samples;
    EXPECT_FALSE(model.watchedSamplesSince(0, samples));
    EXPECT_TRUE(samples.empty());
}

// =============================================================================
// A throwing per-process GPU merge (#1142)
// =============================================================================

TEST(ProcessModelTest, AFailedGpuReadOnASupportedProbeIsAGapNotALackOfSupport)
{
    // #1210: the throw used to publish the generation as unsupported, so the table said "Not
    // available on this system" though only this generation's read failed. It keeps the probe's
    // support and is marked as a failed read instead.
    auto processProbe = std::make_unique<MockProcessProbe>();
    processProbe->setCounters({makeCounter(100, "gpu_process", 'R', 1000, 500)});
    processProbe->setTotalCpuTime(100000);

    auto gpuProbe = std::make_unique<MockGPUProbe>();
    Platform::GPUCapabilities caps;
    caps.hasPerProcessMetrics = true;
    caps.hasPerProcessUtilization = true;
    gpuProbe->withCapabilities(caps);
    gpuProbe->withGPU("GPU0", "Test GPU", "TestVendor").withProcessGPU(100, "GPU0", 512ULL * 1024 * 1024).withProcessCountersThrowing();
    auto gpuModel = std::make_shared<Domain::GPUModel>(std::move(gpuProbe));
    gpuModel->refresh();

    Domain::ProcessModel processModel(std::move(processProbe));
    processModel.setGPUModel(gpuModel);
    processModel.watchProcess(100);
    ASSERT_NO_THROW(processModel.refresh());

    std::vector<Domain::ProcessSample> samples;
    ASSERT_TRUE(processModel.watchedSamplesSince(0, samples));
    ASSERT_EQ(samples.size(), 1U);
    EXPECT_TRUE(samples[0].gpuPerProcessSupported);
    EXPECT_TRUE(samples[0].gpuUtilizationSupported);
    EXPECT_TRUE(samples[0].gpuReadFailed);

    std::shared_ptr<const std::vector<Domain::ProcessSnapshot>> snapshots;
    std::uint64_t version = 0;
    Domain::ProcessModel::GpuSupport support;
    ASSERT_TRUE(processModel.tryCopySnapshotsIfNewer(0, snapshots, version, nullptr, &support));
    EXPECT_TRUE(support.perProcess);
    EXPECT_TRUE(support.readFailed);
}

TEST(ProcessModelTest, ASuccessfulGpuReadIsNotMarkedFailed)
{
    auto processProbe = std::make_unique<MockProcessProbe>();
    processProbe->setCounters({makeCounter(100, "gpu_process", 'R', 1000, 500)});
    processProbe->setTotalCpuTime(100000);
    auto gpuProbe = std::make_unique<MockGPUProbe>();
    Platform::GPUCapabilities caps;
    caps.hasPerProcessMetrics = true;
    gpuProbe->withCapabilities(caps);
    gpuProbe->withGPU("GPU0", "Test GPU", "TestVendor").withProcessGPU(100, "GPU0", 512ULL * 1024 * 1024);
    auto gpuModel = std::make_shared<Domain::GPUModel>(std::move(gpuProbe));
    gpuModel->refresh();

    Domain::ProcessModel processModel(std::move(processProbe));
    processModel.setGPUModel(gpuModel);
    processModel.refresh();

    std::shared_ptr<const std::vector<Domain::ProcessSnapshot>> snapshots;
    std::uint64_t version = 0;
    Domain::ProcessModel::GpuSupport support;
    ASSERT_TRUE(processModel.tryCopySnapshotsIfNewer(0, snapshots, version, nullptr, &support));
    EXPECT_TRUE(support.perProcess);
    EXPECT_FALSE(support.readFailed);
}

TEST(ProcessModelTest, ThrowingPerProcessGpuQueryStillPublishesProcesses)
{
    // #1142: readProcessGPUCounters() throwing escaped refresh() after the
    // per-process state had advanced, so a GPU probe that threw every time froze
    // the process list. The processes are now published without GPU fields.
    auto processProbe = std::make_unique<MockProcessProbe>();
    auto* rawProbe = processProbe.get();
    rawProbe->setCounters({makeCounter(100, "gpu_process", 'R', 1000, 500)});
    rawProbe->setTotalCpuTime(100000);

    auto gpuProbe = std::make_unique<MockGPUProbe>();
    Platform::GPUCapabilities caps;
    caps.hasPerProcessMetrics = true;
    gpuProbe->withCapabilities(caps);
    gpuProbe->withGPU("GPU0", "Test GPU", "TestVendor").withProcessGPU(100, "GPU0", 512ULL * 1024 * 1024).withProcessCountersThrowing();
    auto gpuModel = std::make_shared<Domain::GPUModel>(std::move(gpuProbe));
    gpuModel->refresh();

    Domain::ProcessModel processModel(std::move(processProbe));
    processModel.setGPUModel(gpuModel);

    ASSERT_NO_THROW(processModel.refresh());
    EXPECT_EQ(processModel.snapshotVersion(), 1U);

    rawProbe->setCounters({makeCounter(100, "gpu_process", 'R', 2000, 500), makeCounter(200, "new_process", 'R', 10, 5, 7000)});
    ASSERT_NO_THROW(processModel.refresh());
    EXPECT_EQ(processModel.snapshotVersion(), 2U) << "publication keeps going while the GPU query keeps throwing";

    const auto snaps = processModel.snapshots();
    ASSERT_EQ(snaps.size(), 2U);
    for (const auto& snap : snaps)
    {
        EXPECT_EQ(snap.gpuMemoryBytes, 0U);
        EXPECT_TRUE(snap.gpuDevices.empty());
    }
}

// =============================================================================
// Per-process values the probe could not read (#1110)
// =============================================================================

TEST(ProcessModelTest, UnreadableHandleCountIsUnavailableAndLeftOutOfTheTotal)
{
    // #1110: an unreadable FD/handle count was a 0 that the table showed and the
    // system total counted. It is now flagged unavailable and left out of the
    // total.
    auto probe = std::make_unique<MockProcessProbe>();
    auto readable = makeCounter(100, "mine", 'R', 1000, 0, 5000);
    readable.handleCount = 10;
    auto unreadable = makeCounter(200, "root_owned", 'S', 1000, 0, 6000);
    unreadable.handleCount = 50; // a placeholder that must not be shown or counted
    unreadable.handleCountAvailable = false;
    probe->setCounters({readable, unreadable});
    probe->setTotalCpuTime(100000);
    ManualClock clock;
    Domain::ProcessModel model(std::move(probe), clock.now());
    model.refresh();
    clock.advance(std::chrono::seconds(1));
    model.refresh(); // the aggregate history starts with the second refresh

    const auto snaps = model.snapshots();
    const auto mine = std::ranges::find(snaps, 100, &Domain::ProcessSnapshot::pid);
    const auto theirs = std::ranges::find(snaps, 200, &Domain::ProcessSnapshot::pid);
    ASSERT_NE(mine, snaps.end());
    ASSERT_NE(theirs, snaps.end());
    EXPECT_TRUE(mine->handleCountAvailable);
    EXPECT_EQ(mine->handleCount, 10);
    EXPECT_FALSE(theirs->handleCountAvailable);
    EXPECT_EQ(theirs->handleCount, 0);

    const auto handleTotals = systemHistories(model).handleCount;
    ASSERT_FALSE(handleTotals.empty());
    EXPECT_DOUBLE_EQ(handleTotals.back(), 10.0);
}

TEST(ProcessModelTest, IoRateNeedsBothReadingsToBeAvailable)
{
    // #1110: an I/O rate is taken between two readings. With the earlier one
    // unreadable (a placeholder 0), the next real reading would have counted the
    // process's lifetime of I/O as one interval's; that interval is unavailable
    // instead, and the one after is a real rate.
    auto probe = std::make_unique<MockProcessProbe>();
    auto* rawProbe = probe.get();
    auto counter = makeCounter(100, "proc", 'R', 1000, 0, 5000);
    counter.ioCountersAvailable = false;
    rawProbe->setCounters({counter});
    rawProbe->setTotalCpuTime(100000);
    ManualClock clock;
    Domain::ProcessModel model(std::move(probe), clock.now());
    model.refresh();
    EXPECT_FALSE(model.snapshots().at(0).ioAvailable);

    clock.advance(std::chrono::seconds(1));
    counter.ioCountersAvailable = true;
    counter.readBytes = 1'000'000'000;
    rawProbe->setCounters({counter});
    model.refresh();
    EXPECT_FALSE(model.snapshots().at(0).ioAvailable) << "no earlier reading to take a rate from";
    EXPECT_DOUBLE_EQ(model.snapshots().at(0).ioReadBytesPerSec, 0.0);

    clock.advance(std::chrono::seconds(1));
    counter.readBytes += 4096;
    rawProbe->setCounters({counter});
    model.refresh();
    EXPECT_TRUE(model.snapshots().at(0).ioAvailable);
    EXPECT_DOUBLE_EQ(model.snapshots().at(0).ioReadBytesPerSec, 4096.0);
}

TEST(ProcessModelTest, NetworkCountersTurnedOffDuringTheSampleAreUnavailableInThatSample)
{
    // #1302 review: the Windows probe can find its per-process network counters unusable during the
    // socket-traffic read that follows enumerate() (#1161). The counters enumerate() had returned were
    // already marked available, so that sample published a rate as a reading instead of unavailable.
    auto probe = std::make_unique<MockProcessProbe>();
    auto* rawProbe = probe.get();
    Platform::ProcessCapabilities withNetwork;
    withNetwork.hasNetworkCounters = true;
    rawProbe->setCapabilities(withNetwork);
    auto proc = makeCounter(100, "proc", 'R', 1000, 0, 5000);
    rawProbe->setCounters({proc});
    rawProbe->setTotalCpuTime(100000);
    ManualClock clock;
    Domain::ProcessModel model(std::move(probe), clock.now());
    model.refresh();
    ASSERT_TRUE(model.snapshots().at(0).networkAvailable);

    clock.advance(std::chrono::seconds(1));
    proc.netSentBytes = 2000;
    rawProbe->setCounters({proc});
    Platform::ProcessCapabilities revoked = withNetwork;
    revoked.hasNetworkCounters = false;
    rawProbe->switchCapabilitiesOnNextSocketRead(revoked);
    model.refresh();
    EXPECT_FALSE(model.snapshots().at(0).networkAvailable);
    EXPECT_DOUBLE_EQ(model.snapshots().at(0).netSentBytesPerSec, 0.0);
}

TEST(ProcessModelTest, UnattributableNetworkCountersAreUnavailable)
{
    // #1110: a process whose connections can't be attributed to it (another
    // user's, without root, on Linux) showed 0 B/s. Its network rates are now
    // unavailable.
    auto probe = std::make_unique<MockProcessProbe>();
    auto* rawProbe = probe.get();
    auto mine = makeCounter(100, "mine", 'R', 1000, 0, 5000);
    auto theirs = makeCounter(200, "theirs", 'R', 1000, 0, 6000);
    theirs.networkCountersAvailable = false;
    rawProbe->setCounters({mine, theirs});
    rawProbe->setTotalCpuTime(100000);
    ManualClock clock;
    Domain::ProcessModel model(std::move(probe), clock.now());
    model.refresh();

    clock.advance(std::chrono::seconds(1));
    mine.netSentBytes = 2000;
    theirs.netSentBytes = 1'000'000; // a placeholder that must not be counted
    rawProbe->setCounters({mine, theirs});
    model.refresh();

    const auto snaps = model.snapshots();
    const auto theirsSnap = std::ranges::find(snaps, 200, &Domain::ProcessSnapshot::pid);
    ASSERT_NE(theirsSnap, snaps.end());
    EXPECT_FALSE(theirsSnap->networkAvailable);
    EXPECT_DOUBLE_EQ(theirsSnap->netSentBytesPerSec, 0.0);
    const auto mineSnap = std::ranges::find(snaps, 100, &Domain::ProcessSnapshot::pid);
    ASSERT_NE(mineSnap, snaps.end());
    EXPECT_TRUE(mineSnap->networkAvailable);
    EXPECT_DOUBLE_EQ(mineSnap->netSentBytesPerSec, 2000.0);
}

// #1885: PSS and USS cost the kernel a walk of every mapping, so ProcessModel reads them for its
// watched process only (the one Process Details shows), into that process's watched sample.
TEST(ProcessModelTest, OnlyTheWatchedProcessHasItsPssAndUssRead)
{
    auto probe = std::make_unique<MockProcessProbe>();
    probe->setCounters({makeCounter(100, "watched", 'R', 1000, 500), makeCounter(200, "other", 'S', 100, 50)});
    probe->setTotalCpuTime(100000);
    probe->setMemoryRollup(100, Platform::MemoryRollup{.pssBytes = 6000, .ussBytes = 4000, .swapPssBytes = 50});
    probe->setMemoryRollup(200, Platform::MemoryRollup{.pssBytes = 9000, .ussBytes = 8000, .swapPssBytes = 0});
    MockProcessProbe* rawProbe = probe.get();
    Domain::ProcessModel model(std::move(probe));

    model.refresh(); // Nothing watched: nothing read
    EXPECT_EQ(rawProbe->memoryRollupReads(), 0);

    model.watchProcess(100);
    model.refresh();
    EXPECT_EQ(rawProbe->memoryRollupReads(), 1); // The watched process alone
    // Asked for the process the snapshot is of, by its start time, so a reused PID reads nothing.
    const auto rows = model.snapshots();
    const auto watchedRow = std::ranges::find(rows, 100, &Domain::ProcessSnapshot::pid);
    ASSERT_NE(watchedRow, rows.end());
    EXPECT_EQ(rawProbe->lastMemoryRollupStartTime(), watchedRow->startTimeTicks);
    std::vector<Domain::ProcessSample> samples;
    ASSERT_TRUE(model.watchedSamplesSince(0, samples));
    ASSERT_FALSE(samples.empty());
    ASSERT_NE(samples.back().snapshot, nullptr);
    const Domain::ProcessSnapshot& watched = *samples.back().snapshot;
    EXPECT_TRUE(watched.memoryRollupRead);
    EXPECT_EQ(watched.pssBytes, 6000U);
    EXPECT_EQ(watched.ussBytes, 4000U);
    EXPECT_EQ(watched.swapPssBytes, 50U);

    // The table's rows don't carry them, the watched process's included.
    const auto snaps = model.snapshots();
    for (const Domain::ProcessSnapshot& snap : snaps)
    {
        EXPECT_FALSE(snap.memoryRollupRead) << snap.pid;
        EXPECT_EQ(snap.pssBytes, 0U) << snap.pid;
    }
}

TEST(ProcessModelTest, WithoutMemoryRollupSupportNothingIsRead)
{
    auto probe = std::make_unique<MockProcessProbe>();
    probe->setCounters({makeCounter(100, "watched", 'R', 1000, 500)});
    probe->setTotalCpuTime(100000);
    MockProcessProbe* rawProbe = probe.get(); // Capabilities: no hasMemoryRollup
    Domain::ProcessModel model(std::move(probe));
    model.watchProcess(100);
    model.refresh();
    EXPECT_EQ(rawProbe->memoryRollupReads(), 0);
    std::vector<Domain::ProcessSample> samples;
    ASSERT_TRUE(model.watchedSamplesSince(0, samples));
    ASSERT_NE(samples.back().snapshot, nullptr);
    EXPECT_FALSE(samples.back().snapshot->memoryRollupRead);
}

// #2309: a refused read (another user's process) is said, so Process Details shows "not readable"
// rather than leaving the figures out.
TEST(ProcessModelTest, ARefusedMemoryRollupIsMarkedNotReadable)
{
    auto probe = std::make_unique<MockProcessProbe>();
    probe->setCounters({makeCounter(100, "watched", 'R', 1000, 500)});
    probe->setTotalCpuTime(100000);
    probe->setMemoryRollupDenied(100);
    Domain::ProcessModel model(std::move(probe));
    model.watchProcess(100);
    model.refresh();
    std::vector<Domain::ProcessSample> samples;
    ASSERT_TRUE(model.watchedSamplesSince(0, samples));
    ASSERT_FALSE(samples.empty());
    ASSERT_NE(samples.back().snapshot, nullptr);
    EXPECT_FALSE(samples.back().snapshot->memoryRollupRead);
    EXPECT_TRUE(samples.back().snapshot->memoryRollupDenied);
}

// #3031: a process this refresh couldn't read (Linux's hidepid-hidden processes while the helper is busy)
// arrives carried over. It stays, with its last row, and its next real read is measured over the whole
// time since the last one: no gap, no history reset, no double rate.
TEST(ProcessModelTest, AHeldOverProcessKeepsItsRowAndItsNextReadSpansTheWholeInterval)
{
    Domain::ProcessModel::Clock::time_point now;
    Domain::ProcessModel model(nullptr, [&now] { return now; });
    const auto find = [&model](std::int32_t pid)
    {
        const auto snapshots = model.snapshots();
        const auto it = std::ranges::find(snapshots, pid, &Domain::ProcessSnapshot::pid);
        return it == snapshots.end() ? std::optional<Domain::ProcessSnapshot>{} : std::optional<Domain::ProcessSnapshot>{*it};
    };

    Platform::ProcessCounters visible = makeCounter(100, "visible", 'S', 0, 0);
    Platform::ProcessCounters hidden = makeCounter(200, "hidden", 'S', 0, 0);
    std::uint64_t total = 1000;
    const auto sample = [&](const Platform::ProcessCounters& second)
    {
        now += std::chrono::seconds(1);
        total += 1000;
        visible.userTime += 100;
        model.updateFromCounters({visible, second}, total);
    };

    now += std::chrono::seconds(1);
    model.updateFromCounters({visible, hidden}, total);
    hidden.userTime = 100;
    hidden.readBytes = 1000;
    sample(hidden);
    ASSERT_TRUE(find(200).has_value());
    EXPECT_DOUBLE_EQ(find(200)->cpuPercent, 10.0);
    EXPECT_DOUBLE_EQ(find(200)->ioReadBytesPerSec, 1000.0);

    Platform::ProcessCounters carried = hidden;
    carried.heldOver = true;
    carried.userTime = 0; // whatever a carried record says, the row is the last one
    sample(carried);
    const auto held = find(200);
    ASSERT_TRUE(held.has_value()) << "a carried-over process stays";
    EXPECT_DOUBLE_EQ(held->cpuPercent, 10.0) << "its last row again";
    EXPECT_DOUBLE_EQ(held->ioReadBytesPerSec, 1000.0);
    EXPECT_EQ(held->name, "hidden");
    EXPECT_DOUBLE_EQ(find(100)->cpuPercent, 10.0) << "the others are measured as ever";

    hidden.userTime = 300; // 200 ticks over the two intervals since its last read
    hidden.readBytes = 3000;
    sample(hidden);
    const auto read = find(200);
    ASSERT_TRUE(read.has_value());
    EXPECT_DOUBLE_EQ(read->cpuPercent, 10.0) << "over the 2000 ticks since its last read, not the last 1000";
    EXPECT_DOUBLE_EQ(read->ioReadBytesPerSec, 1000.0) << "over the two seconds since";
    EXPECT_DOUBLE_EQ(find(100)->cpuPercent, 10.0);
}

// #3031: a carried-over record for a process with no row yet is taken as an ordinary first read.
TEST(ProcessModelTest, AHeldOverProcessWithNoRowYetIsListedAsNew)
{
    Domain::ProcessModel::Clock::time_point now;
    Domain::ProcessModel model(nullptr, [&now] { return now; });
    Platform::ProcessCounters carried = makeCounter(200, "hidden", 'S', 100, 0);
    carried.heldOver = true;
    now += std::chrono::seconds(1);
    model.updateFromCounters({carried}, 1000);
    const auto snapshots = model.snapshots();
    ASSERT_EQ(snapshots.size(), 1U);
    EXPECT_EQ(snapshots[0].pid, 200);
    EXPECT_DOUBLE_EQ(snapshots[0].cpuPercent, 0.0);
}
