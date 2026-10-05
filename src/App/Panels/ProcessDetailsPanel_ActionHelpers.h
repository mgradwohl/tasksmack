#pragma once

// This header exposes process-action dispatch helper functions for testing and
// internal use. These were extracted from ProcessDetailsPanel::dispatchConfirmedAction()
// (and the private ProcessAction enum/actionVerb() it depended on) to give the
// mocked-IProcessActions integration tests requested in #415 a testable, pure entry
// point instead of poking at ProcessDetailsPanel's private members.

#include "Domain/ProcessSnapshot.h"
#include "Platform/IProcessActions.h"

#include <cstdint>
#include <format>
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
};

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
    case ProcessAction::None:
        break;
    }
    return "";
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
    case ProcessAction::None:
        break;
    }
    return "";
}

/// Confirm dialog title, "Kill firefox (PID 1234)?": the action, then the process it hits (#1203).
[[nodiscard]] inline std::string confirmTitle(ProcessAction action, std::string_view processName, std::int32_t pid)
{
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
