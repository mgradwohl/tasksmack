#pragma once

// The "Set scheduling policy..." dialog (#3086, Linux): a choice of Normal, Batch or Idle, each with a line
// saying what it does, and Apply, which hands the chosen policy to the Processes panel or Process Details to
// set on the one process the dialog was opened for, by identity (IProcessActions::setSchedPolicy()). Nothing
// here calls the platform, so Cancel can change nothing; Apply is the confirmation, as in the affinity
// dialog (#1887). Real time (FIFO, round robin) isn't offered: that is #3219.

#include "App/Panels/ProcessDetailsPanel_ActionHelpers.h"
#include "Platform/IProcessActions.h"
#include "Platform/SchedPolicy.h"

#include <imgui.h>

#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace App
{

/// One choice the dialog offers: the policy, its label and the line under it.
struct SchedPolicyChoice
{
    Platform::SchedPolicy policy = Platform::SchedPolicy::Normal;
    const char* label = "";
    const char* description = "";
};

/// The dialog's choices, in the order it lists them: Normal, Batch, Idle.
inline constexpr std::array<SchedPolicyChoice, 3> SCHED_POLICY_CHOICES{
    {
        {
            .policy = Platform::SchedPolicy::Normal,
            .label = "Normal",
            .description = "The default time sharing: the process shares the CPU with others by its priority (nice).",
        },
        {
            .policy = Platform::SchedPolicy::Batch,
            .label = "Batch",
            .description = "For CPU-bound work: the scheduler never treats it as interactive, so it is a little less responsive.",
        },
        {
            .policy = Platform::SchedPolicy::Idle,
            .label = "Idle",
            .description = "Runs only when nothing else wants the CPU. Leaving Idle later may need an administrator.",
        },
    },
};

class ProcessSchedPolicyDialog
{
  public:
    /// Asks for the dialog for @p target (named @p name), now under @p current; it opens on the next render().
    /// Normal, Batch or Idle is chosen as @p current has it. A process under any other policy (FIFO, round
    /// robin, Deadline, or one not read) opens with nothing chosen, so Apply waits for a choice: preselecting
    /// Normal there would make one press of Apply take a real-time process off real time without the user
    /// having picked that.
    void open(Platform::ProcessTarget target, std::string name, Platform::SchedPolicy current);

    /// Whether the dialog has been asked for or is open: the panel asks for no other row action meanwhile.
    [[nodiscard]] bool isPending() const noexcept
    {
        return m_ShowRequested || m_Open;
    }

    /// Draws the dialog while it is open; call it every frame, outside any table, from the window the request
    /// came from. Returns the chosen policy on the frame Apply is pressed (the dialog then closes; set it on
    /// target()), and nullopt otherwise -- Cancel and Escape included. Apply is disabled while nothing is chosen.
    [[nodiscard]] std::optional<Platform::SchedPolicy> render();

    /// Closes the dialog unapplied on the next render() (the selection moved to another process); a request
    /// not yet opened is dropped.
    void dismiss() noexcept
    {
        m_ShowRequested = false;
        m_DismissRequested = m_Open;
    }

    /// Chooses @p policy, as its radio button does; a policy the dialog doesn't offer is ignored.
    void choose(Platform::SchedPolicy policy) noexcept;

    /// The policy chosen, as Apply hands it on: nullopt while none is.
    [[nodiscard]] std::optional<Platform::SchedPolicy> choice() const noexcept
    {
        return m_Choice;
    }

    /// Whether Apply is enabled: a policy is chosen.
    [[nodiscard]] bool canApply() const noexcept
    {
        return m_Choice.has_value();
    }

    /// The policy the process was under when the dialog was opened.
    [[nodiscard]] Platform::SchedPolicy current() const noexcept
    {
        return m_Current;
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
    Platform::SchedPolicy m_Current = Platform::SchedPolicy::Unknown;
    std::optional<Platform::SchedPolicy> m_Choice;
    ImVec2 m_LastWorkSize; // For UI::Widgets::centerNextDialog()
    bool m_ShowRequested = false;
    bool m_DismissRequested = false;
    bool m_Open = false; // As of the last render()
};

/// The result line for setting @p policy on @p name (PID @p pid): "Scheduling policy of server (PID 4242) set
/// to Batch", or the platform's error. Shared by the row menu and Process Details' Actions card.
[[nodiscard]] Detail::ActionResultMessage schedPolicyResultMessage(const std::string& name,
                                                                   std::int32_t pid,
                                                                   Platform::SchedPolicy policy,
                                                                   const Platform::ProcessActionResult& result);

} // namespace App
