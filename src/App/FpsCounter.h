#pragma once

#include <algorithm>
#include <cstdint>

namespace App
{

/// Frame-rate counter: accumulates per-frame delta times and periodically recomputes a
/// smoothed FPS value once the accumulated time reaches `averageWindowSeconds`, so the
/// display stays meaningful at any frame rate rather than jittering every frame. Extracted
/// from ShellLayer so this pure timing/averaging logic can be unit-tested without an ImGui
/// context.
class FpsCounter
{
  public:
    /// A non-positive averaging window would let update() divide by an accumulator that can
    /// still be zero (e.g. a 0.0F deltaTime on the very first frame), so the window is
    /// clamped to a small positive floor rather than trusted as-is.
    static constexpr float MIN_AVERAGING_WINDOW_SECONDS = 1.0e-3F;

    explicit FpsCounter(float averageWindowSeconds = 0.5F)
        : m_AverageWindowSeconds(std::max(averageWindowSeconds, MIN_AVERAGING_WINDOW_SECONDS))
    {}

    /// Records one frame's delta time. Recomputes displayedFps() once the accumulated time
    /// reaches the averaging window, then resets the accumulator for the next window.
    void update(float deltaTime)
    {
        m_FrameTime = deltaTime;
        m_FrameTimeAccumulator += deltaTime;
        ++m_FrameCount;

        if (m_FrameTimeAccumulator >= m_AverageWindowSeconds)
        {
            m_DisplayedFps = static_cast<float>(m_FrameCount) / m_FrameTimeAccumulator;
            m_FrameTimeAccumulator = 0.0F;
            m_FrameCount = 0U;
        }
    }

    /// Most recent single-frame delta time, in seconds.
    [[nodiscard]] float frameTime() const noexcept
    {
        return m_FrameTime;
    }

    /// Smoothed FPS, updated once per averaging window (0.0 until the first window completes).
    [[nodiscard]] float displayedFps() const noexcept
    {
        return m_DisplayedFps;
    }

  private:
    float m_AverageWindowSeconds;
    float m_FrameTime = 0.0F;
    float m_FrameTimeAccumulator = 0.0F;
    std::uint32_t m_FrameCount = 0U;
    float m_DisplayedFps = 0.0F;
};

} // namespace App
