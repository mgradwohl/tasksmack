#pragma once

// The Processes table's "Set affinity..." dialog (#1887): a checkbox per logical CPU, All and None, and
// Apply, which hands the chosen CPUs to ProcessesPanel to set on the one process the dialog was opened
// for, by identity (IProcessActions::setAffinity()). Nothing here calls the platform, so Cancel can
// change nothing; Apply is the confirmation, as in the batch priority dialog (#1539).

#include "Platform/CpuAffinity.h"
#include "Platform/IProcessActions.h"

#include <imgui.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace App
{

class ProcessAffinityDialog
{
  public:
    /// Asks for the dialog for @p target (named @p name), its boxes ticked as @p current has them; it
    /// opens on the next render(). It offers @p cpuCount CPUs, or more when @p current names a higher
    /// one. An unread (empty) @p current ticks every box.
    void open(Platform::ProcessTarget target, std::string name, const Platform::CpuAffinity& current, std::size_t cpuCount);

    /// Whether the dialog has been asked for or is open: the panel asks for no other row action meanwhile.
    [[nodiscard]] bool isPending() const noexcept
    {
        return m_ShowRequested || m_Open;
    }

    /// Draws the dialog while it is open; call it every frame, outside any table, from the window the
    /// request came from. Returns the chosen CPUs on the frame Apply is pressed (the dialog then
    /// closes; set them on target()), and nullopt otherwise -- Cancel included. Apply is disabled
    /// while no CPU is chosen.
    [[nodiscard]] std::optional<Platform::CpuAffinity> render();

    /// Ticks or clears @p cpu's box, as a click does; a CPU the dialog doesn't offer is ignored.
    void choose(std::size_t cpu, bool chosen) noexcept;

    /// Ticks (or with false, clears) every box, as All (None) does.
    void chooseAll(bool chosen) noexcept;

    /// The CPUs ticked, as Apply hands them on: empty when none is.
    [[nodiscard]] Platform::CpuAffinity choice() const;

    /// How many CPUs the dialog offers.
    [[nodiscard]] std::size_t cpuCount() const noexcept
    {
        return m_Chosen.size();
    }

    /// The process Apply is for, by identity, and its name.
    [[nodiscard]] const Platform::ProcessTarget& target() const noexcept
    {
        return m_Target;
    }
    [[nodiscard]] const std::string& name() const noexcept
    {
        return m_Name;
    }

  private:
    Platform::ProcessTarget m_Target{.pid = -1, .startTimeTicks = 0};
    std::string m_Name;
    std::vector<std::uint8_t> m_Chosen; // A byte per offered CPU, 1 = ticked (ImGui::Checkbox takes a bool, set below)
    ImVec2 m_LastWorkSize;              // For UI::Widgets::centerNextDialog()
    bool m_ShowRequested = false;
    bool m_Open = false; // As of the last render()
};

/// The CPUs to offer for a process whose affinity is @p current, on a machine with @p onlineCpus logical
/// CPUs (0 when unknown): every online one, and any higher one its affinity already names (one
/// that went offline since keeps its box). At least one.
[[nodiscard]] std::size_t affinityDialogCpuCount(const Platform::CpuAffinity& current, std::size_t onlineCpus) noexcept;

} // namespace App
