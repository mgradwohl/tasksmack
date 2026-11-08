#pragma once

// The Startup tab's scheduled tasks (#3424): the tasks with a logon or boot trigger, read through the Task
// Scheduler API (ITaskService, each folder's IRegisteredTask and its ITaskDefinition), and switched on or off
// through IRegisteredTask::put_Enabled. Never deleted. The COM calls are behind ScheduledTaskApi, so the walk,
// the trigger filter and the switch's refusals are tested with a fake (test_WindowsScheduledTasks.cpp) and
// never touch the real Task Scheduler.

#include "Platform/IStartupActions.h"
#include "Platform/IStartupProbe.h"
#include "WindowsScheduledTasksMath.h"

#include <atomic>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Platform::Windows
{

/// One task as read, or the HRESULT that says why it couldn't be.
using TaskReading = std::expected<ScheduledTaskMath::TaskRecord, std::int32_t>;

/// The Task Scheduler calls the Startup tab makes: the real COM API (makeTaskSchedulerApi), or a test's fake.
/// One is made, used and destroyed on one thread, inside a ComScope the caller holds, so every COM object it
/// holds is released before COM is uninitialised. Paths are full ("\Vendor\Updater"), UTF-8; results are
/// HRESULTs (S_OK is 0).
class ScheduledTaskApi
{
  public:
    virtual ~ScheduledTaskApi() = default;

    ScheduledTaskApi() = default;
    ScheduledTaskApi(const ScheduledTaskApi&) = delete;
    ScheduledTaskApi& operator=(const ScheduledTaskApi&) = delete;
    ScheduledTaskApi(ScheduledTaskApi&&) = delete;
    ScheduledTaskApi& operator=(ScheduledTaskApi&&) = delete;

    /// Connects to the local Task Scheduler as the current user.
    [[nodiscard]] virtual std::int32_t connect() = 0;
    /// The full paths of @p folder's own subfolders; empty when it has none or can't be read.
    [[nodiscard]] virtual std::vector<std::string> subfolders(const std::string& folder) = 0;
    /// The tasks registered in @p folder itself, hidden ones included; empty when it can't be read. A task whose
    /// enabled state can't be read is the HRESULT that says why, never a guess.
    [[nodiscard]] virtual std::vector<TaskReading> tasks(const std::string& folder) = 0;
    /// The task at @p path, or the HRESULT that says why it (or its enabled state) can't be read.
    [[nodiscard]] virtual TaskReading task(const std::string& path) = 0;
    /// Enables or disables the task at @p path (IRegisteredTask::put_Enabled).
    [[nodiscard]] virtual std::int32_t setEnabled(const std::string& path, bool enabled) = 0;
    /// The names a task's principal may give the current user by: "DOMAIN\user", "user" and the SID string.
    [[nodiscard]] virtual std::vector<std::string> currentUserNames() = 0;
};

/// Makes the API a probe or an action uses, once per use.
using ScheduledTaskApiFactory = std::function<std::unique_ptr<ScheduledTaskApi>()>;

/// The real Task Scheduler, through taskschd.dll's COM objects. COM must be initialised on the calling thread.
[[nodiscard]] std::unique_ptr<ScheduledTaskApi> makeTaskSchedulerApi();

/// Every task, in every folder from the root down (to ScheduledTaskMath::MAX_FOLDER_DEPTH), that starts at sign-in
/// or boot (an enabled logon or boot trigger); a folder that can't be read is skipped. Nullopt when the Task
/// Scheduler can't be connected to.
[[nodiscard]] std::optional<std::vector<ScheduledTaskMath::TaskRecord>> readStartupTasks(ScheduledTaskApi& api);

/// Enables or disables the task @p entry (as the probe listed it) names, by its path, after reading it again: one
/// of Windows' own (under \Microsoft\) is refused, as is one no longer registered; one already so succeeds without
/// a write. A refused write says why ("Requires administrator" for another account's task, not elevated). A
/// change is counted in scheduledTaskChanges(), so the probe reads the tasks again at its next sample.
[[nodiscard]] StartupActionResult setStartupTaskEnabled(ScheduledTaskApi& api, const StartupEntry& entry, bool enabled);

/// How many times TaskSmack has switched a scheduled task on or off: the probe walks the tasks again, ahead of its
/// slow cadence, when this changes.
[[nodiscard]] std::atomic<std::uint64_t>& scheduledTaskChanges() noexcept;

} // namespace Platform::Windows
