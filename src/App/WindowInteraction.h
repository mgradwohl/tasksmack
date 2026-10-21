#pragma once

// The custom title bar's drag and resize sessions (#1547), as plain state: what TitleBarLayer does to
// the window for each pointer sample, decided here without SDL, a window or a clock, so the
// thresholds, rebasing and throttling are unit-testable. TitleBarLayer reads the pointer and the
// time, applies each step to the window, and traces how long that took.

#include "TitleBarGeometry.h"

#include <cstdint>
#include <optional>
#include <utility>

namespace App
{

/// A drag of the window by its title bar. On a maximized window the restore waits until the pointer
/// has moved DRAG_THRESHOLD_PX in some direction, so a bare click does not unmaximize it; the drag
/// then continues from where the restored window was placed under the pointer.
class DragTracker
{
  public:
    static constexpr int DRAG_THRESHOLD_PX = 5;

    struct Step
    {
        enum class Kind : std::uint8_t
        {
            None,    // nothing to do this sample
            Restore, // restore the window, then call rebaseAfterRestore() and move it there
            Move,    // move the window to (x, y)
        };
        Kind kind = Kind::None;
        int x = 0;
        int y = 0;
    };

    /// The press: the pointer and the window's origin in global coordinates. @p maximizedWidth is
    /// read only when @p maximized.
    void begin(int mouseX, int mouseY, int windowX, int windowY, bool maximized, int maximizedWidth) noexcept
    {
        m_StartMouseX = mouseX;
        m_StartMouseY = mouseY;
        m_StartWindowX = windowX;
        m_StartWindowY = windowY;
        m_LastAppliedX = windowX;
        m_LastAppliedY = windowY;
        m_PendingRestore = maximized;
        m_MaximizedWindowX = windowX;
        m_MaximizedWindowY = windowY;
        m_MaximizedWindowWidth = maximized ? maximizedWidth : 0;
    }

    /// The pointer now, in global coordinates.
    [[nodiscard]] Step step(int mouseX, int mouseY) noexcept
    {
        const int dx = mouseX - m_StartMouseX;
        const int dy = mouseY - m_StartMouseY;
        if (m_PendingRestore)
        {
            const bool withinThreshold =
                dx > -DRAG_THRESHOLD_PX && dx < DRAG_THRESHOLD_PX && dy > -DRAG_THRESHOLD_PX && dy < DRAG_THRESHOLD_PX;
            return withinThreshold ? Step{} : Step{.kind = Step::Kind::Restore};
        }
        const int targetX = m_StartWindowX + dx;
        const int targetY = m_StartWindowY + dy;
        if (targetX == m_LastAppliedX && targetY == m_LastAppliedY)
        {
            return {};
        }
        m_LastAppliedX = targetX;
        m_LastAppliedY = targetY;
        return {.kind = Step::Kind::Move, .x = targetX, .y = targetY};
    }

    /// After a Restore, with the pointer now at (@p mouseX, @p mouseY): where to put the restored
    /// window, @p restoredWidth wide, so the pointer keeps its place on the title bar: its proportion
    /// across (computeRestoreFromMaximizedDragX()) and its distance below the top
    /// (computeRestoreFromMaximizedDragY(), kept at or below the maximized window's top, which is the
    /// work area's) (#2715). The drag goes on from there, still relative to the original press.
    [[nodiscard]] std::pair<int, int> rebaseAfterRestore(int restoredWidth, int mouseX, int mouseY) noexcept
    {
        const int dx = mouseX - m_StartMouseX;
        const int dy = mouseY - m_StartMouseY;
        const int adjustedX =
            computeRestoreFromMaximizedDragX(m_StartMouseX, m_MaximizedWindowX, m_MaximizedWindowWidth, restoredWidth) + dx;
        const int adjustedY = computeRestoreFromMaximizedDragY(m_StartMouseY, m_MaximizedWindowY, mouseY, m_MaximizedWindowY);
        // Later moves add the pointer's offset from the press to the start, so the start is this
        // position less that offset: another sample at this pointer position is not a move.
        m_StartWindowX = adjustedX - dx;
        m_StartWindowY = adjustedY - dy;
        m_LastAppliedX = adjustedX;
        m_LastAppliedY = adjustedY;
        m_PendingRestore = false;
        return {adjustedX, adjustedY};
    }

    [[nodiscard]] bool pendingRestore() const noexcept
    {
        return m_PendingRestore;
    }

  private:
    int m_StartMouseX = 0;
    int m_StartMouseY = 0;
    int m_StartWindowX = 0;
    int m_StartWindowY = 0;
    int m_LastAppliedX = 0;
    int m_LastAppliedY = 0;
    bool m_PendingRestore = false;
    int m_MaximizedWindowX = 0;
    int m_MaximizedWindowY = 0;
    int m_MaximizedWindowWidth = 0;
};

/// A resize from a window edge or corner. The position follows every sample; the size is committed at
/// most every SIZE_COMMIT_INTERVAL_SECONDS, the latest one held back until then (or until the end of
/// the drag), and dropped if the pointer comes back to the committed size first. A WindowResizedEvent
/// for a commit is raised at most every RESIZE_EVENT_INTERVAL_SECONDS, and only for a new pixel size.
class ResizeTracker
{
  public:
    static constexpr double SIZE_COMMIT_INTERVAL_SECONDS = 1.0 / 20.0;
    static constexpr double RESIZE_EVENT_INTERVAL_SECONDS = 1.0 / 120.0;

    struct Step
    {
        bool move = false; // move the window to (x, y)
        int x = 0;
        int y = 0;
        bool commitSize = false; // resize the window to width x height
        int width = 0;
        int height = 0;
    };

    /// The press on @p edge: the pointer and the window's rectangle in global coordinates, at @p now.
    void begin(ResizeEdge edge, int mouseX, int mouseY, WindowRect window, double now) noexcept
    {
        *this = {};
        m_Edge = edge;
        m_StartMouseX = mouseX;
        m_StartMouseY = mouseY;
        m_Start = window;
        m_LastApplied = window;
        m_LastSizeCommitTime = now;
    }

    /// The pointer now, in global coordinates, with the window's minimum size, at @p now.
    [[nodiscard]] Step step(int mouseX, int mouseY, int minWidth, int minHeight, double now) noexcept
    {
        const WindowRect target = computeResizeGeometry(m_Edge,
                                                        m_Start.x,
                                                        m_Start.y,
                                                        m_Start.width,
                                                        m_Start.height,
                                                        mouseX - m_StartMouseX,
                                                        mouseY - m_StartMouseY,
                                                        minWidth,
                                                        minHeight);
        Step result;
        if (target.x != m_LastApplied.x || target.y != m_LastApplied.y)
        {
            result.move = true;
            result.x = target.x;
            result.y = target.y;
            m_LastApplied.x = target.x;
            m_LastApplied.y = target.y;
        }
        if (target.width != m_LastApplied.width || target.height != m_LastApplied.height)
        {
            if (now - m_LastSizeCommitTime >= SIZE_COMMIT_INTERVAL_SECONDS)
            {
                result.commitSize = true;
                result.width = target.width;
                result.height = target.height;
                m_LastApplied.width = target.width;
                m_LastApplied.height = target.height;
                m_LastSizeCommitTime = now;
                m_Pending.reset();
            }
            else
            {
                m_Pending = std::pair{target.width, target.height};
            }
        }
        else
        {
            // Back at the committed size: a held-back size would now jump the window to one the
            // pointer has left, so it is dropped.
            m_Pending.reset();
        }
        return result;
    }

    /// The size held back for the end of the drag, if any.
    [[nodiscard]] std::optional<std::pair<int, int>> pendingSize() const noexcept
    {
        return m_Pending;
    }

    /// After a size commit: whether to raise a WindowResizedEvent for the window's new @p pixelWidth x
    /// @p pixelHeight at @p now. Recorded as raised when it says so.
    [[nodiscard]] bool takeResizeEvent(int pixelWidth, int pixelHeight, double now) noexcept
    {
        const bool pixelSizeChanged = pixelWidth != m_LastEventPixelWidth || pixelHeight != m_LastEventPixelHeight;
        if (!pixelSizeChanged || now - m_LastEventTime < RESIZE_EVENT_INTERVAL_SECONDS)
        {
            return false;
        }
        m_LastEventPixelWidth = pixelWidth;
        m_LastEventPixelHeight = pixelHeight;
        m_LastEventTime = now;
        return true;
    }

    [[nodiscard]] ResizeEdge edge() const noexcept
    {
        return m_Edge;
    }

  private:
    ResizeEdge m_Edge = ResizeEdge::None;
    int m_StartMouseX = 0;
    int m_StartMouseY = 0;
    WindowRect m_Start{};
    WindowRect m_LastApplied{};
    std::optional<std::pair<int, int>> m_Pending;
    double m_LastSizeCommitTime = 0.0;
    int m_LastEventPixelWidth = 0;
    int m_LastEventPixelHeight = 0;
    double m_LastEventTime = 0.0;
};

} // namespace App
