#ifdef _WIN32

#include "Platform/Windows/PDHGPUProbe.h"
#include "Platform/Windows/PDHGPUProbeImpl.h"

#include <gtest/gtest.h>

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

PDH_STATUS WINAPI fakeGetFormattedCounterArray(PDH_HCOUNTER counter, DWORD format, LPDWORD bufferSize, LPDWORD itemCount,
                                               PPDH_FMT_COUNTERVALUE_ITEM_W buffer)
{
    ++g_scenario->getArrayCallCount;
    static const std::vector<FakeItem> emptyList;
    const auto listIt = g_scenario->items.find(counter);
    const std::vector<FakeItem>& list = (listIt != g_scenario->items.end()) ? listIt->second : emptyList;

    if (list.empty())
    {
        *itemCount = 0;
        return ERROR_SUCCESS;
    }

    if (auto more = g_scenario->extraMoreDataRounds.find(counter); more != g_scenario->extraMoreDataRounds.end() && more->second > 0)
    {
        --more->second;
        *bufferSize = static_cast<DWORD>(sizeof(PDH_FMT_COUNTERVALUE_ITEM_W) * (list.size() + 1));
        return static_cast<PDH_STATUS>(PDH_MORE_DATA);
    }

    const auto needed = static_cast<DWORD>(sizeof(PDH_FMT_COUNTERVALUE_ITEM_W) * list.size());
    if (*bufferSize < needed || buffer == nullptr)
    {
        *bufferSize = needed;
        return static_cast<PDH_STATUS>(PDH_MORE_DATA);
    }

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
    m_scenario->extraMoreDataRounds[impl->utilizationCounter] = 2;
    m_scenario->items[impl->utilizationCounter] = {
        {.name = L"pid_100_luid_0x0_0x1_phys_0_eng_0_engtype_3D", .doubleValue = 42.0},
    };

    PDHGPUProbe probe(std::move(impl));
    const auto results = probe.readProcessGPUCounters();

    ASSERT_EQ(results.size(), 1U);
    EXPECT_EQ(results[0].pid, 100);
    EXPECT_DOUBLE_EQ(results[0].gpuUtilPercent, 42.0);
    EXPECT_GE(m_scenario->getArrayCallCount, 3);
}

TEST_F(WindowsPDHGPUProbeInjectedTest, MalformedInstanceNamesAreSkippedWithoutCrashing)
{
    auto impl = makeInjectedImpl();
    m_scenario->items[impl->utilizationCounter] = {
        {.name = L"garbage_no_pid_here", .doubleValue = 10.0},
        {.name = L"", .doubleValue = 5.0},
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
    for (const auto& r : results)
    {
        EXPECT_EQ(r.pid, 300);
    }
    EXPECT_NE(results[0].gpuId, results[1].gpuId);
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

    for (std::size_t i = 0; i <= Impl::MAX_INSTANCE_CACHE_ENTRIES; ++i)
    {
        const std::wstring name = L"pid_" + std::to_wstring(i + 1) + L"_luid_0x0_0x1_phys_0_eng_0_engtype_3D";
        const auto& cached = impl.instanceFor(name);
        EXPECT_TRUE(cached.valid);
        EXPECT_EQ(cached.pid, static_cast<std::int32_t>(i + 1));
    }

    EXPECT_LE(impl.instanceCache.size(), Impl::MAX_INSTANCE_CACHE_ENTRIES + 1);

    const auto& reparsed = impl.instanceFor(L"pid_1_luid_0x0_0x1_phys_0_eng_0_engtype_3D");
    EXPECT_TRUE(reparsed.valid);
    EXPECT_EQ(reparsed.pid, 1);
    EXPECT_EQ(reparsed.engineType, "3D");
}

} // namespace
} // namespace Platform

#endif // _WIN32
