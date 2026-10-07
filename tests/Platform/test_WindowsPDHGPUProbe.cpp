#ifdef _WIN32

#include "Platform/GPUTypes.h"
#include "Platform/Windows/PDHGPUProbe.h"
#include "Platform/Windows/PDHGPUProbeImpl.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Platform
{
namespace
{

TEST(WindowsPDHGPUProbeTest, ConstructionDoesNotThrow)
{
    EXPECT_NO_THROW(PDHGPUProbe probe);
}

TEST(WindowsPDHGPUProbeTest, BasicOperationsDoNotThrow)
{
    PDHGPUProbe probe;
    EXPECT_NO_THROW([[maybe_unused]] auto available = probe.isAvailable());
    EXPECT_NO_THROW([[maybe_unused]] auto process = probe.readProcessGPUCounters());
    EXPECT_NO_THROW([[maybe_unused]] auto caps = probe.capabilities());
}

TEST(WindowsPDHGPUProbeTest, IsAvailableReturnsBool)
{
    PDHGPUProbe probe;
    const bool available1 = probe.isAvailable();
    const bool available2 = probe.isAvailable();
    EXPECT_EQ(available1, available2) << "isAvailable() should be stable across consecutive calls";
}

TEST(WindowsPDHGPUProbeTest, CapabilitiesRelationshipsAreConsistent)
{
    PDHGPUProbe probe;
    const auto caps = probe.capabilities();

    EXPECT_EQ(caps.hasPerProcessMetrics, caps.hasEngineUtilization); // Process role (the default)
    EXPECT_FALSE(caps.hasTemperature);
    EXPECT_FALSE(caps.hasPowerMetrics);
    EXPECT_FALSE(caps.hasClockSpeeds);
    EXPECT_FALSE(caps.hasFanSpeed);
}

// An Adapter-role probe returns no per-process counters, so it must not advertise them; engine
// utilization (its adapter totals) it does report (#1365 review).
TEST(WindowsPDHGPUProbeTest, AdapterRoleDoesNotAdvertisePerProcessCounters)
{
    const PDHGPUProbe probe(PDHGPUProbe::Role::Adapter);
    const auto caps = probe.capabilities();
    EXPECT_FALSE(caps.hasPerProcessMetrics);
    EXPECT_FALSE(caps.hasPerProcessUtilization);
    if (probe.isAvailable())
    {
        EXPECT_TRUE(caps.hasEngineUtilization);
    }
}

TEST(WindowsPDHGPUProbeTest, MoveConstructionAndAssignmentTransferState)
{
    PDHGPUProbe probe;
    const bool availableBeforeMove = probe.isAvailable();

    PDHGPUProbe moved(std::move(probe));
    EXPECT_EQ(moved.isAvailable(), availableBeforeMove);
    EXPECT_NO_THROW([[maybe_unused]] auto process = moved.readProcessGPUCounters());

    PDHGPUProbe assigned;
    assigned = std::move(moved);
    EXPECT_EQ(assigned.isAvailable(), availableBeforeMove);
    EXPECT_NO_THROW([[maybe_unused]] auto process = assigned.readProcessGPUCounters());
}

// =============================================================================
// parseInstanceName: pure parsing, no probe/fake harness required. Covers malformed-shape
// branches not otherwise reachable via readProcessGPUCounters()'s malformed-name test, which
// only exercises names that already have a "_phys_" or "_engtype_" segment.
// =============================================================================

TEST(ParseInstanceNameTest, NoUnderscoreAfterPidIsInvalid)
{
    // "pid_" prefix present but nothing delimits the PID from what follows.
    EXPECT_FALSE(PDHGPUProbeImplDetail::parseInstanceName("pid_123").valid);
}

TEST(ParseInstanceNameTest, NoPhysOrEngtypeSuffixIsInvalid)
{
    // Valid pid_/luid_ prefix, but the instance name ends there with no _phys_ or _engtype_.
    EXPECT_FALSE(PDHGPUProbeImplDetail::parseInstanceName("pid_700_luid_0x0_0x1").valid);
}

TEST(ParseInstanceNameTest, EmptyLuidSegmentIsInvalid)
{
    EXPECT_FALSE(PDHGPUProbeImplDetail::parseInstanceName("pid_701_luid__phys_0_eng_0_engtype_3D").valid);
}

TEST(ParseInstanceNameTest, LuidPartsMissingHexPrefixAreInvalid)
{
    EXPECT_FALSE(PDHGPUProbeImplDetail::parseInstanceName("pid_702_luid_ABC_DEF_phys_0_eng_0_engtype_3D").valid);
}

TEST(ParseInstanceNameTest, PhysWithoutEngSuffixIsInvalid)
{
    // "_phys_0" followed by garbage instead of "_eng_".
    EXPECT_FALSE(PDHGPUProbeImplDetail::parseInstanceName("pid_703_luid_0x0_0x1_phys_0_garbage").valid);
}

TEST(ParseInstanceNameTest, EngWithNonDigitSuffixIsInvalid)
{
    EXPECT_FALSE(PDHGPUProbeImplDetail::parseInstanceName("pid_704_luid_0x0_0x1_phys_0_eng_x_engtype_3D").valid);
}

TEST(ParseInstanceNameTest, EngWithoutEngtypeSuffixIsInvalid)
{
    EXPECT_FALSE(PDHGPUProbeImplDetail::parseInstanceName("pid_705_luid_0x0_0x1_phys_0_eng_0_garbage").valid);
}

} // namespace
} // namespace Platform

// =============================================================================
// Injectable PDH fakes (testability)
// FakeItem/FakeScenario are outside the anonymous namespace so std::make_unique works.
// =============================================================================

struct FakeItem
{
    std::wstring name;
    DWORD cstatus = ERROR_SUCCESS;
    double doubleValue = 0.0;
    LONGLONG largeValue = 0;
};

struct FakeScenario
{
    std::unordered_map<std::wstring, bool> failToAdd;
    int addCounterCallCount = 0;
    std::vector<std::wstring> addedPaths; // Every counter path the query was asked to add (#1175)
    PDH_STATUS collectStatus = ERROR_SUCCESS;
    int collectCallCount = 0;
    std::unordered_map<PDH_HCOUNTER, std::vector<FakeItem>> items;
    std::unordered_map<PDH_HCOUNTER, int> extraMoreDataRounds;
    std::unordered_map<PDH_HCOUNTER, int> moreDataCallIndex;
    std::unordered_map<PDH_HCOUNTER, std::vector<DWORD>> moreDataRequiredSizes;
    std::unordered_map<PDH_HCOUNTER, std::vector<DWORD>> seenBufferSizes;
    std::unordered_map<PDH_HCOUNTER, std::vector<void*>> seenBufferPointers;
    int getArrayCallCount = 0;
    std::uintptr_t nextHandleValue = 1;
    /// When set for a counter, fakeGetFormattedCounterArray returns this status immediately
    /// (neither ERROR_SUCCESS nor PDH_MORE_DATA), simulating a hard PDH failure.
    std::unordered_map<PDH_HCOUNTER, PDH_STATUS> hardFailureStatus;
};

namespace Platform
{
namespace
{

using Impl = PDHGPUProbe::Impl;

thread_local FakeScenario* g_scenario = nullptr;

PDH_STATUS WINAPI fakeOpenQuery(LPCWSTR, DWORD_PTR, PDH_HQUERY* query)
{
    *query = reinterpret_cast<PDH_HQUERY>(static_cast<std::uintptr_t>(1));
    return ERROR_SUCCESS;
}

PDH_STATUS WINAPI fakeCloseQuery(PDH_HQUERY)
{
    return ERROR_SUCCESS;
}

PDH_STATUS WINAPI fakeAddEnglishCounter(PDH_HQUERY, LPCWSTR path, DWORD_PTR, PDH_HCOUNTER* counter)
{
    ++g_scenario->addCounterCallCount;
    g_scenario->addedPaths.emplace_back(path);
    if (const auto it = g_scenario->failToAdd.find(path); it != g_scenario->failToAdd.end() && it->second)
    {
        *counter = nullptr;
        return static_cast<PDH_STATUS>(PDH_CSTATUS_NO_COUNTER);
    }
    *counter = reinterpret_cast<PDH_HCOUNTER>(g_scenario->nextHandleValue++);
    return ERROR_SUCCESS;
}

PDH_STATUS WINAPI fakeCollectQueryData(PDH_HQUERY)
{
    ++g_scenario->collectCallCount;
    return g_scenario->collectStatus;
}

PDH_STATUS WINAPI
fakeGetFormattedCounterArray(PDH_HCOUNTER counter, DWORD format, LPDWORD bufferSize, LPDWORD itemCount, PPDH_FMT_COUNTERVALUE_ITEM_W buffer)
{
    ++g_scenario->getArrayCallCount;
    if (const auto failIt = g_scenario->hardFailureStatus.find(counter); failIt != g_scenario->hardFailureStatus.end())
    {
        *itemCount = 0;
        return failIt->second;
    }
    static const std::vector<FakeItem> emptyList;
    const auto listIt = g_scenario->items.find(counter);
    const std::vector<FakeItem>& list = (listIt != g_scenario->items.end()) ? listIt->second : emptyList;

    if (list.empty())
    {
        g_scenario->seenBufferSizes[counter].push_back(*bufferSize);
        g_scenario->seenBufferPointers[counter].push_back(buffer);
        *itemCount = 0;
        return ERROR_SUCCESS;
    }

    if (auto more = g_scenario->extraMoreDataRounds.find(counter); more != g_scenario->extraMoreDataRounds.end() && more->second > 0)
    {
        --more->second;
        // Each forced retry advertises a larger requirement than the last, simulating
        // wildcard instances appearing between PdhCollectQueryData calls. A caller that
        // reused an earlier (smaller) reported size instead of the latest one would
        // under-allocate and fail on the eventual real read below.
        const int callIndex = ++g_scenario->moreDataCallIndex[counter];
        *bufferSize = static_cast<DWORD>(sizeof(PDH_FMT_COUNTERVALUE_ITEM_W) * (list.size() + static_cast<std::size_t>(callIndex)));
        g_scenario->moreDataRequiredSizes[counter].push_back(*bufferSize);
        return static_cast<PDH_STATUS>(PDH_MORE_DATA);
    }

    const auto needed = static_cast<DWORD>(sizeof(PDH_FMT_COUNTERVALUE_ITEM_W) * list.size());
    if (*bufferSize < needed || buffer == nullptr)
    {
        g_scenario->seenBufferSizes[counter].push_back(*bufferSize);
        g_scenario->seenBufferPointers[counter].push_back(buffer);
        *bufferSize = needed;
        return static_cast<PDH_STATUS>(PDH_MORE_DATA);
    }
    g_scenario->seenBufferSizes[counter].push_back(*bufferSize);
    g_scenario->seenBufferPointers[counter].push_back(buffer);

    *itemCount = static_cast<DWORD>(list.size());
    for (std::size_t i = 0; i < list.size(); ++i)
    {
        buffer[i].szName = const_cast<LPWSTR>(list[i].name.c_str());
        buffer[i].FmtValue.CStatus = list[i].cstatus;
        if ((format & PDH_FMT_LARGE) != 0)
        {
            buffer[i].FmtValue.largeValue = list[i].largeValue;
        }
        else
        {
            buffer[i].FmtValue.doubleValue = list[i].doubleValue;
        }
    }
    return ERROR_SUCCESS;
}

std::unique_ptr<Impl> makeInjectedImpl(PDHGPUProbe::Role role = PDHGPUProbe::Role::Process)
{
    auto impl = std::make_unique<Impl>();
    impl->role = role;
    impl->pdhOpenQuery = fakeOpenQuery;
    impl->pdhCloseQuery = fakeCloseQuery;
    impl->pdhAddEnglishCounter = fakeAddEnglishCounter;
    impl->pdhCollectQueryData = fakeCollectQueryData;
    impl->pdhGetFormattedCounterArray = fakeGetFormattedCounterArray;
    impl->query = reinterpret_cast<PDH_HQUERY>(static_cast<std::uintptr_t>(1));
    impl->initialized = true;
    impl->ensureCounters();
    impl->warmedUp = true;
    return impl;
}

class WindowsPDHGPUProbeInjectedTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_scenario = std::make_unique<FakeScenario>();
        g_scenario = m_scenario.get();
    }

    void TearDown() override
    {
        g_scenario = nullptr;
        m_scenario.reset();
    }

    std::unique_ptr<FakeScenario> m_scenario;
};

TEST_F(WindowsPDHGPUProbeInjectedTest, UninitializedImplReturnsEmptyWithoutTouchingFakes)
{
    // Impl::initialized stays false (default): readProcessGPUCounters() must bail out before
    // calling any PDH function at all.
    auto impl = std::make_unique<Impl>();
    impl->pdhOpenQuery = fakeOpenQuery;
    impl->pdhCloseQuery = fakeCloseQuery;
    impl->pdhAddEnglishCounter = fakeAddEnglishCounter;
    impl->pdhCollectQueryData = fakeCollectQueryData;
    impl->pdhGetFormattedCounterArray = fakeGetFormattedCounterArray;

    PDHGPUProbe probe(std::move(impl));
    EXPECT_FALSE(probe.isAvailable());
    EXPECT_TRUE(probe.readProcessGPUCounters().empty());
    EXPECT_EQ(m_scenario->collectCallCount, 0) << "must bail out before collecting any data";
}

TEST_F(WindowsPDHGPUProbeInjectedTest, AllCountersFailingToAddReturnsEmptyWithNoCache)
{
    auto impl = std::make_unique<Impl>();
    impl->pdhOpenQuery = fakeOpenQuery;
    impl->pdhCloseQuery = fakeCloseQuery;
    impl->pdhAddEnglishCounter = fakeAddEnglishCounter;
    impl->pdhCollectQueryData = fakeCollectQueryData;
    impl->pdhGetFormattedCounterArray = fakeGetFormattedCounterArray;
    impl->query = reinterpret_cast<PDH_HQUERY>(static_cast<std::uintptr_t>(1));
    impl->initialized = true;
    // All three wildcard counters fail to add, so ensureCounters() returns false and
    // readProcessGPUCounters() must return early with no cached results yet (fresh probe).
    m_scenario->failToAdd[Impl::UTILIZATION_COUNTER_PATH] = true;
    m_scenario->failToAdd[Impl::DEDICATED_MEMORY_COUNTER_PATH] = true;
    m_scenario->failToAdd[Impl::SHARED_MEMORY_COUNTER_PATH] = true;
    m_scenario->failToAdd[Impl::ADAPTER_DEDICATED_COUNTER_PATH] = true;
    m_scenario->failToAdd[Impl::ADAPTER_SHARED_COUNTER_PATH] = true;

    PDHGPUProbe probe(std::move(impl));
    EXPECT_TRUE(probe.readProcessGPUCounters().empty());
    EXPECT_EQ(m_scenario->collectCallCount, 0) << "must bail out before ever collecting";
}

TEST_F(WindowsPDHGPUProbeInjectedTest, AllCountersFailingToAddReturnsStaleCacheWhenPresent)
{
    auto impl = std::make_unique<Impl>();
    impl->pdhOpenQuery = fakeOpenQuery;
    impl->pdhCloseQuery = fakeCloseQuery;
    impl->pdhAddEnglishCounter = fakeAddEnglishCounter;
    impl->pdhCollectQueryData = fakeCollectQueryData;
    impl->pdhGetFormattedCounterArray = fakeGetFormattedCounterArray;
    impl->query = reinterpret_cast<PDH_HQUERY>(static_cast<std::uintptr_t>(1));
    impl->initialized = true;
    // Pre-populate the cache directly, as if a prior successful read had happened, then make
    // all counters fail to add so ensureCounters() fails on this call -- exercises the "has a
    // stale cached result to log/return" branch, not just the empty-cache one above.
    ProcessGPUCounters cached;
    cached.pid = 900;
    cached.gpuUtilPercent = 50.0;
    impl->lastValidResults = {cached};
    impl->lastValidTimestamp = std::chrono::steady_clock::now();
    m_scenario->failToAdd[Impl::UTILIZATION_COUNTER_PATH] = true;
    m_scenario->failToAdd[Impl::DEDICATED_MEMORY_COUNTER_PATH] = true;
    m_scenario->failToAdd[Impl::SHARED_MEMORY_COUNTER_PATH] = true;
    m_scenario->failToAdd[Impl::ADAPTER_DEDICATED_COUNTER_PATH] = true;
    m_scenario->failToAdd[Impl::ADAPTER_SHARED_COUNTER_PATH] = true;

    PDHGPUProbe probe(std::move(impl));
    const auto results = probe.readProcessGPUCounters();
    ASSERT_EQ(results.size(), 1U);
    EXPECT_EQ(results[0].pid, 900);
}

// =============================================================================
// Roles: each query adds and reads only the counters it uses (#1175)
// =============================================================================

TEST_F(WindowsPDHGPUProbeInjectedTest, ProcessRoleAddsEngineAndProcessMemoryCountersOnly)
{
    const auto impl = makeInjectedImpl(PDHGPUProbe::Role::Process);

    const std::vector<std::wstring> expected = {
        Impl::UTILIZATION_COUNTER_PATH,
        Impl::DEDICATED_MEMORY_COUNTER_PATH,
        Impl::SHARED_MEMORY_COUNTER_PATH,
    };
    EXPECT_EQ(m_scenario->addedPaths, expected);
    EXPECT_EQ(impl->adapterDedicatedCounter, nullptr);
    EXPECT_EQ(impl->adapterSharedCounter, nullptr);
}

TEST_F(WindowsPDHGPUProbeInjectedTest, AdapterRoleAddsEngineAndAdapterMemoryCountersOnly)
{
    // GPU Engine stays in both queries: its rates are per query, and the two samplers collect at
    // different intervals.
    const auto impl = makeInjectedImpl(PDHGPUProbe::Role::Adapter);

    const std::vector<std::wstring> expected = {
        Impl::UTILIZATION_COUNTER_PATH,
        Impl::ADAPTER_DEDICATED_COUNTER_PATH,
        Impl::ADAPTER_SHARED_COUNTER_PATH,
    };
    EXPECT_EQ(m_scenario->addedPaths, expected);
    EXPECT_EQ(impl->dedicatedMemoryCounter, nullptr);
    EXPECT_EQ(impl->sharedMemoryCounter, nullptr);
}

TEST_F(WindowsPDHGPUProbeInjectedTest, AFailedCounterIsRetriedOnlyForItsOwnRole)
{
    // A counter that failed to add is retried by its own query, and the retry adds nothing of the
    // other role's.
    m_scenario->failToAdd[Impl::ADAPTER_SHARED_COUNTER_PATH] = true;
    const auto impl = makeInjectedImpl(PDHGPUProbe::Role::Adapter);
    ASSERT_EQ(impl->adapterSharedCounter, nullptr);
    m_scenario->addedPaths.clear();
    m_scenario->failToAdd[Impl::ADAPTER_SHARED_COUNTER_PATH] = false;

    ASSERT_TRUE(impl->ensureCounters());

    EXPECT_EQ(m_scenario->addedPaths, (std::vector<std::wstring>{Impl::ADAPTER_SHARED_COUNTER_PATH}));
}

TEST_F(WindowsPDHGPUProbeInjectedTest, AdapterRoleReadsAdapterFiguresWithoutPerProcessResults)
{
    auto impl = makeInjectedImpl(PDHGPUProbe::Role::Adapter);
    const PDH_HCOUNTER utilization = impl->utilizationCounter;
    m_scenario->items[utilization] = {
        {.name = L"pid_1400_luid_0x0_0x1_phys_0_eng_0_engtype_3D", .doubleValue = 25.0},
    };
    m_scenario->items[impl->adapterDedicatedCounter] = {{.name = L"luid_0x0_0x1_phys_0", .largeValue = 4096}};
    PDHGPUProbe probe(std::move(impl));

    EXPECT_TRUE(probe.readProcessGPUCounters().empty()) << "the adapter query builds no per-process results";
    EXPECT_DOUBLE_EQ(probe.adapterUtilization().at("GPU_0x0_0x1"), 25.0);
    EXPECT_EQ(probe.adapterMemory().at("GPU_0x0_0x1").dedicatedBytes, 4096ULL);
}

TEST_F(WindowsPDHGPUProbeInjectedTest, ProcessRoleReadsNoAdapterFigures)
{
    auto impl = makeInjectedImpl(PDHGPUProbe::Role::Process);
    m_scenario->items[impl->utilizationCounter] = {
        {.name = L"pid_1410_luid_0x0_0x1_phys_0_eng_0_engtype_3D", .doubleValue = 25.0},
    };
    PDHGPUProbe probe(std::move(impl));

    const auto results = probe.readProcessGPUCounters();

    ASSERT_EQ(results.size(), 1U);
    EXPECT_DOUBLE_EQ(results[0].gpuUtilPercent, 25.0);
    EXPECT_TRUE(probe.adapterUtilization().empty());
    EXPECT_TRUE(probe.adapterMemory().empty());
}

TEST_F(WindowsPDHGPUProbeInjectedTest, CollectQueryDataFailureReturnsCachedResults)
{
    auto impl = makeInjectedImpl();
    const PDH_HCOUNTER utilizationCounter = impl->utilizationCounter;
    m_scenario->items[utilizationCounter] = {
        {.name = L"pid_800_luid_0x0_0x1_phys_0_eng_0_engtype_3D", .doubleValue = 44.0},
    };

    PDHGPUProbe probe(std::move(impl));
    const auto warmResults = probe.readProcessGPUCounters();
    ASSERT_EQ(warmResults.size(), 1U);
    EXPECT_EQ(warmResults[0].pid, 800);

    // Now force PdhCollectQueryData to fail; the probe must fall back to the cached result
    // from the successful read above rather than an empty list.
    m_scenario->collectStatus = static_cast<PDH_STATUS>(PDH_CSTATUS_NO_INSTANCE);
    const auto failedResults = probe.readProcessGPUCounters();
    ASSERT_EQ(failedResults.size(), 1U);
    EXPECT_EQ(failedResults[0].pid, 800);
}

// =============================================================================
// Engine aggregation (#1033) and staleness (#1034)
// =============================================================================

TEST(ParseInstanceNameTest, EngineKeyIdentifiesTheEngineInEachFormat)
{
    EXPECT_EQ(PDHGPUProbeImplDetail::parseInstanceName("pid_1_luid_0x0_0x1_phys_0_eng_3_engtype_3D").engineKey, "phys_0_eng_3");
    EXPECT_EQ(PDHGPUProbeImplDetail::parseInstanceName("pid_1_luid_0x0_0x1_phys_1_eng_12_engtype_").engineKey, "phys_1_eng_12");
    EXPECT_EQ(PDHGPUProbeImplDetail::parseInstanceName("pid_1_luid_0x0_0x1_engtype_Copy").engineKey, "engtype_Copy");
    // Memory instances measure no engine.
    EXPECT_EQ(PDHGPUProbeImplDetail::parseInstanceName("pid_1_luid_0x0_0x1_phys_0").engineKey, "");
}

TEST_F(WindowsPDHGPUProbeInjectedTest, ProcessUtilizationIsItsBusiestEngineNotTheSum)
{
    // A video call: decode 40 %, 3D 20 %, copy 10 %. Engines run in parallel, so the process is
    // 40 % busy (Task Manager's figure), not 70 %.
    auto impl = makeInjectedImpl();
    m_scenario->items[impl->utilizationCounter] = {
        {.name = L"pid_1000_luid_0x0_0x1_phys_0_eng_0_engtype_3D", .doubleValue = 20.0},
        {.name = L"pid_1000_luid_0x0_0x1_phys_0_eng_1_engtype_VideoDecode", .doubleValue = 40.0},
        {.name = L"pid_1000_luid_0x0_0x1_phys_0_eng_2_engtype_Copy", .doubleValue = 10.0},
    };
    PDHGPUProbe probe(std::move(impl));

    const auto results = probe.readProcessGPUCounters();
    ASSERT_EQ(results.size(), 1U);
    EXPECT_DOUBLE_EQ(results[0].gpuUtilPercent, 40.0);
    EXPECT_EQ(results[0].activeEngines.size(), 3U);
}

TEST_F(WindowsPDHGPUProbeInjectedTest, AdapterUtilizationSumsProcessesPerEngineThenTakesTheBusiest)
{
    // Two processes share the 3D engine (30 + 30 = 60 %); a third decodes at 50 %. The adapter is
    // 60 % busy, not 110 % clamped to 100.
    auto impl = makeInjectedImpl(PDHGPUProbe::Role::Adapter);
    m_scenario->items[impl->utilizationCounter] = {
        {.name = L"pid_1100_luid_0x0_0x1_phys_0_eng_0_engtype_3D", .doubleValue = 30.0},
        {.name = L"pid_1101_luid_0x0_0x1_phys_0_eng_0_engtype_3D", .doubleValue = 30.0},
        {.name = L"pid_1102_luid_0x0_0x1_phys_0_eng_1_engtype_VideoDecode", .doubleValue = 50.0},
        {.name = L"pid_1103_luid_0x0_0x2_phys_0_eng_0_engtype_3D", .doubleValue = 5.0},
    };
    PDHGPUProbe probe(std::move(impl));
    static_cast<void>(probe.readProcessGPUCounters());

    const auto byAdapter = probe.adapterUtilization();
    ASSERT_EQ(byAdapter.size(), 2U);
    EXPECT_DOUBLE_EQ(byAdapter.at("GPU_0x0_0x1"), 60.0);
    EXPECT_DOUBLE_EQ(byAdapter.at("GPU_0x0_0x2"), 5.0);
}

TEST_F(WindowsPDHGPUProbeInjectedTest, AdapterUtilizationIsCappedAtOneHundred)
{
    // Per-process readings of one engine are sampled independently and can add up past 100.
    auto impl = makeInjectedImpl(PDHGPUProbe::Role::Adapter);
    m_scenario->items[impl->utilizationCounter] = {
        {.name = L"pid_1200_luid_0x0_0x1_phys_0_eng_0_engtype_3D", .doubleValue = 70.0},
        {.name = L"pid_1201_luid_0x0_0x1_phys_0_eng_0_engtype_3D", .doubleValue = 45.0},
    };
    PDHGPUProbe probe(std::move(impl));
    static_cast<void>(probe.readProcessGPUCounters());

    EXPECT_DOUBLE_EQ(probe.adapterUtilization().at("GPU_0x0_0x1"), 100.0);
}

TEST_F(WindowsPDHGPUProbeInjectedTest, StaleCacheIsNotRepeatedAfterACollectFailure)
{
    // A failed collect may fall back to a recent result, but not to one older than
    // MAX_STALE_RESULTS_AGE: that repeated an old busy reading as a frozen flat line (#1034).
    auto impl = makeInjectedImpl();
    ProcessGPUCounters cached;
    cached.pid = 1300;
    cached.gpuUtilPercent = 80.0;
    impl->lastValidResults = {cached};
    impl->lastValidTimestamp = std::chrono::steady_clock::now() - Impl::MAX_STALE_RESULTS_AGE - std::chrono::seconds(1);
    impl->lastAdapterUtilization = {{"GPU_0x0_0x1", 80.0}};
    m_scenario->collectStatus = static_cast<PDH_STATUS>(PDH_CSTATUS_NO_INSTANCE);
    PDHGPUProbe probe(std::move(impl));

    EXPECT_TRUE(probe.readProcessGPUCounters().empty());
    EXPECT_TRUE(probe.adapterUtilization().empty());
}

TEST_F(WindowsPDHGPUProbeInjectedTest, WarmUpCollectLeavesAdapterUtilizationUnread)
{
    // The warm-up collect has no rates: its adapter utilization is unread, not 0% and not a
    // reading left over from before a counter re-add (#1166).
    auto impl = makeInjectedImpl(PDHGPUProbe::Role::Adapter);
    impl->warmedUp = false;
    impl->lastAdapterUtilization = {{"GPU_0x0_0x1", 80.0}};
    m_scenario->items[impl->utilizationCounter] = {
        {.name = L"pid_1310_luid_0x0_0x1_phys_0_eng_0_engtype_3D", .doubleValue = 5.0},
    };
    PDHGPUProbe probe(std::move(impl));

    static_cast<void>(probe.readProcessGPUCounters());
    EXPECT_TRUE(probe.adapterUtilization().empty());
    EXPECT_FALSE(probe.adapterUtilizationCurrent());

    // The next collect has rates.
    static_cast<void>(probe.readProcessGPUCounters());
    EXPECT_DOUBLE_EQ(probe.adapterUtilization().at("GPU_0x0_0x1"), 5.0);
    EXPECT_TRUE(probe.adapterUtilizationCurrent());
}

TEST_F(WindowsPDHGPUProbeInjectedTest, ASuccessfulCollectWithNoEngineActivityIsCurrentNotUnread)
{
    // PDH has GPU Engine instances only for processes using an adapter. A successful collect with
    // none is an idle GPU: current (so it publishes 0%), not unread like a warm-up (#1166).
    auto impl = makeInjectedImpl(PDHGPUProbe::Role::Adapter);
    impl->warmedUp = true;
    m_scenario->items[impl->utilizationCounter] = {};
    PDHGPUProbe probe(std::move(impl));

    static_cast<void>(probe.readProcessGPUCounters());
    EXPECT_TRUE(probe.adapterUtilization().empty());
    EXPECT_TRUE(probe.adapterUtilizationCurrent());
}

// #1277 review: a failed GPU Engine array read says nothing about any adapter, so it must not count as
// "current" -- otherwise every mapped adapter would publish as idle 0% instead of a gap.
TEST_F(WindowsPDHGPUProbeInjectedTest, AFailedUtilizationArrayReadIsNotCurrent)
{
    auto impl = makeInjectedImpl(PDHGPUProbe::Role::Adapter);
    impl->warmedUp = true;
    m_scenario->hardFailureStatus[impl->utilizationCounter] = static_cast<PDH_STATUS>(PDH_INVALID_HANDLE);
    PDHGPUProbe probe(std::move(impl));

    static_cast<void>(probe.readProcessGPUCounters());
    EXPECT_TRUE(probe.adapterUtilization().empty());
    EXPECT_FALSE(probe.adapterUtilizationCurrent());
}

// #1277 review: an adapter whose engine items all came back with a bad status was not idle; it was
// unread. Another adapter with a readable item keeps its reading.
TEST_F(WindowsPDHGPUProbeInjectedTest, AnAdapterWithOnlyUnreadableEngineItemsIsUnreadNotIdle)
{
    auto impl = makeInjectedImpl(PDHGPUProbe::Role::Adapter);
    impl->warmedUp = true;
    m_scenario->items[impl->utilizationCounter] = {
        {.name = L"pid_610_luid_0x0_0x1_phys_0_eng_0_engtype_3D", .cstatus = PDH_CSTATUS_INVALID_DATA, .doubleValue = 50.0},
        {.name = L"pid_611_luid_0x0_0x2_phys_0_eng_0_engtype_3D", .doubleValue = 7.0},
    };
    PDHGPUProbe probe(std::move(impl));

    static_cast<void>(probe.readProcessGPUCounters());
    EXPECT_TRUE(probe.adapterUtilizationCurrent());
    EXPECT_FALSE(probe.adapterUtilization().contains("GPU_0x0_0x1"));
    EXPECT_TRUE(probe.adapterUtilizationUnread().contains("GPU_0x0_0x1"));
    EXPECT_DOUBLE_EQ(probe.adapterUtilization().at("GPU_0x0_0x2"), 7.0);
    EXPECT_FALSE(probe.adapterUtilizationUnread().contains("GPU_0x0_0x2"));
}

TEST_F(WindowsPDHGPUProbeInjectedTest, WarmUpDoesNotMakeAnOldProcessCacheLookFresh)
{
    // The warm-up after a counter re-add used to return the cached per-process results and stamp
    // them as just read, so results from long before stood in for another staleness window.
    auto impl = makeInjectedImpl(PDHGPUProbe::Role::Process);
    impl->warmedUp = false;
    ProcessGPUCounters old;
    old.pid = 4242;
    impl->lastValidResults = {old};
    impl->lastValidTimestamp = std::chrono::steady_clock::now() - std::chrono::minutes{5};
    PDHGPUProbe probe(std::move(impl));

    EXPECT_TRUE(probe.readProcessGPUCounters().empty());
}

TEST_F(WindowsPDHGPUProbeInjectedTest, AFailedCollectClearsTheAdapterReadingsEvenWithAFreshProcessCache)
{
    // A failed collect may hand back the recent per-process results (#1034), but the adapter's
    // utilization and memory were not read: reusing them made a stale value look fresh (#1166).
    auto impl = makeInjectedImpl(PDHGPUProbe::Role::Adapter);
    m_scenario->items[impl->utilizationCounter] = {
        {.name = L"pid_1320_luid_0x0_0x1_phys_0_eng_0_engtype_3D", .doubleValue = 30.0},
    };
    m_scenario->items[impl->adapterSharedCounter] = {{.name = L"luid_0x0_0x1_phys_0", .largeValue = 1234}};
    // A per-process cache recent enough to stand in for the failed collect below.
    ProcessGPUCounters recent;
    recent.pid = 1320;
    impl->lastValidResults = {recent};
    impl->lastValidTimestamp = std::chrono::steady_clock::now();
    PDHGPUProbe probe(std::move(impl));
    static_cast<void>(probe.readProcessGPUCounters());
    ASSERT_FALSE(probe.adapterUtilization().empty());
    ASSERT_FALSE(probe.adapterMemory().empty());

    m_scenario->collectStatus = static_cast<PDH_STATUS>(PDH_CSTATUS_NO_INSTANCE);
    const auto cached = probe.readProcessGPUCounters();

    EXPECT_EQ(cached.size(), 1U) << "the recent per-process results still stand in";
    EXPECT_TRUE(probe.adapterUtilization().empty());
    EXPECT_TRUE(probe.adapterMemory().empty());
}

TEST_F(WindowsPDHGPUProbeInjectedTest, NoActiveCountersClearsTheAdapterReadings)
{
    auto impl = std::make_unique<Impl>();
    impl->pdhOpenQuery = fakeOpenQuery;
    impl->pdhCloseQuery = fakeCloseQuery;
    impl->pdhAddEnglishCounter = fakeAddEnglishCounter;
    impl->pdhCollectQueryData = fakeCollectQueryData;
    impl->pdhGetFormattedCounterArray = fakeGetFormattedCounterArray;
    // NOLINTNEXTLINE(performance-no-int-to-ptr) - a fake query handle the fakes never dereference
    impl->query = reinterpret_cast<PDH_HQUERY>(static_cast<std::uintptr_t>(1));
    impl->initialized = true;
    impl->lastAdapterUtilization = {{"GPU_0x0_0x1", 80.0}};
    impl->lastAdapterMemory = {{"GPU_0x0_0x1", {.dedicatedBytes = 1, .sharedBytes = 2}}};
    m_scenario->failToAdd[Impl::UTILIZATION_COUNTER_PATH] = true;
    m_scenario->failToAdd[Impl::DEDICATED_MEMORY_COUNTER_PATH] = true;
    m_scenario->failToAdd[Impl::SHARED_MEMORY_COUNTER_PATH] = true;
    m_scenario->failToAdd[Impl::ADAPTER_DEDICATED_COUNTER_PATH] = true;
    m_scenario->failToAdd[Impl::ADAPTER_SHARED_COUNTER_PATH] = true;
    PDHGPUProbe probe(std::move(impl));

    static_cast<void>(probe.readProcessGPUCounters());

    EXPECT_TRUE(probe.adapterUtilization().empty());
    EXPECT_TRUE(probe.adapterMemory().empty());
}

TEST(ParseAdapterInstanceLuidTest, AcceptsAdapterInstancesOnly)
{
    EXPECT_EQ(PDHGPUProbeImplDetail::parseAdapterInstanceLuid("luid_0x00000000_0x0001752D_phys_0"), "0x00000000_0x0001752D");
    EXPECT_EQ(PDHGPUProbeImplDetail::parseAdapterInstanceLuid("luid_0x0_0x1_phys_12"), "0x0_0x1");
    EXPECT_TRUE(PDHGPUProbeImplDetail::parseAdapterInstanceLuid("pid_1_luid_0x0_0x1_phys_0").empty());
    EXPECT_TRUE(PDHGPUProbeImplDetail::parseAdapterInstanceLuid("luid_0x0_0x1").empty());
    EXPECT_TRUE(PDHGPUProbeImplDetail::parseAdapterInstanceLuid("luid_0xZZ_0x1_phys_0").empty());
    EXPECT_TRUE(PDHGPUProbeImplDetail::parseAdapterInstanceLuid("luid_0x0_0x1_phys_").empty());
    EXPECT_TRUE(PDHGPUProbeImplDetail::parseAdapterInstanceLuid("luid_0x_0x1_phys_0").empty());
}

TEST_F(WindowsPDHGPUProbeInjectedTest, AdapterMemoryIsReadFromTheAdapterCounters)
{
    // The GPU tab's Memory line used to be TaskSmack's own usage (DXGI QueryVideoMemoryInfo);
    // it now comes from the adapter-wide counters, summed over physical nodes (#1029).
    auto impl = makeInjectedImpl(PDHGPUProbe::Role::Adapter);
    ASSERT_NE(impl->adapterDedicatedCounter, nullptr);
    ASSERT_NE(impl->adapterSharedCounter, nullptr);
    m_scenario->items[impl->adapterDedicatedCounter] = {
        {.name = L"luid_0x0_0x1_phys_0", .largeValue = 300},
        {.name = L"luid_0x0_0x2_phys_0", .largeValue = 5000},
    };
    m_scenario->items[impl->adapterSharedCounter] = {
        {.name = L"luid_0x0_0x1_phys_0", .largeValue = 1000},
        {.name = L"luid_0x0_0x1_phys_1", .largeValue = 500},
        {.name = L"not_an_adapter_instance", .largeValue = 999},
    };
    PDHGPUProbe probe(std::move(impl));
    static_cast<void>(probe.readProcessGPUCounters());

    const auto memory = probe.adapterMemory();
    ASSERT_EQ(memory.size(), 2U);
    EXPECT_EQ(memory.at("GPU_0x0_0x1").dedicatedBytes, 300ULL);
    EXPECT_EQ(memory.at("GPU_0x0_0x1").sharedBytes, 1500ULL);
    EXPECT_TRUE(memory.at("GPU_0x0_0x1").dedicatedRead);
    EXPECT_TRUE(memory.at("GPU_0x0_0x1").sharedRead);
    EXPECT_EQ(memory.at("GPU_0x0_0x2").dedicatedBytes, 5000ULL);
    EXPECT_TRUE(memory.at("GPU_0x0_0x2").dedicatedRead);
    EXPECT_FALSE(memory.at("GPU_0x0_0x2").sharedRead) << "no shared item for this adapter";
}

TEST_F(WindowsPDHGPUProbeInjectedTest, AZeroByteAdapterItemIsAReadingAndAFailedSegmentIsUnread)
{
    // #1246: an idle adapter's 0 B is a reading, so its entry exists with that segment read. A
    // segment whose array failed is unread for every adapter, even where the other one was read.
    auto impl = makeInjectedImpl(PDHGPUProbe::Role::Adapter);
    m_scenario->items[impl->adapterDedicatedCounter] = {
        {.name = L"luid_0x0_0x1_phys_0", .largeValue = 0},
        {.name = L"luid_0x0_0x2_phys_0", .cstatus = PDH_CSTATUS_INVALID_DATA, .largeValue = 77},
    };
    m_scenario->hardFailureStatus[impl->adapterSharedCounter] = static_cast<PDH_STATUS>(PDH_CSTATUS_NO_OBJECT);
    PDHGPUProbe probe(std::move(impl));

    static_cast<void>(probe.readProcessGPUCounters());

    const auto memory = probe.adapterMemory();
    ASSERT_EQ(memory.size(), 1U) << "a bad-status item is not a reading";
    const auto& usage = memory.at("GPU_0x0_0x1");
    EXPECT_TRUE(usage.dedicatedRead);
    EXPECT_EQ(usage.dedicatedBytes, 0ULL);
    EXPECT_FALSE(usage.sharedRead);
}

TEST_F(WindowsPDHGPUProbeInjectedTest, AdapterMemoryIsCollectedWhenOnlyTheAdapterCountersAreAvailable)
{
    // With the engine and process-memory counters unavailable, the adapter counters alone must
    // still be collected, or the GPU tab's Memory line reads zero (#1029).
    m_scenario->failToAdd[Impl::UTILIZATION_COUNTER_PATH] = true;
    m_scenario->failToAdd[Impl::DEDICATED_MEMORY_COUNTER_PATH] = true;
    m_scenario->failToAdd[Impl::SHARED_MEMORY_COUNTER_PATH] = true;
    auto impl = makeInjectedImpl(PDHGPUProbe::Role::Adapter);
    ASSERT_EQ(impl->utilizationCounter, nullptr);
    ASSERT_NE(impl->adapterSharedCounter, nullptr);
    m_scenario->items[impl->adapterSharedCounter] = {{.name = L"luid_0x0_0x1_phys_0", .largeValue = 1234}};
    PDHGPUProbe probe(std::move(impl));

    static_cast<void>(probe.readProcessGPUCounters());

    const auto memory = probe.adapterMemory();
    ASSERT_EQ(memory.size(), 1U);
    EXPECT_EQ(memory.at("GPU_0x0_0x1").sharedBytes, 1234ULL);
}

TEST_F(WindowsPDHGPUProbeInjectedTest, AdapterMemoryIsReadOnTheWarmUpCollect)
{
    // The adapter memory counters are gauges, so the first collect already has them; skipping
    // them during warm-up published the first GPU refresh with 0 bytes in use (#1029).
    auto impl = makeInjectedImpl(PDHGPUProbe::Role::Adapter);
    impl->warmedUp = false;
    m_scenario->items[impl->adapterSharedCounter] = {{.name = L"luid_0x0_0x1_phys_0", .largeValue = 1234}};
    m_scenario->items[impl->utilizationCounter] = {
        {.name = L"pid_802_luid_0x0_0x1_phys_0_eng_0_engtype_3D", .doubleValue = 5.0},
    };
    PDHGPUProbe probe(std::move(impl));

    const auto results = probe.readProcessGPUCounters();

    // Utilization is still a warm-up: no process rates yet.
    EXPECT_TRUE(results.empty());
    EXPECT_TRUE(probe.adapterUtilization().empty());
    const auto memory = probe.adapterMemory();
    ASSERT_EQ(memory.size(), 1U);
    EXPECT_EQ(memory.at("GPU_0x0_0x1").sharedBytes, 1234ULL);
}

TEST_F(WindowsPDHGPUProbeInjectedTest, MemoryCounterSkipsFailingCstatusAndMalformedNames)
{
    auto impl = makeInjectedImpl();
    m_scenario->items[impl->dedicatedMemoryCounter] = {
        {.name = L"pid_801_luid_0x0_0x1_phys_0", .cstatus = PDH_CSTATUS_INVALID_DATA, .largeValue = 999},
        {.name = L"garbage_no_pid_here", .largeValue = 111},
        {.name = L"pid_802_luid_0x0_0x1_phys_0", .cstatus = PDH_CSTATUS_NEW_DATA, .largeValue = 4096},
    };
    m_scenario->items[impl->utilizationCounter] = {
        {.name = L"pid_802_luid_0x0_0x1_phys_0_eng_0_engtype_3D", .doubleValue = 5.0},
    };

    PDHGPUProbe probe(std::move(impl));
    const auto results = probe.readProcessGPUCounters();

    ASSERT_EQ(results.size(), 1U);
    EXPECT_EQ(results[0].pid, 802);
    EXPECT_EQ(results[0].gpuMemoryBytes, 4096U);
}

// #1164: a process's dedicated and shared GPU memory are reported apart, as the adapter's are, so
// Domain can compare each with the adapter's matching figure instead of a sum that could exceed it.
TEST_F(WindowsPDHGPUProbeInjectedTest, DedicatedAndSharedMemoryAreKeptApart)
{
    auto impl = makeInjectedImpl();
    m_scenario->items[impl->dedicatedMemoryCounter] = {{.name = L"pid_802_luid_0x0_0x1_phys_0", .largeValue = 4096}};
    m_scenario->items[impl->sharedMemoryCounter] = {{.name = L"pid_802_luid_0x0_0x1_phys_0", .largeValue = 1000}};
    m_scenario->items[impl->utilizationCounter] = {
        {.name = L"pid_802_luid_0x0_0x1_phys_0_eng_0_engtype_3D", .doubleValue = 5.0},
    };

    PDHGPUProbe probe(std::move(impl));
    const auto results = probe.readProcessGPUCounters();

    ASSERT_EQ(results.size(), 1U);
    EXPECT_EQ(results[0].gpuMemoryBytes, 4096U);
    EXPECT_EQ(results[0].gpuSharedMemoryBytes, 1000U);
}

TEST_F(WindowsPDHGPUProbeInjectedTest, ReadCounterArrayHardFailureIsTreatedAsNoData)
{
    auto impl = makeInjectedImpl();
    m_scenario->hardFailureStatus[impl->utilizationCounter] = static_cast<PDH_STATUS>(PDH_CSTATUS_NO_OBJECT);

    PDHGPUProbe probe(std::move(impl));
    std::vector<ProcessGPUCounters> results;
    EXPECT_NO_THROW(results = probe.readProcessGPUCounters());
    EXPECT_TRUE(results.empty());
}

TEST_F(WindowsPDHGPUProbeInjectedTest, ReadCounterArrayExhaustingRetriesReturnsNoData)
{
    auto impl = makeInjectedImpl();
    const PDH_HCOUNTER utilizationCounter = impl->utilizationCounter;
    // More forced PDH_MORE_DATA rounds than readCounterArray's 3-attempt retry loop allows,
    // so every attempt fails and the loop must exit via its final `return nullptr;` rather
    // than looping forever or crashing.
    m_scenario->extraMoreDataRounds[utilizationCounter] = 5;
    m_scenario->items[utilizationCounter] = {
        {.name = L"pid_803_luid_0x0_0x1_phys_0_eng_0_engtype_3D", .doubleValue = 7.0},
    };

    PDHGPUProbe probe(std::move(impl));
    std::vector<ProcessGPUCounters> results;
    EXPECT_NO_THROW(results = probe.readProcessGPUCounters());
    EXPECT_TRUE(results.empty());
}

TEST_F(WindowsPDHGPUProbeInjectedTest, ResizesBufferOnPdhMoreDataThenSucceeds)
{
    auto impl = makeInjectedImpl();
    const PDH_HCOUNTER utilizationCounter = impl->utilizationCounter;
    m_scenario->extraMoreDataRounds[utilizationCounter] = 2;
    m_scenario->items[utilizationCounter] = {
        {.name = L"pid_100_luid_0x0_0x1_phys_0_eng_0_engtype_3D", .doubleValue = 42.0},
    };

    PDHGPUProbe probe(std::move(impl));
    const auto results = probe.readProcessGPUCounters();

    ASSERT_EQ(results.size(), 1U);
    EXPECT_EQ(results[0].pid, 100);
    EXPECT_DOUBLE_EQ(results[0].gpuUtilPercent, 42.0);
    EXPECT_GE(m_scenario->getArrayCallCount, 3);

    const auto& requiredSizes = m_scenario->moreDataRequiredSizes[utilizationCounter];
    ASSERT_EQ(requiredSizes.size(), 2U);
    EXPECT_LT(requiredSizes[0], requiredSizes[1]) << "each retry should report a larger requirement than the last";

    const auto firstReadSizes = m_scenario->seenBufferSizes[utilizationCounter];
    const auto firstReadPtrs = m_scenario->seenBufferPointers[utilizationCounter];
    ASSERT_FALSE(firstReadSizes.empty());
    ASSERT_EQ(firstReadSizes.size(), firstReadPtrs.size());
    EXPECT_EQ(firstReadSizes.back(), requiredSizes.back())
        << "the eventual successful read must use a buffer sized to the largest reported requirement, not an earlier one";

    m_scenario->seenBufferSizes[utilizationCounter].clear();
    m_scenario->seenBufferPointers[utilizationCounter].clear();
    const auto secondResults = probe.readProcessGPUCounters();
    ASSERT_EQ(secondResults.size(), 1U);

    const auto secondReadSizes = m_scenario->seenBufferSizes[utilizationCounter];
    const auto secondReadPtrs = m_scenario->seenBufferPointers[utilizationCounter];
    ASSERT_FALSE(secondReadSizes.empty());
    ASSERT_EQ(secondReadSizes.size(), secondReadPtrs.size());
    EXPECT_GE(secondReadSizes.front(), static_cast<DWORD>(sizeof(PDH_FMT_COUNTERVALUE_ITEM_W)));
    EXPECT_NE(secondReadPtrs.front(), nullptr);
    EXPECT_EQ(secondReadSizes.front(), firstReadSizes.back());
    EXPECT_EQ(secondReadPtrs.front(), firstReadPtrs.back());
}

TEST_F(WindowsPDHGPUProbeInjectedTest, MalformedInstanceNamesAreSkippedWithoutCrashing)
{
    auto impl = makeInjectedImpl();
    m_scenario->items[impl->utilizationCounter] = {
        {.name = L"garbage_no_pid_here", .doubleValue = 10.0},
        {.name = L"", .doubleValue = 5.0},
        {.name = L"pid_12x_luid_0x0_0x2_phys_0_eng_0_engtype_3D", .doubleValue = 12.0},
        {.name = L"pid_123_junk", .doubleValue = 13.0},
        {.name = L"pid_201_luid_not-a-luid_engtype_3D", .doubleValue = 14.0},
        {.name = L"garbage_pid_202_luid_0x0_0x2_phys_0_eng_0_engtype_3D", .doubleValue = 16.0},
        {.name = L"pid_203_luid_0xGG_0x1_phys_0_eng_0_engtype_3D", .doubleValue = 17.0},
        {.name = L"pid_209_luid_0x0_0x1junk_phys_0_eng_0_engtype_3D", .doubleValue = 23.0},
        {.name = L"pid_204_junk_luid_0x0_0x1_phys_0", .doubleValue = 18.0},
        {.name = L"pid_205_luid_0x0_0x1_phys_", .doubleValue = 19.0},
        {.name = L"pid_208_luid_0x0_0x1_phys_x_eng_0_engtype_3D", .doubleValue = 22.0},
        {.name = L"pid_200_luid_0x0_0x2_phys_0_eng_0_engtype_3D", .doubleValue = 15.0},
    };

    PDHGPUProbe probe(std::move(impl));
    std::vector<ProcessGPUCounters> results;
    EXPECT_NO_THROW(results = probe.readProcessGPUCounters());

    ASSERT_EQ(results.size(), 1U);
    EXPECT_EQ(results[0].pid, 200);
    EXPECT_DOUBLE_EQ(results[0].gpuUtilPercent, 15.0);
}

TEST_F(WindowsPDHGPUProbeInjectedTest, EmptyEngineTypeSuffixIsValidWithUnnamedEngine)
{
    // Real-world Intel Arc driver instance names observed via `Get-Counter -ListSet "GPU
    // Engine"`: several engine indices (e.g. eng_7 through eng_13) report a trailing
    // "_engtype_" with nothing after it because Windows hasn't assigned that engine index a
    // named type yet. These are well-formed, valid PDH instances - dropping them undercounts
    // total GPU utilization for any process using those engines. Both the "_phys_..._eng_N_
    // engtype_" and the simplified "_engtype_" (no "_phys_") shapes must accept an empty type.
    auto impl = makeInjectedImpl();
    m_scenario->items[impl->utilizationCounter] = {
        {.name = L"pid_206_luid_0x0_0x1_engtype_", .doubleValue = 20.0},
        {.name = L"pid_207_luid_0x0_0x1_phys_0_eng_0_engtype_", .doubleValue = 21.0},
    };

    PDHGPUProbe probe(std::move(impl));
    auto results = probe.readProcessGPUCounters();
    std::ranges::sort(results, {}, &ProcessGPUCounters::pid);

    ASSERT_EQ(results.size(), 2U);
    EXPECT_EQ(results[0].pid, 206);
    EXPECT_DOUBLE_EQ(results[0].gpuUtilPercent, 20.0);
    EXPECT_TRUE(results[0].activeEngines.empty());
    EXPECT_EQ(results[1].pid, 207);
    EXPECT_DOUBLE_EQ(results[1].gpuUtilPercent, 21.0);
    EXPECT_TRUE(results[1].activeEngines.empty());
}

TEST_F(WindowsPDHGPUProbeInjectedTest, SimplifiedFormatWithoutPhysSuffixIsParsed)
{
    // Some drivers emit "pid_<N>_luid_<...>_engtype_<T>" with no "_phys_" segment at all;
    // this is the only path through parseInstanceName() that accepts that shape, so it needs
    // its own positive coverage alongside the malformed-simplified-format cases above.
    auto impl = makeInjectedImpl();
    m_scenario->items[impl->utilizationCounter] = {
        {.name = L"pid_600_luid_0x0_0x1_engtype_3D", .doubleValue = 33.0},
    };

    PDHGPUProbe probe(std::move(impl));
    const auto results = probe.readProcessGPUCounters();

    ASSERT_EQ(results.size(), 1U);
    EXPECT_EQ(results[0].pid, 600);
    EXPECT_DOUBLE_EQ(results[0].gpuUtilPercent, 33.0);
    ASSERT_EQ(results[0].activeEngines.size(), 1U);
    EXPECT_EQ(results[0].activeEngines[0], "3D");
}

TEST_F(WindowsPDHGPUProbeInjectedTest, SamePidDifferentLuidAggregatesSeparately)
{
    auto impl = makeInjectedImpl();
    m_scenario->items[impl->utilizationCounter] = {
        {.name = L"pid_300_luid_0x0_0x1_phys_0_eng_0_engtype_3D", .doubleValue = 20.0},
        {.name = L"pid_300_luid_0x0_0x2_phys_0_eng_0_engtype_3D", .doubleValue = 30.0},
    };

    PDHGPUProbe probe(std::move(impl));
    const auto results = probe.readProcessGPUCounters();

    ASSERT_EQ(results.size(), 2U);
    std::unordered_map<std::string, double> utilByGpu;
    for (const auto& r : results)
    {
        EXPECT_EQ(r.pid, 300);
        utilByGpu[r.gpuId] = r.gpuUtilPercent;
    }
    EXPECT_NE(results[0].gpuId, results[1].gpuId);
    EXPECT_DOUBLE_EQ(utilByGpu["GPU_0x0_0x1"], 20.0);
    EXPECT_DOUBLE_EQ(utilByGpu["GPU_0x0_0x2"], 30.0);
}

TEST_F(WindowsPDHGPUProbeInjectedTest, MissingCounterReturnsPartialDataThenRecoversOnRetry)
{
    auto impl = std::make_unique<Impl>();
    impl->pdhOpenQuery = fakeOpenQuery;
    impl->pdhCloseQuery = fakeCloseQuery;
    impl->pdhAddEnglishCounter = fakeAddEnglishCounter;
    impl->pdhCollectQueryData = fakeCollectQueryData;
    impl->pdhGetFormattedCounterArray = fakeGetFormattedCounterArray;
    impl->query = reinterpret_cast<PDH_HQUERY>(static_cast<std::uintptr_t>(1));
    impl->initialized = true;

    m_scenario->failToAdd[Impl::DEDICATED_MEMORY_COUNTER_PATH] = true;
    impl->ensureCounters();
    impl->warmedUp = true;
    ASSERT_NE(impl->utilizationCounter, nullptr);
    ASSERT_EQ(impl->dedicatedMemoryCounter, nullptr);

    m_scenario->items[impl->utilizationCounter] = {
        {.name = L"pid_400_luid_0x0_0x1_phys_0_eng_0_engtype_3D", .doubleValue = 25.0},
    };

    PDHGPUProbe probe(std::move(impl));
    const auto firstResults = probe.readProcessGPUCounters();
    ASSERT_EQ(firstResults.size(), 1U);
    EXPECT_EQ(firstResults[0].pid, 400);
    EXPECT_DOUBLE_EQ(firstResults[0].gpuUtilPercent, 25.0);
    EXPECT_EQ(firstResults[0].gpuMemoryBytes, 0U);

    m_scenario->failToAdd[Impl::DEDICATED_MEMORY_COUNTER_PATH] = false;
    const auto expectedDedicatedHandle = reinterpret_cast<PDH_HCOUNTER>(m_scenario->nextHandleValue);
    m_scenario->items[expectedDedicatedHandle] = {
        {.name = L"pid_400_luid_0x0_0x1_phys_0", .largeValue = 1234},
    };

    const auto secondResults = probe.readProcessGPUCounters();
    ASSERT_EQ(secondResults.size(), 1U);
    EXPECT_EQ(secondResults[0].gpuMemoryBytes, 1234U);
    EXPECT_DOUBLE_EQ(secondResults[0].gpuUtilPercent, 25.0);
}

TEST_F(WindowsPDHGPUProbeInjectedTest, ItemsWithFailingCStatusAreSkipped)
{
    auto impl = makeInjectedImpl();
    m_scenario->items[impl->utilizationCounter] = {
        {.name = L"pid_500_luid_0x0_0x1_phys_0_eng_0_engtype_3D", .cstatus = PDH_CSTATUS_INVALID_DATA, .doubleValue = 999.0},
        {.name = L"pid_500_luid_0x0_0x1_phys_0_eng_1_engtype_Compute", .cstatus = PDH_CSTATUS_NEW_DATA, .doubleValue = 8.0},
    };

    PDHGPUProbe probe(std::move(impl));
    const auto results = probe.readProcessGPUCounters();

    ASSERT_EQ(results.size(), 1U);
    EXPECT_DOUBLE_EQ(results[0].gpuUtilPercent, 8.0);
}

// =============================================================================
// instanceForAt's positional-slot shortcut (see PDHGPUProbeImpl.h): a second call with the
// same item at the same array index should skip the hash-map lookup entirely; anything else
// (reordering, or content changing at a fixed index) must still produce correct data by
// falling back to the hash-map path, never a stale/wrong answer.
// =============================================================================

TEST_F(WindowsPDHGPUProbeInjectedTest, RepeatedIdenticalOrderHitsPositionalCacheOnSecondCall)
{
    auto impl = makeInjectedImpl();
    m_scenario->items[impl->utilizationCounter] = {
        {.name = L"pid_900_luid_0x0_0x1_phys_0_eng_0_engtype_3D", .doubleValue = 10.0},
        {.name = L"pid_901_luid_0x0_0x1_phys_0_eng_0_engtype_Compute", .doubleValue = 20.0},
    };

    PDHGPUProbe probe(std::move(impl));
    const auto first = probe.readProcessGPUCounters();
    ASSERT_EQ(first.size(), 2U);

    const auto statsAfterFirst = probe.instanceCacheStats();
    const auto second = probe.readProcessGPUCounters();
    const auto statsAfterSecond = probe.instanceCacheStats();

    ASSERT_EQ(second.size(), 2U);
    // Same items, same order: the second call's 2 lookups should be positional hits, and
    // must NOT touch the underlying hash map at all (its hit/miss counters stay unchanged).
    EXPECT_EQ(statsAfterSecond.positionalHits - statsAfterFirst.positionalHits, 2U);
    EXPECT_EQ(statsAfterSecond.hits, statsAfterFirst.hits);
    EXPECT_EQ(statsAfterSecond.misses, statsAfterFirst.misses);
}

TEST_F(WindowsPDHGPUProbeInjectedTest, ReorderedItemsStillProduceCorrectDataViaFallback)
{
    auto impl = makeInjectedImpl();
    const PDH_HCOUNTER utilizationCounter = impl->utilizationCounter;
    m_scenario->items[utilizationCounter] = {
        {.name = L"pid_910_luid_0x0_0x1_phys_0_eng_0_engtype_3D", .doubleValue = 11.0},
        {.name = L"pid_911_luid_0x0_0x1_phys_0_eng_0_engtype_Compute", .doubleValue = 22.0},
    };

    PDHGPUProbe probe(std::move(impl));
    const auto first = probe.readProcessGPUCounters();
    ASSERT_EQ(first.size(), 2U);

    // Swap the two items' positions - the same instances, but at each other's array index -
    // simulating PDH returning a different (still valid) order on a later collect.
    m_scenario->items[utilizationCounter] = {
        {.name = L"pid_911_luid_0x0_0x1_phys_0_eng_0_engtype_Compute", .doubleValue = 22.0},
        {.name = L"pid_910_luid_0x0_0x1_phys_0_eng_0_engtype_3D", .doubleValue = 11.0},
    };

    const auto second = probe.readProcessGPUCounters();
    ASSERT_EQ(second.size(), 2U);
    std::unordered_map<int32_t, double> utilByPid;
    for (const auto& c : second)
    {
        utilByPid[c.pid] = c.gpuUtilPercent;
    }
    EXPECT_DOUBLE_EQ(utilByPid[910], 11.0) << "reordering must not scramble which value belongs to which pid";
    EXPECT_DOUBLE_EQ(utilByPid[911], 22.0) << "reordering must not scramble which value belongs to which pid";
}

TEST_F(WindowsPDHGPUProbeInjectedTest, ContentChangeAtSameIndexIsDetectedNotStale)
{
    // Mirrors the real-world case this file's own comments describe: a driver assigning an
    // engine index a type it previously left blank. The positional cache must detect the
    // content change at that fixed slot rather than serving the stale cached engineType.
    auto impl = makeInjectedImpl();
    const PDH_HCOUNTER utilizationCounter = impl->utilizationCounter;
    m_scenario->items[utilizationCounter] = {
        {.name = L"pid_920_luid_0x0_0x1_phys_0_eng_5_engtype_", .doubleValue = 5.0},
    };

    PDHGPUProbe probe(std::move(impl));
    const auto first = probe.readProcessGPUCounters();
    ASSERT_EQ(first.size(), 1U);
    EXPECT_TRUE(first[0].activeEngines.empty()) << "unnamed engine type at first read";

    // Same pid/luid/phys/eng slot, but the driver now reports a real engine type.
    m_scenario->items[utilizationCounter] = {
        {.name = L"pid_920_luid_0x0_0x1_phys_0_eng_5_engtype_3D", .doubleValue = 6.0},
    };

    const auto second = probe.readProcessGPUCounters();
    ASSERT_EQ(second.size(), 1U);
    EXPECT_DOUBLE_EQ(second[0].gpuUtilPercent, 6.0);
    ASSERT_EQ(second[0].activeEngines.size(), 1U);
    EXPECT_EQ(second[0].activeEngines[0], "3D") << "content change at the same slot must not be served from stale positional cache";
}

// pdh.dll is a core OS component present on every Windows machine, so loadPDH()/initialize()'s
// real dynamic-loading path always succeeds in CI - only the "already initialized" early-return
// guard is exercisable this way, not the load-failure branches (those would need mocking
// LoadLibraryW/GetProcAddress themselves, not worth it for a defensive guard like NVML's #781).
TEST(WindowsPDHGPUProbeRealImplTest, InitializeCalledTwiceReturnsTrueImmediately)
{
    Impl impl;
    ASSERT_TRUE(impl.initialize());
    EXPECT_TRUE(impl.initialize()) << "second call should hit the already-initialized early return, not reload pdh.dll";
}

TEST(WindowsPDHGPUProbeInstanceCacheTest, CacheClearsPastLimitWithoutLosingCorrectness)
{
    Impl impl;

    std::size_t maxObservedSize = 0;
    for (std::size_t i = 0; i <= Impl::MAX_INSTANCE_CACHE_ENTRIES; ++i)
    {
        const std::wstring name = L"pid_" + std::to_wstring(i + 1) + L"_luid_0x0_0x1_phys_0_eng_0_engtype_3D";
        const auto& cached = impl.instanceFor(name);
        EXPECT_TRUE(cached.valid);
        EXPECT_EQ(cached.pid, static_cast<std::int32_t>(i + 1));
        maxObservedSize = std::max(maxObservedSize, impl.instanceCache.size());
    }

    // Add one more unique name to force the clear guard path:
    // size() > MAX_INSTANCE_CACHE_ENTRIES before insertion.
    const auto& afterClearCandidate = impl.instanceFor(L"pid_99999_luid_0x0_0x1_phys_0_eng_0_engtype_3D");
    EXPECT_TRUE(afterClearCandidate.valid);
    EXPECT_EQ(afterClearCandidate.pid, 99999);

    EXPECT_LE(impl.instanceCache.size(), Impl::MAX_INSTANCE_CACHE_ENTRIES + 1);
    EXPECT_LT(impl.instanceCache.size(), maxObservedSize);

    const auto& reparsed = impl.instanceFor(L"pid_1_luid_0x0_0x1_phys_0_eng_0_engtype_3D");
    EXPECT_TRUE(reparsed.valid);
    EXPECT_EQ(reparsed.pid, 1);
    EXPECT_EQ(reparsed.engineType, "3D");
}

} // namespace
} // namespace Platform

#endif // _WIN32
