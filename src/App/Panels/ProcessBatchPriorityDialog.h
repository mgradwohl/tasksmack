#pragma once

// The Processes table's "Set priority for N processes..." dialog (#1484): one priority, picked with the
// same control as Process Details' (Detail::renderPriorityPicker(): the nice slider, or the Windows
// priority-class combo), for every selected process. It only picks the value: Continue hands it to
// ProcessesPanel, which asks the batch confirmation (ProcessBatch::priorityConfirmBody()) and then
// sets it on each process by identity (ProcessBatch::runBatchPriority()). Nothing here calls the
// platform, so Cancel can change nothing.

#include "Domain/PriorityConfig.h"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace App
{

class ProcessBatchPriorityDialog
{
  public:
    /// Asks for the dialog for @p count processes, showing the default priority; it opens on the next
    /// render().
    void open(std::size_t count) noexcept
    {
        m_Count = count;
        m_Nice = Domain::Priority::NORMAL_NICE;
        m_ShowRequested = true;
    }

    /// Whether the dialog has been asked for or is open: the panel asks for no other row action meanwhile.
    [[nodiscard]] bool isPending() const noexcept
    {
        return m_ShowRequested || m_Open;
    }

    /// Draws the dialog while it is open; call it every frame, outside any table, from the window the
    /// request came from. Returns the picked nice value on the frame Continue is pressed (the dialog
    /// then closes), and nullopt otherwise -- Cancel included.
    [[nodiscard]] std::optional<std::int32_t> render();

    /// Picks @p nice (held to the nice range), as the control does.
    void pickNice(std::int32_t nice) noexcept
    {
        m_Nice = Domain::Priority::clampNice(nice);
    }

    /// The value the dialog shows: what Continue hands on.
    [[nodiscard]] std::int32_t niceValue() const noexcept
    {
        return m_Nice;
    }

    /// How many processes the dialog was opened for.
    [[nodiscard]] std::size_t count() const noexcept
    {
        return m_Count;
    }

  private:
    std::size_t m_Count = 0;
    std::int32_t m_Nice = Domain::Priority::NORMAL_NICE;
    bool m_ShowRequested = false;
    bool m_Open = false; // As of the last render()
};

} // namespace App
