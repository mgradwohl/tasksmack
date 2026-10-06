/// @file test_CpuCoreGridIds.cpp
/// @brief Tests for App::CpuCoresSection::selectCpuCoreGridIds(): the CPU Cores grid charts only the
///        core ids the probe reported this session (#1262)

#include "App/Panels/CpuCoreGridIds.h"
#include "Domain/SystemModel.h"
#include "Mocks/MockProbes.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace
{

using App::CpuCoresSection::selectCpuCoreGridIds;
using App::CpuCoresSection::showCpuCoresTab;
using TestMocks::makeCpuCounters;
using TestMocks::makeMemoryCounters;
using TestMocks::makeSystemCounters;
using TestMocks::MockSystemProbe;

void setCoreSample(MockSystemProbe& probe, std::uint64_t sample, const std::vector<std::size_t>& onlineIds)
{
    auto counters = makeSystemCounters(makeCpuCounters(0, 0, 0, 1000 * sample), makeMemoryCounters(1024, 512));
    for (const std::size_t id : onlineIds)
    {
        auto core = makeCpuCounters(100 * sample, 0, 0, 900 * sample);
        core.coreId = id;
        counters.cpuPerCore.push_back(core);
    }
    probe.setCounters(counters);
}

/// The ids the grid would chart for the model's latest publication, sized the way
/// CpuCoresSection sizes its slots.
std::vector<std::size_t> gridIdsFor(const Domain::SystemModel& model)
{
    const auto publication = model.publication();
    const std::size_t slotCount = std::max(publication->perCoreHistory.size(), publication->snapshot.cpuPerCore.size());
    std::vector<std::size_t> ids;
    selectCpuCoreGridIds(publication->snapshot.seenCoreIds, slotCount, ids);
    return ids;
}

TEST(CpuCoreGridIdsTest, NeverReportedIdsGetNoChart)
{
    // Ids {0-3, 6-8}: nine slots, seven cores (#1262).
    auto probe = std::make_unique<MockSystemProbe>();
    auto* rawProbe = probe.get();
    const std::vector<std::size_t> reported{0, 1, 2, 3, 6, 7, 8};
    setCoreSample(*rawProbe, 0, reported);
    Domain::SystemModel model(std::move(probe));
    model.refresh();
    setCoreSample(*rawProbe, 1, reported);
    model.refresh();

    const auto ids = gridIdsFor(model);
    EXPECT_EQ(ids.size(), 7U);
    EXPECT_EQ(ids, reported);
}

TEST(CpuCoreGridIdsTest, ACoreSeenThenOfflineKeepsItsChart)
{
    auto probe = std::make_unique<MockSystemProbe>();
    auto* rawProbe = probe.get();
    setCoreSample(*rawProbe, 0, {0, 1, 2, 3});
    Domain::SystemModel model(std::move(probe));
    model.refresh();
    setCoreSample(*rawProbe, 1, {0, 1, 2, 3});
    model.refresh();
    setCoreSample(*rawProbe, 2, {0, 1}); // cpu2 and cpu3 offline
    model.refresh();
    setCoreSample(*rawProbe, 3, {0, 1});
    model.refresh();

    EXPECT_EQ(gridIdsFor(model), (std::vector<std::size_t>{0, 1, 2, 3}));
}

TEST(CpuCoreGridIdsTest, TwoCpusDroppingToOneKeepTheTabAndTheOfflineChart)
{
    auto probe = std::make_unique<MockSystemProbe>();
    auto* rawProbe = probe.get();
    setCoreSample(*rawProbe, 0, {0, 1});
    Domain::SystemModel model(std::move(probe));
    model.refresh();
    setCoreSample(*rawProbe, 1, {0, 1});
    model.refresh();
    setCoreSample(*rawProbe, 2, {0}); // cpu1 offline: one CPU online
    model.refresh();
    setCoreSample(*rawProbe, 3, {0});
    model.refresh();

    const auto publication = model.publication();
    EXPECT_TRUE(showCpuCoresTab(publication->snapshot.seenCoreIds, static_cast<std::size_t>(publication->snapshot.coreCount)));
    EXPECT_EQ(gridIdsFor(model), (std::vector<std::size_t>{0, 1}));
}

TEST(CpuCoreGridIdsTest, TabVisibilityFollowsSeenIdsElseTheCoreCount)
{
    EXPECT_FALSE(showCpuCoresTab({0}, 1));
    EXPECT_TRUE(showCpuCoresTab({0, 1}, 1));
    EXPECT_FALSE(showCpuCoresTab({}, 1));
    EXPECT_TRUE(showCpuCoresTab({}, 2));
}

TEST(CpuCoreGridIdsTest, WithoutSeenIdsEverySlotIsCharted)
{
    std::vector<std::size_t> ids{42}; // reused output: replaced, not appended to
    selectCpuCoreGridIds({}, 3, ids);
    EXPECT_EQ(ids, (std::vector<std::size_t>{0, 1, 2}));

    selectCpuCoreGridIds({1, 5}, 6, ids);
    EXPECT_EQ(ids, (std::vector<std::size_t>{1, 5}));
}

} // namespace
