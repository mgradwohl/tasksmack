/// @file test_ChartSourceGenerations.cpp
/// @brief The System Metrics panel's chart data generation per source (#843): a storage, GPU or
/// process-history adoption leaves the CPU and memory charts' caches alone.

#include "App/Panels/ChartSourceGenerations.h"
#include "UI/ChartWidgets.h"

#include <gtest/gtest.h>

#include <cstdint>

namespace App
{
namespace
{

/// How often a chart's generation changed: each change makes its caches reduce and rescan again.
class ChangeCounter
{
  public:
    void observe(std::uint64_t generation) noexcept
    {
        if (generation != m_Last)
        {
            ++m_Changes;
            m_Last = generation;
        }
    }
    [[nodiscard]] int changes() const noexcept
    {
        return m_Changes;
    }

  private:
    std::uint64_t m_Last = 0;
    int m_Changes = 0;
};

TEST(ChartSourceGenerationsTest, NothingAdoptedIsGenerationZero)
{
    const ChartSourceGenerations generations;
    EXPECT_EQ(generations.system(), 0U);
    EXPECT_EQ(generations.storage(), 0U);
    EXPECT_EQ(generations.gpu(), 0U);
    EXPECT_EQ(generations.processHistories(), 0U);
}

TEST(ChartSourceGenerationsTest, EachAdoptionMovesOnlyItsOwnSource)
{
    ChartSourceGenerations generations;
    generations.systemAdopted();
    generations.storageAdopted();
    generations.gpuAdopted();
    generations.processHistoriesAdopted();
    const std::uint64_t system = generations.system();
    const std::uint64_t storage = generations.storage();
    const std::uint64_t gpu = generations.gpu();
    const std::uint64_t process = generations.processHistories();
    EXPECT_NE(system, 0U);

    generations.storageAdopted();
    generations.gpuAdopted();
    generations.processHistoriesAdopted();
    EXPECT_EQ(generations.system(), system); // the CPU and memory charts keep their caches
    EXPECT_NE(generations.storage(), storage);
    EXPECT_NE(generations.gpu(), gpu);
    EXPECT_NE(generations.processHistories(), process);

    generations.systemAdopted();
    EXPECT_NE(generations.system(), system);
}

TEST(ChartSourceGenerationsTest, ACombinedGenerationChangesWhenEitherSourceDoes)
{
    ChartSourceGenerations generations;
    generations.systemAdopted();
    generations.processHistoriesAdopted();
    std::uint64_t last = ChartSourceGenerations::combined(generations.system(), generations.processHistories());
    for (int i = 0; i < 4; ++i)
    {
        // Either source, in either order, and the older one too: the newest generation is new.
        if (i % 2 == 0)
        {
            generations.systemAdopted();
        }
        else
        {
            generations.processHistoriesAdopted();
        }
        const std::uint64_t now = ChartSourceGenerations::combined(generations.system(), generations.processHistories());
        EXPECT_NE(now, last) << i;
        last = now;
    }
    EXPECT_EQ(ChartSourceGenerations::combined(0, 0), 0U);
}

// The improvement itself: one refresh adopts a system, storage, GPU and process-history publication.
// With one generation for every source (before #843's follow-up) the CPU and memory charts' caches
// were invalidated four times per refresh; with one per source, once.
TEST(ChartSourceGenerationsTest, ARefreshInvalidatesTheCpuAndMemoryChartsOnce)
{
    constexpr int REFRESHES = 60;
    std::uint64_t shared = 0; // the single generation the panel used to keep
    ChartSourceGenerations generations;
    ChangeCounter sharedCpu;
    ChangeCounter cpu;
    ChangeCounter storage;
    ChangeCounter power;
    for (int refresh = 0; refresh < REFRESHES; ++refresh)
    {
        // The panel adopts publications as they arrive, a frame or more apart; the charts draw between.
        const auto frame = [&]
        {
            sharedCpu.observe(shared);
            cpu.observe(generations.system());
            storage.observe(generations.storage());
            power.observe(ChartSourceGenerations::combined(generations.system(), generations.processHistories()));
        };
        generations.systemAdopted();
        shared = UI::Widgets::nextChartDataGeneration();
        frame();
        generations.storageAdopted();
        shared = UI::Widgets::nextChartDataGeneration();
        frame();
        generations.gpuAdopted();
        shared = UI::Widgets::nextChartDataGeneration();
        frame();
        generations.processHistoriesAdopted();
        shared = UI::Widgets::nextChartDataGeneration();
        frame();
    }
    EXPECT_EQ(sharedCpu.changes(), 4 * REFRESHES);
    EXPECT_EQ(cpu.changes(), REFRESHES);
    EXPECT_EQ(storage.changes(), REFRESHES);
    EXPECT_EQ(power.changes(), 2 * REFRESHES); // it draws two sources
}

} // namespace
} // namespace App
