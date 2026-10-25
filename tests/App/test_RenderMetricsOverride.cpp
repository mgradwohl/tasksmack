/// @file test_RenderMetricsOverride.cpp
/// @brief App::RenderMetricsOverride (#2923): the TASKSMACK_RENDER_METRICS parser, the demo script's
/// metrics= values, where an export lands beside a screenshot, and the CSV writer, whose file holds the
/// bytes the overlay's Copy CSV button puts on the clipboard (UI::RenderMetrics::toCsv()).

#include "App/RenderMetricsOverride.h"
#include "Core/Utf8Path.h"
#include "UI/RenderMetrics.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <system_error>

namespace
{

using App::RenderMetricsOverride::csvBesideScreenshot;
using App::RenderMetricsOverride::csvRowCount;
using App::RenderMetricsOverride::MetricsRequest;
using App::RenderMetricsOverride::metricsRequestName;
using App::RenderMetricsOverride::parse;
using App::RenderMetricsOverride::parseMetricsRequest;
using App::RenderMetricsOverride::writeCsv;

/// @p path's bytes, as written.
[[nodiscard]] std::string readAll(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

// ========== TASKSMACK_RENDER_METRICS ==========

TEST(RenderMetricsOverrideParseTest, UnsetAndBlankGiveNoFile)
{
    for (const char* value : {static_cast<const char*>(nullptr), "", "   ", "\t\r\n"})
    {
        SCOPED_TRACE(value == nullptr ? "(nullptr)" : value);
        EXPECT_FALSE(parse(value).has_value());
    }
}

TEST(RenderMetricsOverrideParseTest, ThePathIsKeptWithTheWhitespaceAroundItTrimmed)
{
    EXPECT_EQ(parse("perf-data/idle.csv"), std::filesystem::path("perf-data/idle.csv"));
    // #2170: every hook trims the same ASCII whitespace, so a Windows .env file's trailing CR is harmless.
    EXPECT_EQ(parse(" \tperf-data/idle.csv\r\n"), std::filesystem::path("perf-data/idle.csv"));
    // Spaces inside the name are the name's.
    EXPECT_EQ(parse("my runs/before resize.csv"), std::filesystem::path("my runs/before resize.csv"));
}

// The value is UTF-8, as every hook's is (#1648): decoded as such on Windows, whatever the code page.
TEST(RenderMetricsOverrideParseTest, ThePathIsDecodedAsUtf8)
{
    const std::optional<std::filesystem::path> path = parse("messungen/größe.csv");
    ASSERT_TRUE(path.has_value());
    EXPECT_EQ(Core::pathToUtf8(path.value_or(std::filesystem::path{})), "messungen/größe.csv");
}

// ========== metrics=<on|off|export> ==========

TEST(RenderMetricsOverrideTest, MetricsRequestsParseInAnyCase)
{
    EXPECT_EQ(parseMetricsRequest("on"), MetricsRequest::On);
    EXPECT_EQ(parseMetricsRequest("ON"), MetricsRequest::On);
    EXPECT_EQ(parseMetricsRequest("off"), MetricsRequest::Off);
    EXPECT_EQ(parseMetricsRequest("Off"), MetricsRequest::Off);
    EXPECT_EQ(parseMetricsRequest("export"), MetricsRequest::Export);
    EXPECT_EQ(parseMetricsRequest("EXPORT"), MetricsRequest::Export);
    for (const char* other : {"", "1", "true", "show", "csv", "on ", "exports"})
    {
        SCOPED_TRACE(other);
        EXPECT_FALSE(parseMetricsRequest(other).has_value());
    }
}

TEST(RenderMetricsOverrideTest, EveryMetricsRequestNameRoundTrips)
{
    for (const MetricsRequest request : {MetricsRequest::On, MetricsRequest::Off, MetricsRequest::Export})
    {
        const std::string_view name = metricsRequestName(request);
        EXPECT_FALSE(name.empty());
        EXPECT_EQ(parseMetricsRequest(name), request);
    }
}

// ========== Where an export lands beside a screenshot ==========

TEST(RenderMetricsOverrideTest, TheCsvBesideAScreenshotTakesItsStem)
{
    EXPECT_EQ(csvBesideScreenshot("shots/idle.png"), std::filesystem::path("shots") / "idle-metrics.csv");
    EXPECT_EQ(csvBesideScreenshot("idle.png"), std::filesystem::path("idle-metrics.csv"));
    // A sequence's frames are <stem>-NNNN.png beside the path; the CSV sits beside them, named once.
    EXPECT_EQ(csvBesideScreenshot("out/run.png"), std::filesystem::path("out") / "run-metrics.csv");
    // A non-ASCII stem is kept (#1648).
    EXPECT_EQ(Core::pathToUtf8(csvBesideScreenshot(Core::utf8ToPath("снимок/экран.png")).filename()), "экран-metrics.csv");
}

TEST(RenderMetricsOverrideTest, CsvRowCountIsTheLinesLessTheHeader)
{
    EXPECT_EQ(csvRowCount(""), 0U);
    EXPECT_EQ(csvRowCount("timestamp_us,scenario,frame_id,publication_id,chart,vertices,cpu_us\n"), 0U);
    EXPECT_EQ(csvRowCount("h\n1,2\n3,4\n"), 2U);
}

// ========== The CSV writer ==========

/// A fresh directory with a non-ASCII name under the system's temporary directory (the CSV path may
/// have one, #1648), removed with everything in it when the test ends. The RenderMetrics singleton
/// is reset around each test, as tests/UI/test_RenderMetrics.cpp resets it.
class RenderMetricsCsvFileTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        UI::RenderMetrics::get().setEnabled(false);
        UI::RenderMetrics::get().setEnabled(true);
        std::random_device random;
        constexpr int MAX_ATTEMPTS = 100;
        for (int attempt = 0; attempt < MAX_ATTEMPTS; ++attempt)
        {
            m_Dir = std::filesystem::temp_directory_path() / Core::utf8ToPath("tasksmack_metrics_größe_" + std::to_string(random()));
            std::error_code ec;
            if (std::filesystem::create_directory(m_Dir, ec) && !ec)
            {
                return;
            }
        }
        FAIL() << "Failed to create a temp directory";
    }

    void TearDown() override
    {
        UI::RenderMetrics::get().setEnabled(false);
        UI::RenderMetrics::get().setScenario("");
        std::error_code ec;
        std::filesystem::remove_all(m_Dir, ec);
    }

    std::filesystem::path m_Dir;
};

// The file holds the overlay's export byte for byte: what the hook writes is what Copy CSV would have
// put on the clipboard for the same frame, quoting and "\n" line ends included.
TEST_F(RenderMetricsCsvFileTest, TheFileIsTheClipboardExportByteForByte)
{
    UI::RenderMetrics& metrics = UI::RenderMetrics::get();
    metrics.setScenario("resize, tab \"A\"");
    metrics.beginFrame(1);
    metrics.record("##CPUHistory", 120, 42.35, 1);
    metrics.record("##MemHistory", 60, 7.0, 1);
    metrics.record("##Net,Chart", 12, 1.5, 1);
    metrics.beginFrame(2); // Publish frame 1

    const std::string csv = metrics.toCsv();
    ASSERT_TRUE(csv.starts_with("timestamp_us,scenario,frame_id,publication_id,chart,vertices,cpu_us\n"));
    ASSERT_EQ(csvRowCount(csv), 3U);

    const std::filesystem::path file = m_Dir / Core::utf8ToPath("größe.csv");
    const auto written = writeCsv(file, csv);
    ASSERT_TRUE(written.has_value()) << written.error();
    EXPECT_EQ(readAll(file), csv);
    EXPECT_NE(csv.find("\"resize, tab \"\"A\"\"\",1,1,##CPUHistory,120,42.4\n"), std::string::npos);
    EXPECT_NE(csv.find(",\"##Net,Chart\",12,1.5\n"), std::string::npos);
    EXPECT_EQ(csv.find('\r'), std::string::npos) << "the line ends are the export's, not the platform's";
}

TEST_F(RenderMetricsCsvFileTest, AnExistingFileIsReplaced)
{
    const std::filesystem::path file = m_Dir / "metrics.csv";
    ASSERT_TRUE(writeCsv(file, "old,longer,content\n1,2,3\n4,5,6\n").has_value());
    const std::string csv = UI::RenderMetrics::get().toCsv(); // header only: nothing published
    ASSERT_TRUE(writeCsv(file, csv).has_value());
    EXPECT_EQ(readAll(file), csv);
}

TEST_F(RenderMetricsCsvFileTest, AMissingDirectoryIsAnErrorNamingTheFile)
{
    const std::filesystem::path file = m_Dir / "missing" / "metrics.csv";
    const auto written = writeCsv(file, "h\n");
    ASSERT_FALSE(written.has_value());
    EXPECT_NE(written.error().find("metrics.csv"), std::string::npos) << written.error();
    EXPECT_FALSE(std::filesystem::exists(file));
}

} // namespace
