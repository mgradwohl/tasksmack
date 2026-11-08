#pragma once

// The Processes table's "Set I/O priority for N processes..." dialog (#3551): one I/O priority, picked
// with the same slider as Process Details' I/O priority control (Detail::IO_PRIORITY_SLIDER, or without
// Realtime unless the platform can set it, #1540; on Windows Detail::IO_HINT_SLIDER, #3362), for every
// selected process. Like the batch priority dialog (ProcessBatchPriorityDialog, #1539) it is the only
// dialog for the change: it lists the processes it will touch -- name, PID and current I/O priority,
// TaskSmack itself and PID 1 always named -- and its "Apply to N processes" is the confirmation. Apply
// hands the picked value to ProcessesPanel, which sets it on each listed process by identity
// (ProcessBatch::runBatchIoPriority()). Nothing here calls the platform, so Cancel can change nothing.

#include "App/Panels/ProcessBatchAction.h"
#include "Platform/IoPriorityTypes.h"

#include <imgui.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace App
{

class ProcessBatchIoPriorityDialog
{
  public:
    ProcessBatchIoPriorityDialog() = default;
    ~ProcessBatchIoPriorityDialog() = default;

    // Not copyable: m_Listed points into m_Targets, as in ProcessBatchPriorityDialog (#2143). Moving
    // keeps the vector's buffer, so the moved-to dialog's list still points into its own m_Targets.
    ProcessBatchIoPriorityDialog(const ProcessBatchIoPriorityDialog&) = delete;
    ProcessBatchIoPriorityDialog& operator=(const ProcessBatchIoPriorityDialog&) = delete;
    ProcessBatchIoPriorityDialog(ProcessBatchIoPriorityDialog&&) noexcept = default;
    ProcessBatchIoPriorityDialog& operator=(ProcessBatchIoPriorityDialog&&) noexcept = default;

    /// Asks for the dialog for @p targets (the selection, as ProcessBatch::resolveIoPriorityTargets()
    /// found it), showing the default I/O priority; it opens on the next render(). @p ownPid is
    /// TaskSmack's own PID (0 when unknown), which the list always names. @p realtimeSettable offers
    /// Linux's Realtime class (ProcessActionCapabilities::canSetRealtimeIoPriority); @p windowsHints
    /// picks Windows' hints instead of ionice's classes (ProcessActionCapabilities::ioPriorityHints).
    void open(std::vector<ProcessBatch::BatchTarget> targets, std::int32_t ownPid, bool realtimeSettable, bool windowsHints);

    /// Whether the dialog has been asked for or is open: the panel asks for no other row action meanwhile.
    [[nodiscard]] bool isPending() const noexcept
    {
        return m_ShowRequested || m_Open;
    }

    /// Draws the dialog while it is open; call it every frame, outside any table, from the window the
    /// request came from. Returns the picked I/O priority on the frame Apply is pressed (the dialog then
    /// closes; set it on targets()), and nullopt otherwise -- Cancel included.
    [[nodiscard]] std::optional<Platform::IoPriority> render();

    /// Picks @p priority, as the slider does: the classes without a level are held to level 0.
    void pick(const Platform::IoPriority& priority) noexcept;

    /// The value the dialog shows: what Apply hands on.
    [[nodiscard]] const Platform::IoPriority& value() const noexcept
    {
        return m_Priority;
    }

    /// How many processes the dialog was opened for.
    [[nodiscard]] std::size_t count() const noexcept
    {
        return m_Targets.size();
    }

    /// The processes the dialog lists and Apply is for, by identity.
    [[nodiscard]] std::span<const ProcessBatch::BatchTarget> targets() const noexcept
    {
        return m_Targets;
    }

    /// The rows the list draws (at most ProcessBatch::CONFIRM_LIST_LIMIT) and how many it folds into
    /// "+N more".
    [[nodiscard]] const ProcessBatch::ListedTargets& listed() const noexcept
    {
        return m_Listed;
    }

    /// Whether the slider offers Linux's Realtime class: what open() was told the platform can set now.
    [[nodiscard]] bool realtimeSettable() const noexcept
    {
        return m_RealtimeSettable;
    }

    /// The Apply button's label: "Apply to 5 processes".
    [[nodiscard]] const std::string& applyLabel() const noexcept
    {
        return m_ApplyLabel;
    }

    /// The value a newly opened dialog shows: the class never set (each process follows its nice value)
    /// on Linux, the Normal hint on Windows. Both are every process's default.
    [[nodiscard]] static Platform::IoPriority defaultValue(bool windowsHints) noexcept;

  private:
    std::vector<ProcessBatch::BatchTarget> m_Targets;
    ProcessBatch::ListedTargets m_Listed; // Points into m_Targets; rebuilt by open()
    std::string m_Warnings;               // TaskSmack itself / PID 1 paragraphs, built by open()
    std::string m_ApplyLabel;
    Platform::IoPriority m_Priority{};
    ImVec2 m_LastWorkSize; // For UI::Widgets::centerNextDialog()
    bool m_RealtimeSettable = false;
    bool m_WindowsHints = false;
    bool m_ShowRequested = false;
    bool m_Open = false; // As of the last render()
};

} // namespace App
