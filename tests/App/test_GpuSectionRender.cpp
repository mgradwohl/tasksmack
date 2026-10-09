/// @file test_GpuSectionRender.cpp
/// @brief The System Metrics GPU tab (GpuSection, #1545), run headless against hand-built GPU
/// publications: its empty states, one chart pair per GPU, each sensor series drawn only when the GPU
/// reports it, a GPU missing from a read keeping its slot, the smoothing, and a chart's tooltip.

#include "App/Panels/GpuSection.h"
#include "Domain/GPUModel.h"
#include "Domain/GPUSnapshot.h"
#include "Domain/SamplingConfig.h"
#include "Domain/SharedHistory.h"
#include "Platform/GPUTypes.h"

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <implot.h>
#include <implot_internal.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace App
{
namespace
{

constexpr float DISPLAY_WIDTH = 1600.0F;
constexpr float DISPLAY_HEIGHT = 1200.0F;
constexpr std::size_t HISTORY_POINTS = 30;
constexpr std::chrono::milliseconds REFRESH{Domain::Sampling::REFRESH_INTERVAL_DEFAULT_MS};

using SeriesLabels = std::set<std::string>;

/// A published-history view that owns @p values, like SharedHistoryBuffer::view() hands out.
template<typename T> [[nodiscard]] Domain::HistoryView<T> viewOf(std::vector<T> values)
{
    auto owner = std::make_shared<const std::vector<T>>(std::move(values));
    const std::span<const T> span(*owner);
    return Domain::HistoryView<T>(std::move(owner), span);
}

/// HISTORY_POINTS timestamps one second apart, the newest at now (historyFrameNowSeconds()'s clock).
[[nodiscard]] std::vector<double> timestampsToNow()
{
    const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    std::vector<double> times;
    times.reserve(HISTORY_POINTS);
    for (std::size_t i = 0; i < HISTORY_POINTS; ++i)
    {
        times.push_back(now - static_cast<double>(HISTORY_POINTS - 1 - i));
    }
    return times;
}

template<typename T> [[nodiscard]] std::vector<T> constant(T value)
{
    return std::vector<T>(HISTORY_POINTS, value);
}

/// Every sensor a GPU can report, on.
[[nodiscard]] Platform::GPUCapabilities allSensors()
{
    Platform::GPUCapabilities caps;
    caps.hasTemperature = true;
    caps.hasPowerMetrics = true;
    caps.hasClockSpeeds = true;
    caps.hasFanSpeed = true;
    caps.hasEncoderDecoder = true;
    return caps;
}

/// One GPU's enumeration entry, latest snapshot and published history, every series populated.
void addGpu(Domain::GPUPublication& publication, const std::string& id, const std::string& name)
{
    Platform::GPUInfo info;
    info.id = id;
    info.name = name;
    info.vendor = "NVIDIA";
    publication.gpuInfo.push_back(info);

    Domain::GPUSnapshot snap;
    snap.gpuId = id;
    snap.name = name;
    snap.vendor = "NVIDIA";
    snap.utilizationPercent = 42.0;
    snap.memoryUtilPercent = 25.0;
    snap.memoryUsedBytes = std::uint64_t{2} << 30U;
    snap.memoryTotalBytes = std::uint64_t{8} << 30U;
    snap.temperatureC = 61;
    snap.powerDrawWatts = 120.0;
    snap.powerLimitWatts = 250.0;
    snap.gpuClockMHz = 1800;
    snap.encoderUtilPercent = 10.0;
    snap.decoderUtilPercent = 5.0;
    snap.fanSpeedPercent = 40;
    snap.fanSpeedAvailable = true;
    publication.snapshots.push_back(snap);

    Domain::GPUPublishedHistory history;
    history.timestamps = viewOf(timestampsToNow());
    history.memoryUsedBytes = viewOf(constant<std::uint64_t>(std::uint64_t{2} << 30U));
    history.memoryTotalBytes = viewOf(constant<std::uint64_t>(std::uint64_t{8} << 30U));
    history.utilization = viewOf(constant(42.0F));
    history.memoryPercent = viewOf(constant(25.0F));
    history.gpuClock = viewOf(constant(1800.0F));
    history.encoder = viewOf(constant(10.0F));
    history.decoder = viewOf(constant(5.0F));
    history.temperature = viewOf(constant(61.0F));
    history.power = viewOf(constant(120.0F));
    history.fanSpeed = viewOf(constant(40.0F));
    publication.histories.emplace(id, std::move(history));
}

/// Whether @p labels has one that starts with @p prefix (the scaled labels name their scale).
[[nodiscard]] bool hasPrefix(const SeriesLabels& labels, std::string_view prefix)
{
    return std::ranges::any_of(labels, [prefix](const std::string& label) { return label.starts_with(prefix); });
}

class GpuSectionRenderTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_ImGui = ImGui::CreateContext();
        m_ImPlot = ImPlot::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.DisplaySize = ImVec2(DISPLAY_WIDTH, DISPLAY_HEIGHT);
        io.DeltaTime = 1.0F / 60.0F;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
        io.Fonts->AddFontDefault();
    }

    void TearDown() override
    {
        ImPlot::DestroyContext(m_ImPlot);
        ImGui::DestroyContext(m_ImGui);
    }

    /// The tab's render context for @p publication, wired to this fixture's per-GPU state.
    [[nodiscard]] GpuSection::RenderContext context(const Domain::GPUPublication* publication)
    {
        return GpuSection::RenderContext{
            .publication = publication,
            .chartDataGeneration = 1,
            .maxHistorySeconds = 300.0,
            .lastDeltaSeconds = 1.0F,
            .refreshInterval = REFRESH,
            .smoothedGPUs = &m_Smoothed,
            .fill = nullptr,
            .cache = &m_Cache,
        };
    }

    /// One frame of a window filling the display, drawing the tab, and the text it drew (ImGui's
    /// own logging, which also turns clipping off).
    [[nodiscard]] static std::string renderAndCapture(GpuSection::RenderContext& ctx)
    {
        std::string captured;
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
        ImGui::SetNextWindowSize(ImVec2(DISPLAY_WIDTH, DISPLAY_HEIGHT));
        ImGui::Begin("System", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
        ImGui::LogToBuffer();
        GpuSection::renderGpuSection(ctx);
        captured = GImGui->LogBuffer.c_str();
        ImGui::LogFinish();
        ImGui::End();
        ImGui::Render();
        return captured;
    }

    /// Every plot ImPlot has seen in this context, each as the set of its series' legend labels.
    [[nodiscard]] static std::vector<SeriesLabels> plotsDrawn()
    {
        std::vector<SeriesLabels> plots;
        ImPlotContext& context = *ImPlot::GetCurrentContext();
        for (int i = 0; i < context.Plots.GetBufSize(); ++i)
        {
            ImPlotPlot* plot = context.Plots.GetByIndex(i);
            if (plot == nullptr)
            {
                continue;
            }
            SeriesLabels labels;
            for (int item = 0; item < plot->Items.GetLegendCount(); ++item)
            {
                labels.emplace(plot->Items.GetLegendLabel(item));
            }
            plots.push_back(std::move(labels));
        }
        return plots;
    }

    std::unordered_map<std::string, GpuSection::SmoothedGPU> m_Smoothed;
    GpuSection::FrameCache m_Cache;

  private:
    ImGuiContext* m_ImGui = nullptr;
    ImPlotContext* m_ImPlot = nullptr;
};

TEST_F(GpuSectionRenderTest, EmptyStatesSayWhy)
{
    GpuSection::RenderContext none = context(nullptr);
    EXPECT_TRUE(renderAndCapture(none).contains("GPU monitoring is not available"));

    Domain::GPUPublication noDevices;
    noDevices.gpuInfoKnown = true;
    GpuSection::RenderContext found = context(&noDevices);
    EXPECT_TRUE(renderAndCapture(found).contains("No GPU detected"));

    const Domain::GPUPublication failedEnumeration; // gpuInfoKnown false: empty means "couldn't look"
    GpuSection::RenderContext failed = context(&failedEnumeration);
    EXPECT_TRUE(renderAndCapture(failed).contains("GPU monitoring is not available"));
    EXPECT_TRUE(plotsDrawn().empty());
}

TEST_F(GpuSectionRenderTest, OneGpuDrawsItsCoreAndThermalCharts)
{
    Domain::GPUPublication publication;
    publication.gpuInfoKnown = true;
    publication.capabilities = allSensors();
    addGpu(publication, "gpu0", "NVIDIA GeForce RTX 4080");
    GpuSection::RenderContext ctx = context(&publication);
    static_cast<void>(renderAndCapture(ctx));
    const std::string text = renderAndCapture(ctx);

    EXPECT_TRUE(text.contains("GPU Monitoring (1 GPU)")) << text;
    EXPECT_TRUE(text.contains("NVIDIA GeForce RTX 4080")) << text;
    EXPECT_TRUE(text.contains("GPU Core & Video")) << text;
    EXPECT_TRUE(text.contains("Thermal & Power")) << text;
    const auto plots = plotsDrawn();
    ASSERT_EQ(plots.size(), 2U);
    EXPECT_TRUE(plots[0].contains("Utilization"));
    EXPECT_TRUE(plots[0].contains("Memory"));
    EXPECT_TRUE(plots[0].contains("Encoder"));
    EXPECT_TRUE(plots[0].contains("Decoder"));
    EXPECT_TRUE(hasPrefix(plots[0], "Clock (% of "));
    EXPECT_TRUE(hasPrefix(plots[1], "Temperature (% of "));
    EXPECT_TRUE(hasPrefix(plots[1], "Power (% of "));
    EXPECT_TRUE(plots[1].contains("Fan"));
}

TEST_F(GpuSectionRenderTest, SeriesFollowWhatTheGpuReports)
{
    Domain::GPUPublication publication;
    publication.gpuInfoKnown = true;
    publication.capabilities = allSensors();
    addGpu(publication, "gpu0", "Integrated GPU");
    // This adapter reports no sensors of its own, although the probe as a whole does (#1040).
    publication.gpuInfo[0].sensorCapabilities = Platform::GPUCapabilities{};
    GpuSection::RenderContext ctx = context(&publication);
    static_cast<void>(renderAndCapture(ctx));
    const std::string text = renderAndCapture(ctx);

    const auto plots = plotsDrawn();
    ASSERT_EQ(plots.size(), 1U); // no thermal chart at all
    EXPECT_EQ(plots[0], (SeriesLabels{"Utilization", "Memory"}));
    EXPECT_FALSE(text.contains("Thermal & Power")) << text;
}

TEST_F(GpuSectionRenderTest, SeveralGpusAndOneMissingFromTheRead)
{
    Domain::GPUPublication publication;
    publication.gpuInfoKnown = true;
    publication.capabilities = allSensors();
    addGpu(publication, "gpu0", "First GPU");
    addGpu(publication, "gpu1", "Second GPU");
    GpuSection::RenderContext ctx = context(&publication);
    static_cast<void>(renderAndCapture(ctx));
    EXPECT_TRUE(renderAndCapture(ctx).contains("GPU Monitoring (2 GPUs)"));
    EXPECT_EQ(plotsDrawn().size(), 4U); // two charts per GPU

    // The second GPU missed this read: it keeps its slot, saying so, instead of disappearing (#1163).
    publication.snapshots.pop_back();
    ++publication.version;
    const std::string text = renderAndCapture(ctx);
    EXPECT_TRUE(text.contains("Second GPU")) << text;
    EXPECT_TRUE(text.contains("No reading")) << text;

    // Every GPU missed it: the tab says so and still lists them.
    publication.snapshots.clear();
    ++publication.version;
    const std::string none = renderAndCapture(ctx);
    EXPECT_TRUE(none.contains("2 GPUs were detected, but the latest reading returned no data.")) << none;
    EXPECT_TRUE(none.contains("First GPU")) << none;
}

TEST_F(GpuSectionRenderTest, SmoothingSnapsFirstThenEasesAndSkipsUnreadValues)
{
    Domain::GPUPublication publication;
    publication.gpuInfoKnown = true;
    publication.capabilities = allSensors();
    addGpu(publication, "gpu0", "GPU");
    GpuSection::RenderContext ctx = context(&publication);

    Domain::GPUSnapshot snap = publication.snapshots[0];
    GpuSection::updateSmoothedGPU("gpu0", snap, ctx);
    const GpuSection::SmoothedGPU& smoothed = m_Smoothed.at("gpu0");
    EXPECT_TRUE(smoothed.initialized);
    EXPECT_DOUBLE_EQ(smoothed.utilizationPercent, 42.0); // the first sample snaps

    snap.utilizationPercent = 82.0;
    GpuSection::updateSmoothedGPU("gpu0", snap, ctx);
    EXPECT_GT(smoothed.utilizationPercent, 42.0); // later samples ease toward the reading
    EXPECT_LT(smoothed.utilizationPercent, 82.0);

    // A sample that couldn't read the temperature leaves it where it was (#1111).
    const double temperatureBefore = smoothed.temperatureC;
    snap.temperatureAvailable = false;
    snap.temperatureC = 0;
    GpuSection::updateSmoothedGPU("gpu0", snap, ctx);
    EXPECT_DOUBLE_EQ(smoothed.temperatureC, temperatureBefore);
}

TEST_F(GpuSectionRenderTest, AHoveredChartShowsItsTooltip)
{
    Domain::GPUPublication publication;
    publication.gpuInfoKnown = true;
    publication.capabilities = allSensors();
    addGpu(publication, "gpu0", "GPU");
    GpuSection::RenderContext ctx = context(&publication);
    static_cast<void>(renderAndCapture(ctx));
    static_cast<void>(renderAndCapture(ctx));

    ImPlotContext& plots = *ImPlot::GetCurrentContext();
    ASSERT_GT(plots.Plots.GetBufSize(), 0);
    const ImRect rect = plots.Plots.GetByIndex(0)->PlotRect;
    ImGui::GetIO().AddMousePosEvent(rect.Max.x - 2.0F, (rect.Min.y + rect.Max.y) * 0.5F);
    static_cast<void>(renderAndCapture(ctx));
    const std::string text = renderAndCapture(ctx);
    const ImGuiWindow* tooltip = ImGui::FindWindowByName("##Tooltip_00");
    ASSERT_NE(tooltip, nullptr);
    EXPECT_TRUE(tooltip->Active);
    EXPECT_TRUE(text.contains("Utilization")) << text;
}

} // namespace
} // namespace App
