#pragma once

// The process actions' pure helpers: the ProcessAction enum, its verbs and labels, the confirm
// dialog's title and body, and the dispatch to IProcessActions and its result message, so the
// mocked-IProcessActions tests (#415) reach them without ProcessDetailsPanel's private members.

#include "Domain/ProcessSnapshot.h"
#include "Platform/IProcessActions.h"

#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>

namespace App::Detail
{

/// Action pending confirmation in the "Confirm Action" popup. An enum instead of a
/// free-form string gives compiler-checked exhaustiveness in the dispatch switch and
/// avoids a string-compare per confirmation-popup frame.
enum class ProcessAction : std::uint8_t
{
    None,
    Terminate,
    Kill,
    Stop,
    Resume,
    // The other signals (#3063, Linux): each its own action, so the confirm, run and result paths need no
    // extra parameter. signalFor() maps them to Platform::ProcessSignal.
    SignalHangup,
    SignalInterrupt,
    SignalQuit,
    SignalUser1,
    SignalUser2,
};

/// The signal a Signal* action sends (#3063), or nullopt for the other actions.
[[nodiscard]] constexpr std::optional<Platform::ProcessSignal> signalFor(ProcessAction action) noexcept
{
    switch (action)
    {
    case ProcessAction::SignalHangup:
        return Platform::ProcessSignal::Hangup;
    case ProcessAction::SignalInterrupt:
        return Platform::ProcessSignal::Interrupt;
    case ProcessAction::SignalQuit:
        return Platform::ProcessSignal::Quit;
    case ProcessAction::SignalUser1:
        return Platform::ProcessSignal::User1;
    case ProcessAction::SignalUser2:
        return Platform::ProcessSignal::User2;
    case ProcessAction::None:
    case ProcessAction::Terminate:
    case ProcessAction::Kill:
    case ProcessAction::Stop:
    case ProcessAction::Resume:
        break;
    }
    return std::nullopt;
}

/// The signal's name, "SIGHUP": a Signal* action's label (#3063); empty for the other actions.
[[nodiscard]] constexpr const char* signalName(ProcessAction action) noexcept
{
    switch (action)
    {
    case ProcessAction::SignalHangup:
        return "SIGHUP";
    case ProcessAction::SignalInterrupt:
        return "SIGINT";
    case ProcessAction::SignalQuit:
        return "SIGQUIT";
    case ProcessAction::SignalUser1:
        return "SIGUSR1";
    case ProcessAction::SignalUser2:
        return "SIGUSR2";
    case ProcessAction::None:
    case ProcessAction::Terminate:
    case ProcessAction::Kill:
    case ProcessAction::Stop:
    case ProcessAction::Resume:
        break;
    }
    return "";
}

/// Human-readable verb for ProcessAction, used in confirmation/result messages.
/// Returns a `const char*` (not std::string_view) since callers need a null-terminated
/// string for both ImGui::Text()'s printf-style "%s" and std::string concatenation.
///
/// ProcessAction::Stop is "suspend": it is the Suspend button, so the dialog and the result say
/// the same word instead of "stop" -- which reads as ending the process (#1203).
[[nodiscard]] inline const char* actionVerb(ProcessAction action)
{
    switch (action)
    {
    case ProcessAction::Terminate:
        return "terminate";
    case ProcessAction::Kill:
        return "kill";
    case ProcessAction::Stop:
        return "suspend";
    case ProcessAction::Resume:
        return "resume";
    case ProcessAction::SignalHangup:
        return "send SIGHUP to";
    case ProcessAction::SignalInterrupt:
        return "send SIGINT to";
    case ProcessAction::SignalQuit:
        return "send SIGQUIT to";
    case ProcessAction::SignalUser1:
        return "send SIGUSR1 to";
    case ProcessAction::SignalUser2:
        return "send SIGUSR2 to";
    case ProcessAction::None:
        break;
    }
    return "";
}

/// How long a result line offering "Authenticate and retry" stays up: long enough to read it and decide (#2308).
inline constexpr float AUTHORIZE_RESULT_SECONDS = 30.0F;

/// @p action on @p target as an administrator would redo it (#2308): "Authenticate and retry" after a
/// permission refusal. Nullopt for None.
[[nodiscard]] inline std::optional<Platform::AuthorizedAction> authorizedActionFor(ProcessAction action,
                                                                                   const Platform::ProcessTarget& target)
{
    using Kind = Platform::AuthorizedAction::Kind;
    Platform::AuthorizedAction authorized;
    authorized.target = target;
    switch (action)
    {
    case ProcessAction::Terminate:
        authorized.kind = Kind::Terminate;
        return authorized;
    case ProcessAction::Kill:
        authorized.kind = Kind::Kill;
        return authorized;
    case ProcessAction::Stop:
        authorized.kind = Kind::Stop;
        return authorized;
    case ProcessAction::Resume:
        authorized.kind = Kind::Resume;
        return authorized;
    case ProcessAction::SignalHangup:
    case ProcessAction::SignalInterrupt:
    case ProcessAction::SignalQuit:
    case ProcessAction::SignalUser1:
    case ProcessAction::SignalUser2:
        if (const std::optional<Platform::ProcessSignal> signal = signalFor(action))
        {
            authorized.kind = Kind::SendSignal;
            authorized.signal = *signal;
            return authorized;
        }
        break;
    case ProcessAction::None:
        break;
    }
    return std::nullopt;
}

/// The action as the result line says it while it runs (#2305): "Terminating", "Suspending".
[[nodiscard]] inline const char* actionProgressLabel(ProcessAction action)
{
    switch (action)
    {
    case ProcessAction::Terminate:
        return "Terminating";
    case ProcessAction::Kill:
        return "Killing";
    case ProcessAction::Stop:
        return "Suspending";
    case ProcessAction::Resume:
        return "Resuming";
    case ProcessAction::SignalHangup:
        return "Sending SIGHUP to";
    case ProcessAction::SignalInterrupt:
        return "Sending SIGINT to";
    case ProcessAction::SignalQuit:
        return "Sending SIGQUIT to";
    case ProcessAction::SignalUser1:
        return "Sending SIGUSR1 to";
    case ProcessAction::SignalUser2:
        return "Sending SIGUSR2 to";
    case ProcessAction::None:
        break;
    }
    return "Acting on";
}

/// The action's name as its button, the confirm dialog's title and its confirm button show it:
/// actionVerb() capitalised ("Kill", "Suspend"), so all three use one word (#1203).
[[nodiscard]] inline const char* actionLabel(ProcessAction action)
{
    switch (action)
    {
    case ProcessAction::Terminate:
        return "Terminate";
    case ProcessAction::Kill:
        return "Kill";
    case ProcessAction::Stop:
        return "Suspend";
    case ProcessAction::Resume:
        return "Resume";
    case ProcessAction::SignalHangup:
    case ProcessAction::SignalInterrupt:
    case ProcessAction::SignalQuit:
    case ProcessAction::SignalUser1:
    case ProcessAction::SignalUser2:
        return signalName(action); // "SIGHUP sent to PID 1234"
    case ProcessAction::None:
        break;
    }
    return "";
}

/// True for the actions that end the process (Terminate, Kill), which can lose its unsaved work and
/// cannot be undone. Their buttons are drawn in the theme's danger colour; Suspend and Resume, which
/// undo each other, are not (#1273).
[[nodiscard]] constexpr bool isDestructiveAction(ProcessAction action) noexcept
{
    return action == ProcessAction::Terminate || action == ProcessAction::Kill;
}

/// Confirm dialog title, "Kill firefox (PID 1234)?": the action, then the process it hits (#1203).
[[nodiscard]] inline std::string confirmTitle(ProcessAction action, std::string_view processName, std::int32_t pid)
{
    if (signalFor(action).has_value())
    {
        return std::format("Send {} to {} (PID {})?", signalName(action), processName, pid); // #3063
    }
    return std::format("{} {} (PID {})?", actionLabel(action), processName, pid);
}

/// Confirm dialog body: what the action does to this process, so the outcome of a destructive
/// action is stated rather than hidden behind "Are you sure?" (#1203).
[[nodiscard]] inline std::string confirmBody(ProcessAction action, std::string_view processName, std::int32_t pid)
{
    switch (action)
    {
    case ProcessAction::Terminate:
        return std::format("{} (PID {}) will be asked to exit. It can save its work first, or refuse.", processName, pid);
    case ProcessAction::Kill:
        return std::format("{} (PID {}) will end immediately, without saving its work.", processName, pid);
    case ProcessAction::Stop:
        return std::format("{} (PID {}) will stop running until it is resumed.", processName, pid);
    case ProcessAction::Resume:
        return std::format("{} (PID {}) will continue running.", processName, pid);
    case ProcessAction::SignalHangup:
        return std::format("{} (PID {}) gets SIGHUP. A daemon usually reloads its configuration; a program with no handler for it ends.",
                           processName,
                           pid);
    case ProcessAction::SignalInterrupt:
        return std::format("{} (PID {}) gets SIGINT, as Ctrl+C sends: most programs stop what they're doing and exit.", processName, pid);
    case ProcessAction::SignalQuit:
        return std::format("{} (PID {}) gets SIGQUIT: most programs exit and leave a core dump.", processName, pid);
    case ProcessAction::SignalUser1:
    case ProcessAction::SignalUser2:
        return std::format("{} (PID {}) gets {}, which runs its own handler for it if it has one; a program with none ends.",
                           processName,
                           pid,
                           signalName(action));
    case ProcessAction::None:
        break;
    }
    return {};
}

/// The process an action on the current selection is meant for.
///
/// The start time comes only from a snapshot already confirmed to be of the selected process (the
/// pane checks the unique key before caching one). Without such a snapshot it is left unknown, and
/// the platform refuses the action rather than send it to whatever holds the PID (#973).
///
/// @param selectedPid        PID of the selected process.
/// @param confirmedSnapshot  Latest snapshot of that same process, or nullptr if there is none.
[[nodiscard]] inline Platform::ProcessTarget targetForSelection(std::int32_t selectedPid, const Domain::ProcessSnapshot* confirmedSnapshot)
{
    return {.pid = selectedPid, .startTimeTicks = (confirmedSnapshot != nullptr) ? confirmedSnapshot->startTimeTicks : 0};
}

/// Dispatch a confirmed process action to the given IProcessActions implementation.
/// Pure aside from the call through `actions` - no ProcessDetailsPanel state is touched -
/// so it's directly testable with a mock IProcessActions.
[[nodiscard]] inline Platform::ProcessActionResult
dispatchProcessAction(Platform::IProcessActions& actions, ProcessAction action, const Platform::ProcessTarget& target)
{
    switch (action)
    {
    case ProcessAction::Terminate:
        return actions.terminate(target);
    case ProcessAction::Kill:
        return actions.kill(target);
    case ProcessAction::Stop:
        return actions.stop(target);
    case ProcessAction::Resume:
        return actions.resume(target);
    case ProcessAction::SignalHangup:
    case ProcessAction::SignalInterrupt:
    case ProcessAction::SignalQuit:
    case ProcessAction::SignalUser1:
    case ProcessAction::SignalUser2:
        if (const std::optional<Platform::ProcessSignal> signal = signalFor(action))
        {
            return actions.sendSignal(target, *signal);
        }
        break;
    case ProcessAction::None:
        break;
    }
    // ProcessAction::None can't be reached today (ProcessDetailsPanel always sets the
    // confirmation action and the show-dialog flag together), but this keeps that
    // invariant from being a silent "reports success" bug if that ever changes.
    return Platform::ProcessActionResult::error("No action selected");
}

/// The feedback line ProcessDetailsPanel shows after a dispatched action. The outcome travels as
/// `ok` rather than being recovered by searching the text for "Error"/"Failed", which a platform
/// error message or a process name could make wrong either way (#1203).
struct ActionResultMessage
{
    bool ok = false;
    std::string text; ///< Empty when there is nothing to show.

    [[nodiscard]] bool empty() const noexcept
    {
        return text.empty();
    }
};

/// "Suspend sent to PID 321" / "Could not suspend PID 321: Operation not permitted", as a pure
/// function of the action, pid, and result.
[[nodiscard]] inline ActionResultMessage
formatActionResultMessage(ProcessAction action, std::int32_t pid, const Platform::ProcessActionResult& result)
{
    if (result.success)
    {
        return {.ok = true, .text = std::format("{} sent to PID {}", actionLabel(action), pid)};
    }
    return {.ok = false, .text = std::format("Could not {} PID {}: {}", actionVerb(action), pid, result.errorMessage)};
}

} // namespace App::Detail
