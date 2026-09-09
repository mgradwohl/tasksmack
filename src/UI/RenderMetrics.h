#pragma once

#include <chrono>
#include <cstdint>
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
///
/// Frame publication is explicit: the per-frame driver calls beginFrame() once every frame
/// (regardless of whether any chart ends up recording), so a frame with zero chart activity
/// correctly publishes an empty "last frame" instead of leaving whatever the previous
/// chart-bearing frame's totals were in place (see #875).
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
            m_CurrentFrame = NO_FRAME;
            m_LastFrameIndex = NO_FRAME;
            m_PublicationId = 0;
        }
    }

    /// Set a free-form label identifying what's currently being profiled (e.g. "idle",
    /// "process-list-1000-rows"), included in CSV export so exported rows from different
    /// capture sessions can be told apart. Empty by default.
    void setScenario(std::string_view scenario) noexcept
    {
        try
        {
            m_Scenario = scenario;
        }
        // NOLINTNEXTLINE(bugprone-empty-catch) -- intentional: instrumentation must never crash the app
        catch (...)
        {
            m_Scenario.clear();
        }
    }

    [[nodiscard]] const std::string& scenario() const noexcept
    {
        return m_Scenario;
    }

    /// Publish the previous frame's accumulated samples as "last frame" and start a new,
    /// empty accumulation for frameIndex. Must be called once per frame by the per-frame
    /// driver (ShellLayer::onUpdate), unconditionally -- unlike record(), which only runs
    /// when a chart actually renders, this runs every frame so a zero-chart frame correctly
    /// publishes empty totals rather than leaving stale data in place. Idempotent within a
    /// single frame (repeat calls with the same frameIndex are no-ops). No-op while disabled.
    void beginFrame(int frameIndex) noexcept
    {
        if (!m_Enabled || frameIndex == m_CurrentFrame)
        {
            return;
        }

        // The very first beginFrame() after enabling transitions from the -1 sentinel; there is
        // no real previous frame to publish, so m_LastFrame is (and should remain) empty and
        // this doesn't count as a publication.
        const bool hadPreviousFrame = m_CurrentFrame != NO_FRAME;

        // Swap (not move+clear) so the two vectors' underlying storage alternates between
        // "current" and "last frame" roles each frame, reusing both buffers' capacity
        // indefinitely instead of repeatedly allocating/freeing every frame.
        std::swap(m_Current, m_LastFrame);
        m_Current.clear();
        // m_LastFrame now holds the frame that was *previously* current -- record its index
        // before overwriting m_CurrentFrame, so toCsv() reports which frame the data actually
        // came from, not the frame that's only just starting to accumulate.
        m_LastFrameIndex = m_CurrentFrame;
        m_CurrentFrame = frameIndex;

        if (hadPreviousFrame)
        {
            m_LastFrameTimestamp = std::chrono::system_clock::now();
            ++m_PublicationId;
        }
    }

    /// Record one chart's cost for the current frame (see beginFrame()).
    ///
    /// noexcept: called from destructors (HistoryChart, RenderMetricsScope), so a throwing
    /// allocation would hit std::terminate. Metrics are best-effort — on any failure the
    /// sample is dropped rather than propagating the exception.
    void record(std::string_view id, int vertices, int indices, double micros, int frameIndex) noexcept
    {
        if (!m_Enabled)
        {
            return;
        }

        // beginFrame() is the source of truth for frame boundaries; this handles only the
        // case where a chart's HistoryChart/RenderMetricsScope outlives beginFrame() somehow
        // not yet having run for its frame (e.g. capture enabled mid-frame) by falling back to
        // the same rollover beginFrame() would have done, so a sample is never misattributed
        // to a stale frame's bucket.
        beginFrame(frameIndex);

        try
        {
            m_Current.push_back({.id = std::string(id), .vertices = vertices, .indices = indices, .micros = micros});
        }
        // NOLINTNEXTLINE(bugprone-empty-catch) -- intentional: instrumentation must never crash the app
        catch (...)
        {
            // Drop the sample; instrumentation must never crash the app.
        }
    }

    [[nodiscard]] const std::vector<ChartRenderSample>& lastFrame() const noexcept
    {
        return m_LastFrame;
    }

    /// Serialize the last completed frame as CSV (header + one row per chart) for
    /// clipboard export into profiling notes or spreadsheets. Every row repeats the
    /// frame-level timestamp/scenario/frame_id/publication_id columns so a spreadsheet or
    /// script can group/filter rows from multiple pasted exports without a separate header.
    ///
    /// noexcept: called directly from UI code (overlay button click); on failure returns
    /// whatever was built so far — best-effort like all instrumentation.
    [[nodiscard]] std::string toCsv() const noexcept
    {
        std::string csv;
        try
        {
            const auto timestampUs = std::chrono::duration_cast<std::chrono::microseconds>(m_LastFrameTimestamp.time_since_epoch()).count();
            csv = "timestamp_us,scenario,frame_id,publication_id,chart,vertices,indices,cpu_us\n";
            for (const auto& sample : m_LastFrame)
            {
                csv += std::format("{},{},{},{},{},{},{},{:.1f}\n",
                                   timestampUs,
                                   csvField(m_Scenario),
                                   m_LastFrameIndex,
                                   m_PublicationId,
                                   csvField(sample.id),
                                   sample.vertices,
                                   sample.indices,
                                   sample.micros);
            }
        }
        // NOLINTNEXTLINE(bugprone-empty-catch) -- intentional: instrumentation must never crash the app
        catch (...)
        {
            // Return the partial CSV; instrumentation must never crash the app.
        }
        return csv;
    }

    /// Draw the "Render Metrics" overlay window. Enables capture while *open is true.
    void renderOverlay(bool* open);

  private:
    static constexpr int NO_FRAME = -1;

    /// RFC 4180-style CSV field encoding: wrap in double quotes (doubling any embedded quote)
    /// whenever the field contains a comma, quote, or newline. Needed for m_Scenario (free-form
    /// user input via the overlay's text field) and, defensively, chart ids, so a value like
    /// `resize, tab A` doesn't silently split into extra columns or corrupt the row.
    [[nodiscard]] static std::string csvField(std::string_view field)
    {
        if (field.find_first_of(",\"\r\n") == std::string_view::npos)
        {
            return std::string(field);
        }

        std::string escaped;
        escaped.reserve(field.size() + 2);
        escaped.push_back('"');
        for (const char character : field)
        {
            if (character == '"')
            {
                escaped.push_back('"');
            }
            escaped.push_back(character);
        }
        escaped.push_back('"');
        return escaped;
    }

    std::vector<ChartRenderSample> m_Current;
    std::vector<ChartRenderSample> m_LastFrame;
    int m_CurrentFrame = NO_FRAME;
    int m_LastFrameIndex = NO_FRAME;
    std::int64_t m_PublicationId = 0;
    std::chrono::system_clock::time_point m_LastFrameTimestamp;
    std::string m_Scenario;
    bool m_Enabled = false;
};

} // namespace UI
