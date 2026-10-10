#pragma once

// Process Details' "Recent crashes" line (#1675): how often the selected process's executable has
// crashed or hung in the last Platform::CRASH_HISTORY_DAYS days, with its newest few in a tooltip.
//
// The data is the shared Domain::CrashHistory, which the System Information read also fills (#1524),
// so the event log or the core dump directory is never read per frame or per selection change:
// update(), from the panel's update path, starts one read on a worker only while the line was drawn
// on the last frame and the cache is empty or older than Domain::Sampling::PROCESS_CRASH_HISTORY_REFRESH_MS.
// A newer read published by the System tab counts as fresh. Matching the executable against the
// list is Domain's (crashCountsFor()); the text is built here once per read and executable, never per
// frame. Everything but render() is free of ImGui and tested without a context; render() is run
// headless in tests/App/test_ProcessCrashHistoryView.cpp.

#include "Domain/CrashHistory.h"

#include <cstdint>
#include <future>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace App
{

namespace Detail
{

/// How the crash line's value is drawn.
enum class CrashLineTone : std::uint8_t
{
    Muted,   ///< Nothing to report, not read yet, or unreadable
    Warning, ///< At least one crash or hang
};

/// The crash line for one executable, built from one read.
struct CrashLine
{
    bool visible = false; ///< False where the platform has no crash history at all
    std::string value;    ///< "2 crashes, 1 hang in the last 14 days (last 2026-10-02 14:44)"
    CrashLineTone tone = CrashLineTone::Muted;
    std::string tooltip; ///< The newest few, how they were matched, and when they were read
};

/// The crash line for @p executable (its file name or path) from @p history; "Reading..." before the
/// first read.
[[nodiscard]] CrashLine crashLineFor(const Domain::CrashHistorySnapshot& history, std::string_view executable);

} // namespace Detail

class ProcessCrashHistoryView
{
  public:
    ProcessCrashHistoryView() = default;
    /// Waits for a read in flight (a std::async future); it holds its own reference to the history.
    ~ProcessCrashHistoryView() = default;

    ProcessCrashHistoryView(const ProcessCrashHistoryView&) = delete;
    ProcessCrashHistoryView& operator=(const ProcessCrashHistoryView&) = delete;
    ProcessCrashHistoryView(ProcessCrashHistoryView&&) noexcept = default;
    ProcessCrashHistoryView& operator=(ProcessCrashHistoryView&&) noexcept = default;

    /// The shared cache, from the composition root (ShellLayer); null hides the line (synthetic runs, tests).
    void setHistory(std::shared_ptr<Domain::CrashHistory> history) noexcept
    {
        m_History = std::move(history);
    }

    /// Takes in a finished read and, when the line was drawn since the last call and the cache is
    /// empty or stale, starts a read on a worker. Advances the staleness clock by @p deltaSeconds.
    /// @return true when a read was started.
    bool update(float deltaSeconds);

    /// Draws the line for @p executable (the selected process's name); nothing without a history.
    void render(std::string_view executable);

    /// Records that the line was drawn this frame (render() does; tests call it directly).
    void markDrawn() noexcept
    {
        m_Drawn = true;
    }

    /// Waits for a read in flight (tests).
    void finishPendingRead();

    /// The line for @p executable from the newest read, rebuilt only when the read or the executable changes.
    [[nodiscard]] const Detail::CrashLine& line(std::string_view executable);

  private:
    /// Takes in the finished (or, from finishPendingRead(), waited-for) read.
    void takePending();

    std::shared_ptr<Domain::CrashHistory> m_History;
    std::future<void> m_Pending; // the read in flight, if any
    bool m_Drawn = false;        // drawn since the last update()
    bool m_Attempted = false;    // a read was started (or failed to start) at least once
    std::uint64_t m_SeenVersion = 0;
    float m_SecondsSinceFresh = 0.0F; // since the newest read was published or one was started

    // The line as last built, and what it was built from.
    std::shared_ptr<const Domain::CrashHistorySnapshot> m_LineSnapshot;
    std::string m_LineExecutable;
    Detail::CrashLine m_Line;
};

} // namespace App
