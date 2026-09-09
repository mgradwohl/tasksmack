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

    metrics.beginFrame(1);
    metrics.record("##Chart", 100, 150, 12.5, 1);
    metrics.beginFrame(2);
    metrics.record("##Chart", 100, 150, 12.5, 2);

    EXPECT_TRUE(metrics.lastFrame().empty());
}

TEST_F(RenderMetricsTest, SamplesBecomeLastFrameWhenFrameAdvances)
{
    auto& metrics = RenderMetrics::get();

    metrics.beginFrame(1);
    metrics.record("##A", 10, 20, 1.0, 1);
    metrics.record("##B", 30, 40, 2.0, 1);
    EXPECT_TRUE(metrics.lastFrame().empty()); // Frame 1 still in progress

    metrics.beginFrame(2); // Frame 2 starts; frame 1 published
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

    metrics.beginFrame(5);
    metrics.record("##A", 1, 2, 0.1, 5);
    metrics.record("##B", 3, 4, 0.2, 5);
    metrics.record("##C", 5, 6, 0.3, 5);
    metrics.beginFrame(6);

    EXPECT_EQ(metrics.lastFrame().size(), 3U);
}

TEST_F(RenderMetricsTest, SetEnabledFalseClearsAllState)
{
    auto& metrics = RenderMetrics::get();

    metrics.beginFrame(1);
    metrics.record("##A", 10, 20, 1.0, 1);
    metrics.beginFrame(2); // Publish frame 1
    ASSERT_FALSE(metrics.lastFrame().empty());

    metrics.setEnabled(false);
    EXPECT_TRUE(metrics.lastFrame().empty());
    EXPECT_FALSE(metrics.enabled());
}

// A frame with zero chart activity must publish an *empty* last-frame snapshot, not retain the
// previous chart-bearing frame's totals -- this is the exact bug #875 reports: record() only
// advanced the frame boundary when a chart actually recorded, so a zero-chart frame (no record()
// calls at all) left stale data from whenever a chart last rendered.
TEST_F(RenderMetricsTest, ZeroChartFrameClearsLastFramePublication)
{
    auto& metrics = RenderMetrics::get();

    metrics.beginFrame(1);
    metrics.record("##A", 10, 20, 1.0, 1);
    metrics.beginFrame(2); // Publish frame 1 (has ##A)
    ASSERT_FALSE(metrics.lastFrame().empty());

    // Frame 2 has zero chart activity: no record() call at all, only beginFrame() advancing.
    metrics.beginFrame(3); // Publish frame 2 (empty)
    EXPECT_TRUE(metrics.lastFrame().empty());
}

TEST_F(RenderMetricsTest, BeginFrameIsIdempotentWithinSameFrame)
{
    auto& metrics = RenderMetrics::get();

    metrics.beginFrame(1);
    metrics.record("##A", 10, 20, 1.0, 1);
    metrics.beginFrame(1); // Same frame index again -- must not roll over
    metrics.record("##B", 30, 40, 2.0, 1);
    metrics.beginFrame(2); // Now publish frame 1

    ASSERT_EQ(metrics.lastFrame().size(), 2U);
    EXPECT_EQ(metrics.lastFrame()[0].id, "##A");
    EXPECT_EQ(metrics.lastFrame()[1].id, "##B");
}

TEST_F(RenderMetricsTest, RecordFallsBackToBeginFrameIfNotYetCalled)
{
    // A chart that records before beginFrame() has run for its frame (e.g. capture enabled
    // mid-frame) must not misattribute its sample to a stale frame's bucket.
    auto& metrics = RenderMetrics::get();

    metrics.record("##A", 10, 20, 1.0, 7);
    metrics.beginFrame(8); // Publish frame 7

    ASSERT_EQ(metrics.lastFrame().size(), 1U);
    EXPECT_EQ(metrics.lastFrame()[0].id, "##A");
}

// ========== Scenario label ==========

TEST_F(RenderMetricsTest, ScenarioDefaultsToEmpty)
{
    EXPECT_TRUE(RenderMetrics::get().scenario().empty());
}

TEST_F(RenderMetricsTest, SetScenarioRoundTrips)
{
    auto& metrics = RenderMetrics::get();
    metrics.setScenario("idle");
    EXPECT_EQ(metrics.scenario(), "idle");
}

TEST_F(RenderMetricsTest, SetEnabledFalseDoesNotClearScenario)
{
    // The scenario label identifies a whole capture session, not a single frame -- disabling
    // capture (e.g. closing the overlay) shouldn't discard it.
    auto& metrics = RenderMetrics::get();
    metrics.setScenario("resize-stress");
    metrics.setEnabled(false);
    EXPECT_EQ(metrics.scenario(), "resize-stress");
}

// ========== CSV export ==========

TEST_F(RenderMetricsTest, ToCsvContainsHeaderWhenEmpty)
{
    EXPECT_EQ(RenderMetrics::get().toCsv(), "timestamp_us,scenario,frame_id,publication_id,chart,vertices,indices,cpu_us\n");
}

TEST_F(RenderMetricsTest, ToCsvSerializesLastFrameRows)
{
    auto& metrics = RenderMetrics::get();
    metrics.setScenario("test-scenario");

    metrics.beginFrame(1);
    metrics.record("##CPUHistory", 120, 180, 42.35, 1);
    metrics.record("##MemHistory", 60, 90, 7.0, 1);
    metrics.beginFrame(2); // Publish frame 1

    const std::string csv = metrics.toCsv();
    const std::string header = "timestamp_us,scenario,frame_id,publication_id,chart,vertices,indices,cpu_us\n";
    ASSERT_TRUE(csv.starts_with(header));
    EXPECT_NE(csv.find(",test-scenario,1,1,##CPUHistory,120,180,42.4\n"), std::string::npos);
    EXPECT_NE(csv.find(",test-scenario,1,1,##MemHistory,60,90,7.0\n"), std::string::npos);
}

TEST_F(RenderMetricsTest, ToCsvPublicationIdIncrementsPerPublishedFrame)
{
    auto& metrics = RenderMetrics::get();

    metrics.beginFrame(1);
    metrics.record("##A", 1, 1, 1.0, 1);
    metrics.beginFrame(2); // Publishes frame 1 as publication 1
    EXPECT_NE(metrics.toCsv().find(",1,1,##A,"), std::string::npos);

    metrics.record("##B", 2, 2, 2.0, 2);
    metrics.beginFrame(3); // Publishes frame 2 as publication 2
    EXPECT_NE(metrics.toCsv().find(",2,2,##B,"), std::string::npos);
}

// Scenario is free-form text typed into the overlay; a comma, quote, or newline in it must not
// silently split into extra columns or corrupt the row (RFC 4180-style quoting).
TEST_F(RenderMetricsTest, ToCsvEscapesScenarioContainingComma)
{
    auto& metrics = RenderMetrics::get();
    metrics.setScenario("resize, tab A");

    metrics.beginFrame(1);
    metrics.record("##A", 1, 2, 0.5, 1);
    metrics.beginFrame(2); // Publish frame 1

    const std::string csv = metrics.toCsv();
    EXPECT_NE(csv.find("\"resize, tab A\",1,1,##A,1,2,0.5\n"), std::string::npos);
}

TEST_F(RenderMetricsTest, ToCsvEscapesScenarioContainingQuote)
{
    auto& metrics = RenderMetrics::get();
    metrics.setScenario(R"(say "hi")");

    metrics.beginFrame(1);
    metrics.record("##A", 1, 2, 0.5, 1);
    metrics.beginFrame(2); // Publish frame 1

    const std::string csv = metrics.toCsv();
    EXPECT_NE(csv.find(R"("say ""hi""",1,1,##A,1,2,0.5)"), std::string::npos);
}

TEST_F(RenderMetricsTest, ToCsvDoesNotQuoteOrdinaryScenario)
{
    auto& metrics = RenderMetrics::get();
    metrics.setScenario("idle");

    metrics.beginFrame(1);
    metrics.record("##A", 1, 2, 0.5, 1);
    metrics.beginFrame(2); // Publish frame 1

    // No quotes should appear for a field that needs none.
    EXPECT_NE(metrics.toCsv().find(",idle,1,1,##A,"), std::string::npos);
}

} // namespace
} // namespace UI
