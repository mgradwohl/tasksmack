#pragma once

// This header exposes process-action dispatch helper functions for testing and
// internal use. These were extracted from ProcessDetailsPanel::dispatchConfirmedAction()
// (and the private ProcessAction enum/actionVerb() it depended on) to give the
// mocked-IProcessActions integration tests requested in #415 a testable, pure entry
// point instead of poking at ProcessDetailsPanel's private members.

#include "Platform/IProcessActions.h"

#include <cstdint>
#include <string>

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
[[nodiscard]] inline const char* actionVerb(ProcessAction action)
{
    switch (action)
    {
    case ProcessAction::Terminate:
        return "terminate";
    case ProcessAction::Kill:
        return "kill";
    case ProcessAction::Stop:
        return "stop";
    case ProcessAction::Resume:
        return "resume";
    case ProcessAction::None:
        break;
    }
    return "";
}

/// Dispatch a confirmed process action to the given IProcessActions implementation.
/// Pure aside from the call through `actions` - no ProcessDetailsPanel state is touched -
/// so it's directly testable with a mock IProcessActions.
[[nodiscard]] inline Platform::ProcessActionResult
dispatchProcessAction(Platform::IProcessActions& actions, ProcessAction action, std::int32_t pid)
{
    switch (action)
    {
    case ProcessAction::Terminate:
        return actions.terminate(pid);
    case ProcessAction::Kill:
        return actions.kill(pid);
    case ProcessAction::Stop:
        return actions.stop(pid);
    case ProcessAction::Resume:
        return actions.resume(pid);
    case ProcessAction::None:
        break;
    }
    // ProcessAction::None can't be reached today (ProcessDetailsPanel always sets the
    // confirmation action and the show-dialog flag together), but this keeps that
    // invariant from being a silent "reports success" bug if that ever changes.
    return Platform::ProcessActionResult::error("No action selected");
}

/// Format the same "Success: <verb> sent to PID <pid>" / "Error: <message>" text
/// ProcessDetailsPanel shows after a dispatched action, as a pure function of the
/// action, pid, and result.
[[nodiscard]] inline std::string
formatActionResultMessage(ProcessAction action, std::int32_t pid, const Platform::ProcessActionResult& result)
{
    if (result.success)
    {
        return std::string("Success: ") + actionVerb(action) + " sent to PID " + std::to_string(pid);
    }
    return "Error: " + result.errorMessage;
}

} // namespace App::Detail
