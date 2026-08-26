#pragma once

#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace UI
{

/// One chart's geometry and CPU cost for a single frame, captured by HistoryChart.
struct ChartRenderSample
{
    std::string id;
    int vertices = 0;
    int indices = 0;
    double micros = 0.0;
};

/// Frame-scoped registry of per-chart render cost (vertices, indices, CPU time).
///
/// HistoryChart records a sample per chart per frame while capture is enabled; the overlay
/// (renderOverlay) displays the totals for the last completed frame alongside ImGui's
/// io.MetricsRenderVertices/Indices. Recording is a no-op when disabled, so shipping builds
/// pay only a branch per chart. Single-threaded: all access happens on the UI thread.
class RenderMetrics
{
  public:
    [[nodiscard]] static RenderMetrics& get()
    {
        static RenderMetrics instance;
        return instance;
    }

    [[nodiscard]] bool enabled() const noexcept
    {
        return m_Enabled;
    }

    void setEnabled(bool enabled) noexcept
    {
        m_Enabled = enabled;
        if (!enabled)
        {
            m_Current.clear();
            m_LastFrame.clear();
            m_CurrentFrame = -1;
        }
    }

    /// Record one chart's cost for the given frame. When frameIndex advances, the previous
    /// frame's samples become the "last frame" snapshot shown by the overlay.
    void record(std::string_view id, int vertices, int indices, double micros, int frameIndex)
    {
        if (!m_Enabled)
        {
            return;
        }

        if (frameIndex != m_CurrentFrame)
        {
            m_LastFrame = std::move(m_Current);
            m_Current.clear();
            m_CurrentFrame = frameIndex;
        }

        m_Current.push_back({.id = std::string(id), .vertices = vertices, .indices = indices, .micros = micros});
    }

    [[nodiscard]] const std::vector<ChartRenderSample>& lastFrame() const noexcept
    {
        return m_LastFrame;
    }

    /// Serialize the last completed frame as CSV (header + one row per chart) for
    /// clipboard export into profiling notes or spreadsheets.
    [[nodiscard]] std::string toCsv() const
    {
        std::string csv = "chart,vertices,indices,cpu_us\n";
        for (const auto& sample : m_LastFrame)
        {
            csv += std::format("{},{},{},{:.1f}\n", sample.id, sample.vertices, sample.indices, sample.micros);
        }
        return csv;
    }

    /// Draw the "Render Metrics" overlay window. Enables capture while *open is true.
    void renderOverlay(bool* open);

  private:
    std::vector<ChartRenderSample> m_Current;
    std::vector<ChartRenderSample> m_LastFrame;
    int m_CurrentFrame = -1;
    bool m_Enabled = false;
};

} // namespace UI
