#pragma once

// The Status ProcessModel publishes (ProcessSnapshot::status): the probe's own status ("Suspended",
// "Efficiency Mode") with "Not responding" for a process whose window has stopped answering (#3084), as
// Task Manager's Status column words it. Pure and header-only; tested in tests/Domain/test_ProcessStatus.cpp.

#include <string>
#include <string_view>

namespace Domain
{

/// The Status of a process whose window has stopped answering its messages (#3084).
inline constexpr std::string_view NOT_RESPONDING_STATUS = "Not responding";

/// What ProcessSnapshot::status says for a process the probe gave @p status, whose window has hung when
/// @p notResponding: "Not responding" first, the hang being what a user looks for, then any other status
/// after a middle dot ("Not responding · Efficiency Mode"). A suspended process stays "Suspended": its
/// windows stop answering because it is suspended, not because it hangs.
[[nodiscard]] inline std::string processStatusText(std::string_view status, bool notResponding)
{
    if (!notResponding || status == "Suspended")
    {
        return std::string(status);
    }
    std::string text(NOT_RESPONDING_STATUS);
    if (!status.empty())
    {
        text += " \xC2\xB7 "; // a middle dot, as Process Details joins its state and status
        text += status;
    }
    return text;
}

} // namespace Domain
