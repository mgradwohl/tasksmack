#pragma once

// The Processes table's "Set priority for N processes..." dialog (#1484, #1539): one priority, picked with
// the same control as Process Details' (Detail::renderPriorityPicker(): the nice slider, or the Windows
// priority-class combo), for every selected process. It is the only dialog for the change: it lists the
// processes it will touch -- name, PID and current priority, TaskSmack itself and PID 1 always named --
// and its "Apply to N processes" is the confirmation. Apply hands the picked value to ProcessesPanel,
// which sets it on each listed process by identity (ProcessBatch::runBatchPriority()). Nothing here calls
// the platform, so Cancel can change nothing.

#include "App/Panels/ProcessBatchAction.h"
#include "Domain/PriorityConfig.h"

#include <imgui.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace App
{

class ProcessBatchPriorityDialog
{
  public:
    /// Asks for the dialog for @p targets (the selection, as ProcessBatch::resolveTargets() found it),
    /// showing the default priority; it opens on the next render(). @p ownPid is TaskSmack's own PID (0
    /// when unknown), which the list always names.
    void open(std::vector<ProcessBatch::BatchTarget> targets, std::int32_t ownPid);

    /// Whether the dialog has been asked for or is open: the panel asks for no other row action meanwhile.
    [[nodiscard]] bool isPending() const noexcept
    {
        return m_ShowRequested || m_Open;
    }

    /// Draws the dialog while it is open; call it every frame, outside any table, from the window the
    /// request came from. Returns the picked nice value on the frame Apply is pressed (the dialog then
    /// closes; set it on targets()), and nullopt otherwise -- Cancel included.
    [[nodiscard]] std::optional<std::int32_t> render();

    /// Picks @p nice (held to the nice range), as the control does.
    void pickNice(std::int32_t nice) noexcept
    {
        m_Nice = Domain::Priority::clampNice(nice);
    }

    /// The value the dialog shows: what Apply hands on.
    [[nodiscard]] std::int32_t niceValue() const noexcept
    {
        return m_Nice;
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

    /// The Apply button's label: "Apply to 5 processes".
    [[nodiscard]] const std::string& applyLabel() const noexcept
    {
        return m_ApplyLabel;
    }

  private:
    std::vector<ProcessBatch::BatchTarget> m_Targets;
    ProcessBatch::ListedTargets m_Listed; // Points into m_Targets; rebuilt by open()
    std::string m_Warnings;               // TaskSmack itself / PID 1 paragraphs, built by open()
    std::string m_ApplyLabel;
    std::int32_t m_Nice = Domain::Priority::NORMAL_NICE;
    ImVec2 m_LastWorkSize; // For UI::Widgets::centerNextDialog()
    bool m_ShowRequested = false;
    bool m_Open = false; // As of the last render()
};

} // namespace App
