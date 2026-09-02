#ifdef _WIN32

#include "Platform/Windows/PDHGPUProbe.h"
#include "Platform/Windows/PDHGPUProbeImpl.h"

#include <gtest/gtest.h>

#include <algorithm>
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

    EXPECT_EQ(caps.hasPerProcessMetrics, caps.hasEngineUtilization);
    EXPECT_FALSE(caps.hasTemperature);
    EXPECT_FALSE(caps.hasPowerMetrics);
    EXPECT_FALSE(caps.hasClockSpeeds);
    EXPECT_FALSE(caps.hasFanSpeed);
}

using Impl = PDHGPUProbe::Impl;

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
};

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

std::unique_ptr<Impl> makeInjectedImpl()
{
    auto impl = std::make_unique<Impl>();
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
        {.name = L"pid_203_luid_0xGG_0x1junk_phys_0_eng_0_engtype_3D", .doubleValue = 17.0},
        {.name = L"pid_204_junk_luid_0x0_0x1_phys_0", .doubleValue = 18.0},
        {.name = L"pid_205_luid_0x0_0x1_phys_", .doubleValue = 19.0},
        {.name = L"pid_206_luid_0x0_0x1_engtype_", .doubleValue = 20.0},
        {.name = L"pid_207_luid_0x0_0x1_phys_0_eng_0_engtype_", .doubleValue = 21.0},
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
