#pragma once

// The "Set scheduling policy..." dialog (#3086, Linux): a choice of Normal, Batch or Idle, or the real-time FIFO and
// round robin with a priority from 1 to 99 (#3219), each with a line saying what it does, and Apply, which hands the
// chosen policy to the Processes panel or Process Details to set on the one process the dialog was opened for, by
// identity (IProcessActions::setSchedPolicy()). Nothing here calls the platform, so Cancel can change nothing. For
// Normal, Batch and Idle, Apply is the confirmation, as in the affinity dialog (#1887); for real time, a warning
// says what it risks while FIFO or RR is chosen, and Apply first asks to confirm, naming the process, its PID, the
// policy and the priority, before Set hands it on. TaskSmack's own process is never offered real time.

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

/// The dialog's choices, in the order it lists them: Normal, Batch, Idle, then the real-time FIFO and RR (#3219).
inline constexpr std::array<SchedPolicyChoice, 5> SCHED_POLICY_CHOICES{
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
        {
            .policy = Platform::SchedPolicy::Fifo,
            .label = "FIFO",
            .description = "Real time: runs ahead of every normal process, at its priority, until it blocks or yields.",
        },
        {
            .policy = Platform::SchedPolicy::RoundRobin,
            .label = "RR",
            .description = "Real time, round robin: as FIFO, but processes at the same priority take turns in time slices.",
        },
    },
};

/// What Apply hands on: the policy, and its real-time priority (1 to 99 for FIFO and RR, 0 for the others).
struct SchedPolicySetting
{
    Platform::SchedPolicy policy = Platform::SchedPolicy::Normal;
    std::int32_t rtPriority = 0;
    friend bool operator==(const SchedPolicySetting&, const SchedPolicySetting&) = default;
};

/// The priority a new real-time choice starts at: the middle of 1 to 99 (#3219).
inline constexpr std::int32_t DEFAULT_REALTIME_PRIORITY = 50;

/// What the opener knows besides the process: TaskSmack's own PID (never offered real time; 0 when unknown), and
/// whether real time needs no password (ProcessActionCapabilities::canSetRealtimeSchedPolicy). Without it the dialog
/// still offers FIFO and RR, since the user's RLIMIT_RTPRIO may allow them, and says that a refusal may follow.
struct SchedPolicyDialogOptions
{
    std::int32_t ownPid = 0;
    bool realtimeNeedsNoPassword = false;
};

class ProcessSchedPolicyDialog
{
  public:
    /// Asks for the dialog for @p target (named @p name), now under @p current at real-time priority
    /// @p currentRtPriority; it opens on the next render(). A policy the dialog offers is chosen as @p current has it:
    /// Normal, Batch or Idle, or (#3219) FIFO or RR with the slider at @p currentRtPriority, so Apply re-applies what
    /// the process already runs under. A process under Deadline, one not read, or one on FIFO or RR whose priority
    /// wasn't read (@p currentRtPriority 0, which real time never has) opens with nothing chosen, so Apply waits for a
    /// choice: preselecting Normal there would make one press of Apply take it off Deadline, and preselecting FIFO 1
    /// would silently lower it to the least real time, without the user having picked that. The slider starts at
    /// DEFAULT_REALTIME_PRIORITY for a process not on real time, or whose priority wasn't read.
    void open(Platform::ProcessTarget target,
              std::string name,
              Platform::SchedPolicy current,
              std::uint32_t currentRtPriority = 0,
              SchedPolicyDialogOptions options = {});

    /// Whether the dialog has been asked for or is open: the panel asks for no other row action meanwhile.
    [[nodiscard]] bool isPending() const noexcept
    {
        return m_ShowRequested || m_Open;
    }

    /// Draws the dialog while it is open; call it every frame, outside any table, from the window the request
    /// came from. Returns the chosen setting on the frame it is applied (the dialog then closes; set it on
    /// target()), and nullopt otherwise -- Cancel and Escape included. Apply is disabled while nothing is chosen.
    /// For FIFO or RR, Apply shows the confirm step instead, and its Set applies; Back returns to the choices.
    [[nodiscard]] std::optional<SchedPolicySetting> render();

    /// Closes the dialog unapplied on the next render() (the selection moved to another process); a request
    /// not yet opened is dropped.
    void dismiss() noexcept
    {
        m_ShowRequested = false;
        m_DismissRequested = m_Open;
    }

    /// Chooses @p policy, as its radio button does; a policy the dialog doesn't offer is ignored, as is real time
    /// for TaskSmack's own process.
    void choose(Platform::SchedPolicy policy) noexcept;

    /// Sets the real-time priority, as the slider does, clamped to 1 to 99.
    void setRtPriority(std::int32_t priority) noexcept;

    /// The policy chosen, as Apply hands it on: nullopt while none is.
    [[nodiscard]] std::optional<Platform::SchedPolicy> choice() const noexcept
    {
        return m_Choice;
    }

    /// The slider's real-time priority, 1 to 99: handed on with FIFO or RR, ignored for the others.
    [[nodiscard]] std::int32_t rtPriority() const noexcept
    {
        return m_RtPriority;
    }

    /// The setting Apply hands on, as render() returns it: nullopt while nothing is chosen.
    [[nodiscard]] std::optional<SchedPolicySetting> setting() const noexcept;

    /// Whether real time may be chosen: not for TaskSmack's own process (#3219).
    [[nodiscard]] bool offersRealtime() const noexcept
    {
        return !m_OwnProcess;
    }

    /// Whether the real-time warning shows: FIFO or RR is chosen.
    [[nodiscard]] bool showsRealtimeWarning() const noexcept
    {
        return m_Choice.has_value() && Platform::isRealtimeSchedPolicy(*m_Choice);
    }

    /// Whether the confirm step for real time is showing (Apply was pressed with FIFO or RR chosen).
    [[nodiscard]] bool confirming() const noexcept
    {
        return m_Confirming;
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
    /// The choices: the radio buttons with their lines, and, while FIFO or RR is chosen, the slider and the warning.
    void renderChoices();
    /// The priority slider and the real-time warning, drawn by renderChoices() while FIFO or RR is chosen (#3219).
    void renderRealtimeControls();
    /// The real-time confirm step's text for @p chosen: the process, its PID, the policy and the priority.
    void renderConfirmBody(const SchedPolicySetting& chosen) const;
    /// The footer: Apply and Cancel at the choices, Set and Back at the confirm step (@p confirming). Returns the
    /// setting on the frame it is applied, nothing for Cancel, Escape (@p escaped), Back, or a press that only moves
    /// on to the confirm step.
    [[nodiscard]] std::optional<SchedPolicySetting> renderFooter(bool escaped, bool confirming);

    Platform::ProcessTarget m_Target{.pid = -1, .startTimeTicks = 0};
    std::string m_Name;
    Platform::SchedPolicy m_Current = Platform::SchedPolicy::Unknown;
    std::optional<Platform::SchedPolicy> m_Choice;
    std::int32_t m_RtPriority = DEFAULT_REALTIME_PRIORITY;
    bool m_OwnProcess = false;              // TaskSmack itself: never real time
    bool m_CurrentRtPriorityUnread = false; // On FIFO or RR at a priority not read (0): nothing chosen
    bool m_RealtimeNeedsNoPassword = false; // As SchedPolicyDialogOptions has it
    bool m_Confirming = false;              // The real-time confirm step is showing
    ImVec2 m_LastWorkSize;                  // For UI::Widgets::centerNextDialog()
    bool m_ShowRequested = false;
    bool m_DismissRequested = false;
    bool m_Open = false; // As of the last render()
};

/// The result line for setting @p setting on @p name (PID @p pid): "Scheduling policy of server (PID 4242) set
/// to Batch" or "... set to FIFO 50", or the platform's error. Shared by the row menu and Process Details' Actions card.
[[nodiscard]] Detail::ActionResultMessage schedPolicyResultMessage(const std::string& name,
                                                                   std::int32_t pid,
                                                                   const SchedPolicySetting& setting,
                                                                   const Platform::ProcessActionResult& result);

} // namespace App
