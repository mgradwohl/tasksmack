#pragma once

// Batch process actions over the Processes table's multi-selection (#804): the confirmation's text,
// the run over every selected process, and the one-line summary of how it went. Pure aside from the
// calls through IProcessActions, so it is unit-tested with MockProcessActions
// (test_ProcessBatchAction.cpp). The single-process wording and dispatch it builds on are in
// ProcessDetailsPanel_ActionHelpers.h. A batch priority change (#1484) runs the same way, with the
// nice value carried alongside (runBatchPriority()).
//
// Each process is acted on by its ProcessTarget (PID and start time), exactly as a single action is:
// the platform refuses any whose PID now belongs to another process (#973), and that refusal is
// reported as one of the batch's failures.

#include "App/Panels/ProcessDetailsPanel_ActionHelpers.h"
#include "App/Panels/ProcessDetailsPanel_PriorityHelpers.h"
#include "Domain/PriorityConfig.h"
#include "Platform/IProcessActions.h"

#include <cstddef>
#include <cstdint>
#include <format>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace App::ProcessBatch
{

/// At most this many processes are named in the confirmation; the rest are counted ("and 12 more").
/// TaskSmack itself and PID 1 are named whatever the count (isNotableTarget()).
inline constexpr std::size_t CONFIRM_LIST_LIMIT = 8;

/// At most this many failures are quoted in the result line; the rest are counted.
inline constexpr std::size_t RESULT_FAILURE_LIMIT = 3;

/// The system's init process (systemd and the like) on Linux.
inline constexpr std::int32_t INIT_PID = 1;

/// One process a batch action is for: its identity, its name for the dialog and the result, and its
/// priority when it was picked (the batch priority dialog's list, #1539; empty when not known).
struct BatchTarget
{
    Platform::ProcessTarget target;
    std::string name;
    std::string priority;
};

/// Whether @p pid is one the confirmation must always name, never fold into "and N more": TaskSmack's
/// own process (@p ownPid) or PID 1. Neither is refused -- the single-process actions are not -- but
/// acting on either has consequences the user must see before confirming.
[[nodiscard]] constexpr bool isNotableTarget(std::int32_t pid, std::int32_t ownPid) noexcept
{
    return pid == INIT_PID || (ownPid > 0 && pid == ownPid);
}

/// The selected processes still listed in @p snapshots, as targets, in @p snapshots' order. @p isSelected
/// is asked with each snapshot's exact identity, PID and start time (an O(1) set lookup in
/// ProcessesPanel). A selected process that has exited is simply not found, so it is never acted on,
/// and no other process can stand in for it: not one given its PID, and not one whose uniqueKey hash
/// happens to match, since the hash is never what is compared (#1503).
template<typename SnapshotRange, typename SelectedPredicate>
[[nodiscard]] std::vector<BatchTarget> resolveTargets(const SnapshotRange& snapshots, SelectedPredicate isSelected)
{
    std::vector<BatchTarget> targets;
    for (const auto& snapshot : snapshots)
    {
        const Platform::ProcessTarget target{.pid = snapshot.pid, .startTimeTicks = snapshot.startTimeTicks};
        if (isSelected(target))
        {
            targets.push_back({.target = target,
                               .name = snapshot.name,
                               .priority = std::string(Domain::Priority::getProcessPriorityLabel(snapshot.priorityClass, snapshot.nice))});
        }
    }
    return targets;
}

/// "Kill 5 processes?"
[[nodiscard]] inline std::string confirmTitle(Detail::ProcessAction action, std::size_t count)
{
    return std::format("{} {} processes?", Detail::actionLabel(action), count);
}

/// What the action does to these processes, as the single confirmation states it for one (#1203).
[[nodiscard]] inline std::string confirmSummary(Detail::ProcessAction action, std::size_t count)
{
    switch (action)
    {
    case Detail::ProcessAction::Terminate:
        return std::format("{} processes will be asked to exit. Each can save its work first, or refuse.", count);
    case Detail::ProcessAction::Kill:
        return std::format("{} processes will end immediately, without saving their work.", count);
    case Detail::ProcessAction::Stop:
        return std::format("{} processes will stop running until they are resumed.", count);
    case Detail::ProcessAction::Resume:
        return std::format("{} processes will continue running.", count);
    case Detail::ProcessAction::None:
        break;
    }
    return {};
}

/// Which of TaskSmack itself and PID 1 appendTargetList() listed (null: not among the targets).
struct NotableTargets
{
    const BatchTarget* self = nullptr;
    const BatchTarget* init = nullptr;
};

/// Appends the processes to @p body by name and PID, a line each -- TaskSmack itself and PID 1 first
/// and always, then the others up to CONFIRM_LIST_LIMIT names in all, then "and N more" -- and returns
/// which of TaskSmack itself and PID 1 are among them. @p ownPid is TaskSmack's own PID (0 when
/// unknown).
inline NotableTargets appendTargetList(std::string& body, std::span<const BatchTarget> targets, std::int32_t ownPid)
{
    std::size_t listed = 0;
    NotableTargets notable;
    for (const BatchTarget& t : targets)
    {
        if (isNotableTarget(t.target.pid, ownPid))
        {
            std::format_to(std::back_inserter(body), "\n    {} (PID {})", t.name, t.target.pid);
            ++listed;
            // Independent: TaskSmack can itself be PID 1 (the init of a PID namespace), and then
            // both warnings apply (#804 review).
            if (t.target.pid == INIT_PID)
            {
                notable.init = &t;
            }
            if (ownPid > 0 && t.target.pid == ownPid)
            {
                notable.self = &t;
            }
        }
    }
    for (const BatchTarget& t : targets)
    {
        if (listed >= CONFIRM_LIST_LIMIT)
        {
            break;
        }
        if (!isNotableTarget(t.target.pid, ownPid))
        {
            std::format_to(std::back_inserter(body), "\n    {} (PID {})", t.name, t.target.pid);
            ++listed;
        }
    }
    if (listed < targets.size())
    {
        std::format_to(std::back_inserter(body), "\n    and {} more", targets.size() - listed);
    }
    return notable;
}

/// The processes a list names, in order -- TaskSmack itself and PID 1 first and always, then the
/// others up to @p limit in all -- and how many it leaves out: the batch priority dialog's list (#1539),
/// the same choice appendTargetList() makes for a confirmation's text.
struct ListedTargets
{
    std::vector<const BatchTarget*> listed;
    std::size_t more = 0;
    NotableTargets notable;
};

[[nodiscard]] inline ListedTargets
listTargets(std::span<const BatchTarget> targets, std::int32_t ownPid, std::size_t limit = CONFIRM_LIST_LIMIT)
{
    ListedTargets out;
    for (const BatchTarget& t : targets)
    {
        if (isNotableTarget(t.target.pid, ownPid))
        {
            out.listed.push_back(&t);
            if (t.target.pid == INIT_PID)
            {
                out.notable.init = &t;
            }
            if (ownPid > 0 && t.target.pid == ownPid)
            {
                out.notable.self = &t;
            }
        }
    }
    for (const BatchTarget& t : targets)
    {
        if (out.listed.size() >= limit)
        {
            break;
        }
        if (!isNotableTarget(t.target.pid, ownPid))
        {
            out.listed.push_back(&t);
        }
    }
    out.more = targets.size() - out.listed.size();
    return out;
}

/// Appends a paragraph for each of TaskSmack itself and PID 1 that @p notable has: "This includes
/// TaskSmack itself (PID 4000). It is {selfDone} last." and "This includes PID 1 (systemd), ...".
inline void appendNotableWarnings(std::string& body, const NotableTargets& notable, std::string_view selfDone)
{
    if (notable.self != nullptr)
    {
        std::format_to(
            std::back_inserter(body), "\n\nThis includes TaskSmack itself (PID {}). It is {} last.", notable.self->target.pid, selfDone);
    }
    if (notable.init != nullptr)
    {
        std::format_to(std::back_inserter(body), "\n\nThis includes PID 1 ({}), the system's init process.", notable.init->name);
    }
}

/// The confirmation's body: the summary, then the processes by name and PID (appendTargetList()), then
/// a line for each of TaskSmack itself and PID 1 that is among them. @p ownPid is TaskSmack's own PID
/// (0 when unknown).
[[nodiscard]] inline std::string confirmBody(Detail::ProcessAction action, std::span<const BatchTarget> targets, std::int32_t ownPid)
{
    std::string body = confirmSummary(action, targets.size());
    body += '\n';
    const NotableTargets notable = appendTargetList(body, targets, ownPid);
    appendNotableWarnings(body, notable, action == Detail::ProcessAction::Resume ? "resumed" : "acted on");
    return body;
}

/// One process the action could not be sent to, and why.
struct BatchFailure
{
    std::int32_t pid = 0;
    std::string name;
    std::string error;
};

/// How a batch went: how many were attempted and succeeded, and the first RESULT_FAILURE_LIMIT
/// failures (failed() counts them all).
struct BatchResult
{
    std::size_t attempted = 0;
    std::size_t succeeded = 0;
    std::vector<BatchFailure> firstFailures;

    [[nodiscard]] std::size_t failed() const noexcept
    {
        return attempted - succeeded;
    }
};

/// Calls @p actOnTarget (a ProcessTarget in, a ProcessActionResult out) for every target, one
/// ProcessTarget at a time, each checked by the platform exactly as a single action is, and tallies
/// the results. TaskSmack's own process (@p ownPid) goes last, so an action that ends TaskSmack has
/// reached every other target first.
template<typename ActOnTarget>
[[nodiscard]] BatchResult runBatch(std::span<const BatchTarget> targets, std::int32_t ownPid, const ActOnTarget& actOnTarget)
{
    BatchResult result;
    const auto runOne = [&](const BatchTarget& t)
    {
        ++result.attempted;
        const Platform::ProcessActionResult r = actOnTarget(t.target);
        if (r.success)
        {
            ++result.succeeded;
        }
        else if (result.firstFailures.size() < RESULT_FAILURE_LIMIT)
        {
            result.firstFailures.push_back({.pid = t.target.pid, .name = t.name, .error = r.errorMessage});
        }
    };
    const auto isSelf = [ownPid](const BatchTarget& t)
    {
        return ownPid > 0 && t.target.pid == ownPid;
    };
    for (const BatchTarget& t : targets)
    {
        if (!isSelf(t))
        {
            runOne(t);
        }
    }
    for (const BatchTarget& t : targets)
    {
        if (isSelf(t))
        {
            runOne(t);
        }
    }
    return result;
}

/// Sends @p action to every target through @p actions (runBatch()): a Kill or Terminate that ends
/// TaskSmack has reached every other target first.
[[nodiscard]] inline BatchResult
runBatchAction(Platform::IProcessActions& actions, Detail::ProcessAction action, std::span<const BatchTarget> targets, std::int32_t ownPid)
{
    return runBatch(targets,
                    ownPid,
                    [&actions, action](const Platform::ProcessTarget& target)
                    { return Detail::dispatchProcessAction(actions, action, target); });
}

/// Appends @p result's quoted failures to @p text -- ": PID 1 (systemd): Operation not permitted;
/// ..." -- and counts the rest ("; and 2 more").
inline void appendFailures(std::string& text, const BatchResult& result)
{
    std::string_view separator = ": ";
    for (const BatchFailure& f : result.firstFailures)
    {
        std::format_to(std::back_inserter(text), "{}PID {} ({}): {}", separator, f.pid, f.name, f.error);
        separator = "; ";
    }
    if (result.failed() > result.firstFailures.size())
    {
        std::format_to(std::back_inserter(text), "; and {} more", result.failed() - result.firstFailures.size());
    }
}

/// The toolbar's one-line result: "Kill sent to 5 processes", or "Kill sent to 3 of 5 processes; 2
/// failed: PID 1 (systemd): Operation not permitted; ...", or "Could not kill 5 processes: ...". The
/// first RESULT_FAILURE_LIMIT failures are quoted and the rest counted. `ok` only when every one
/// succeeded.
[[nodiscard]] inline Detail::ActionResultMessage formatBatchResultMessage(Detail::ProcessAction action, const BatchResult& result)
{
    if (result.failed() == 0)
    {
        return {.ok = true, .text = std::format("{} sent to {} processes", Detail::actionLabel(action), result.attempted)};
    }
    std::string text = (result.succeeded == 0) ? std::format("Could not {} {} processes", Detail::actionVerb(action), result.attempted)
                                               : std::format("{} sent to {} of {} processes; {} failed",
                                                             Detail::actionLabel(action),
                                                             result.succeeded,
                                                             result.attempted,
                                                             result.failed());
    appendFailures(text, result);
    return {.ok = false, .text = std::move(text)};
}

// ---------------------------------------------------------------------------------------------------
// Batch priority (#1484): one nice value -- a priority class on Windows -- set on every selected
// process, picked and confirmed in one dialog that lists them (ProcessBatchPriorityDialog, #1539), and
// summarised in the same one line as the actions above.
// ---------------------------------------------------------------------------------------------------

/// Whether the Processes table's row menu offers "Set priority for N processes...": only for a row of a
/// multi-selection (@p batchCount, 0 for a single row, whose priority stays in Process Details), and only
/// where the platform can set priority.
[[nodiscard]] constexpr bool offersBatchPriority(const Platform::ProcessActionCapabilities& capabilities, std::size_t batchCount) noexcept
{
    return batchCount > 1 && capabilities.canSetPriority;
}

/// "1 process", "5 processes".
[[nodiscard]] inline std::string processCountText(std::size_t count)
{
    return std::format("{} {}", count, count == 1 ? "process" : "processes");
}

/// The priority a batch sets, as it will be applied: the class name on Windows, where setPriority()
/// maps the nice value to a priority class (#1204), so that class is what the processes get; the label
/// with its nice value elsewhere ("Below Normal (nice: 10)"). @p nice is held to the nice range first,
/// as runBatchPriority() holds it.
[[nodiscard]] inline std::string priorityValueText(std::int32_t nice, bool windowsClasses = Detail::PRIORITY_USES_WINDOWS_CLASSES)
{
    return Detail::priorityDisplayText(Domain::Priority::clampNice(nice), windowsClasses);
}

/// Sets @p nice, held to the nice range, on every target through @p actions (runBatch()), TaskSmack's
/// own process last.
[[nodiscard]] inline BatchResult
runBatchPriority(Platform::IProcessActions& actions, std::span<const BatchTarget> targets, std::int32_t nice, std::int32_t ownPid)
{
    const std::int32_t clamped = Domain::Priority::clampNice(nice);
    return runBatch(
        targets, ownPid, [&actions, clamped](const Platform::ProcessTarget& target) { return actions.setPriority(target, clamped); });
}

/// The toolbar's one-line result for a batch priority change, naming what was applied: "Priority set to
/// Below Normal (nice: 10) for 5 processes", or "Priority set to ... for 3 of 5 processes; 2 failed:
/// PID 1 (systemd): Permission denied; ...", or "Could not set priority to High (nice: -15) for 5
/// processes: ...". The first RESULT_FAILURE_LIMIT failures are quoted and the rest counted. `ok` only
/// when every one succeeded.
[[nodiscard]] inline Detail::ActionResultMessage
formatBatchPriorityResultMessage(std::int32_t nice, const BatchResult& result, bool windowsClasses = Detail::PRIORITY_USES_WINDOWS_CLASSES)
{
    const std::string value = priorityValueText(nice, windowsClasses);
    if (result.failed() == 0)
    {
        return {.ok = true, .text = std::format("Priority set to {} for {}", value, processCountText(result.attempted))};
    }
    std::string text = (result.succeeded == 0)
                         ? std::format("Could not set priority to {} for {}", value, processCountText(result.attempted))
                         : std::format("Priority set to {} for {} of {}; {} failed",
                                       value,
                                       result.succeeded,
                                       processCountText(result.attempted),
                                       result.failed());
    appendFailures(text, result);
    return {.ok = false, .text = std::move(text)};
}

} // namespace App::ProcessBatch
