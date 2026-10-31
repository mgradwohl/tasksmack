#pragma once

// Replace Task Manager (#3072) as TaskSmack sees it: what taskmgr.exe's IFEO Debugger says, and the
// change to it the user asks for in Settings or in the startup notice. Windows only: the factory makes
// none on Linux (Platform::makeTaskManagerReplacement()). Both calls block (a registry read, a UAC prompt
// and the helper's answer), so App calls them on a worker, never the UI thread
// (App/TaskManagerReplacementController.h).

#include "Platform/BrokerProtocol.h"
#include "Platform/IProcessActions.h"
#include "Platform/TaskManagerReplacement.h"

#include <stop_token>
#include <string>

namespace Platform
{

/// What a read of the replacement found.
struct TaskManagerReplacementInfo
{
    TaskManagerReplacement::Status status;
    /// Why the value couldn't be read, when @c readFailed.
    std::string readError;
    /// The running TaskSmack.exe, the one a Set points Task Manager at.
    std::string selfPath;
    /// Why this copy can't change the replacement, or empty when it can.
    std::string unavailableReason;
    /// The value couldn't be read; @c status is then Off.
    bool readFailed = false;
    /// Whether the program the value starts exists (always true when Off or ThisCopy).
    bool otherProgramExists = true;
    /// Whether this copy is installed in Program Files (TaskManagerReplacement::isInstalledCopy()).
    bool installedCopy = false;
};

class ITaskManagerReplacement
{
  public:
    virtual ~ITaskManagerReplacement() = default;
    ITaskManagerReplacement() = default;
    ITaskManagerReplacement(const ITaskManagerReplacement&) = delete;
    ITaskManagerReplacement& operator=(const ITaskManagerReplacement&) = delete;
    ITaskManagerReplacement(ITaskManagerReplacement&&) = delete;
    ITaskManagerReplacement& operator=(ITaskManagerReplacement&&) = delete;

    /// Reads the Debugger value (no elevation needed) and sorts it against this copy. Blocking.
    [[nodiscard]] virtual TaskManagerReplacementInfo read() = 0;

    /// Points Task Manager at this copy (Set) or gives it back (Clear), as an administrator: through the
    /// helper, which may first raise a UAC prompt, or in-process when TaskSmack is elevated. Another
    /// program's Debugger is replaced only with @p replaceOther, once the user confirmed it. Blocking.
    [[nodiscard]] virtual ProcessActionResult
    change(Broker::TaskManagerChange change, bool replaceOther, const std::stop_token& stopToken) = 0;
};

} // namespace Platform
