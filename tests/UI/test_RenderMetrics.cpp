#include "UI/RenderMetrics.h"

#include <gtest/gtest.h>

#include <string>

namespace UI
{
namespace
{

class RenderMetricsTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        // Reset the singleton to a clean, enabled state for each test.
        RenderMetrics::get().setEnabled(false);
        RenderMetrics::get().setEnabled(true);
    }

    void TearDown() override
    {
        RenderMetrics::get().setEnabled(false);
    }
};

// ========== Recording & frame rollover ==========

TEST_F(RenderMetricsTest, RecordIsNoOpWhenDisabled)
{
    auto& metrics = RenderMetrics::get();
    metrics.setEnabled(false);

    metrics.record("##Chart", 100, 150, 12.5, 1);
    metrics.record("##Chart", 100, 150, 12.5, 2);

    EXPECT_TRUE(metrics.lastFrame().empty());
}

TEST_F(RenderMetricsTest, SamplesBecomeLastFrameWhenFrameAdvances)
{
    auto& metrics = RenderMetrics::get();

    metrics.record("##A", 10, 20, 1.0, 1);
    metrics.record("##B", 30, 40, 2.0, 1);
    EXPECT_TRUE(metrics.lastFrame().empty()); // Frame 1 still in progress

    metrics.record("##A", 11, 21, 1.1, 2); // Frame 2 starts; frame 1 published
    ASSERT_EQ(metrics.lastFrame().size(), 2U);
    EXPECT_EQ(metrics.lastFrame()[0].id, "##A");
    EXPECT_EQ(metrics.lastFrame()[0].vertices, 10);
    EXPECT_EQ(metrics.lastFrame()[0].indices, 20);
    EXPECT_DOUBLE_EQ(metrics.lastFrame()[0].micros, 1.0);
    EXPECT_EQ(metrics.lastFrame()[1].id, "##B");
}

TEST_F(RenderMetricsTest, MultipleRecordsInSameFrameAccumulate)
{
    auto& metrics = RenderMetrics::get();

    metrics.record("##A", 1, 2, 0.1, 5);
    metrics.record("##B", 3, 4, 0.2, 5);
    metrics.record("##C", 5, 6, 0.3, 5);
    metrics.record("##Next", 0, 0, 0.0, 6);

    EXPECT_EQ(metrics.lastFrame().size(), 3U);
}

TEST_F(RenderMetricsTest, SetEnabledFalseClearsAllState)
{
    auto& metrics = RenderMetrics::get();

    metrics.record("##A", 10, 20, 1.0, 1);
    metrics.record("##A", 10, 20, 1.0, 2); // Publish frame 1
    ASSERT_FALSE(metrics.lastFrame().empty());

    metrics.setEnabled(false);
    EXPECT_TRUE(metrics.lastFrame().empty());
    EXPECT_FALSE(metrics.enabled());
}

// ========== CSV export ==========

TEST_F(RenderMetricsTest, ToCsvContainsHeaderWhenEmpty)
{
    EXPECT_EQ(RenderMetrics::get().toCsv(), "chart,vertices,indices,cpu_us\n");
}

TEST_F(RenderMetricsTest, ToCsvSerializesLastFrameRows)
{
    auto& metrics = RenderMetrics::get();

    metrics.record("##CPUHistory", 120, 180, 42.35, 1);
    metrics.record("##MemHistory", 60, 90, 7.0, 1);
    metrics.record("##Next", 0, 0, 0.0, 2); // Publish frame 1

    const std::string csv = metrics.toCsv();
    EXPECT_EQ(csv,
              "chart,vertices,indices,cpu_us\n"
              "##CPUHistory,120,180,42.4\n"
              "##MemHistory,60,90,7.0\n");
}

} // namespace
} // namespace UI
