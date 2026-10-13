#pragma once

// The Processes table's "Set affinity..." dialog (#1887): a checkbox per logical CPU, All and None, and
// Apply, which hands the chosen CPUs to ProcessesPanel to set on the one process the dialog was opened
// for, by identity (IProcessActions::setAffinity()). Nothing here calls the platform, so Cancel can
// change nothing; Apply is the confirmation, as in the batch priority dialog (#1539). The boxes are grouped
// by processor group and, on a hybrid CPU, by kind of core (#1881).

#include "App/Panels/ProcessAffinitySections.h"
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
    /// opens on the next render(). It offers @p layout's online CPUs, or when those are unknown CPUs 0
    /// to @p cpuCount - 1, and any other CPU @p current names (affinityDialogOfferedCpus()), in
    /// @p layout's sections (affinityDialogSections()). An unread (empty) @p current ticks every box.
    void open(Platform::ProcessTarget target,
              std::string name,
              const Platform::CpuAffinity& current,
              std::size_t cpuCount,
              const Platform::CpuAffinityLayout& layout = {});

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

    /// Ticks (or with false, clears) every box of section @p index, as its heading's box does.
    void chooseSection(std::size_t index, bool chosen) noexcept;

    /// The sections the boxes are drawn in, in order; one, untitled, unless the layout groups them.
    [[nodiscard]] const std::vector<AffinityDialogSection>& sections() const noexcept
    {
        return m_Sections;
    }

    /// Whether Apply is enabled: at least one CPU is ticked.
    [[nodiscard]] bool canApply() const noexcept;

    /// The CPUs ticked, as Apply hands them on: empty when none is.
    [[nodiscard]] Platform::CpuAffinity choice() const;

    /// How many CPUs the dialog offers.
    [[nodiscard]] std::size_t cpuCount() const noexcept
    {
        return m_OfferedCount;
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
    // A byte per CPU up to the highest offered, 1 = ticked (ImGui::Checkbox takes a bool, set below);
    // a CPU not offered (a gap in the online set) is never ticked.
    std::vector<std::uint8_t> m_Chosen;
    std::vector<std::uint8_t> m_Offered; // Parallel to m_Chosen: 1 = the dialog has a box for it
    std::size_t m_OfferedCount = 0;
    std::vector<AffinityDialogSection> m_Sections;
    std::size_t m_GroupCount = 0;    // Processor groups, for the CPU sets note
    bool m_AppliedAsCpuSets = false; // Platform::CpuAffinityLayout::appliedAsCpuSets
    ImVec2 m_LastWorkSize;           // For UI::Widgets::centerNextDialog()
    bool m_ShowRequested = false;
    bool m_Open = false; // As of the last render()
};

/// The CPUs to offer for a process whose affinity is @p current, on a machine with @p onlineCpus logical
/// CPUs (0 when unknown): every online one, and any higher one its affinity already names (one
/// that went offline since keeps its box). At least one.
[[nodiscard]] std::size_t affinityDialogCpuCount(const Platform::CpuAffinity& current, std::size_t onlineCpus) noexcept;

} // namespace App
