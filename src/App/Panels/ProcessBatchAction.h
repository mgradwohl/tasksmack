#pragma once

// Batch process actions over the Processes table's multi-selection (#804): the confirmation's text,
// the run over every selected process, and the one-line summary of how it went. Pure aside from the
// calls through IProcessActions, so it is unit-tested with MockProcessActions
// (test_ProcessBatchAction.cpp). The single-process wording and dispatch it builds on are in
// ProcessDetailsPanel_ActionHelpers.h.
//
// Each process is acted on by its ProcessTarget (PID and start time), exactly as a single action is:
// the platform refuses any whose PID now belongs to another process (#973), and that refusal is
// reported as one of the batch's failures.

#include "App/Panels/ProcessDetailsPanel_ActionHelpers.h"
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

/// One process a batch action is for: its identity, and its name for the dialog and the result.
struct BatchTarget
{
    Platform::ProcessTarget target;
    std::string name;
};

/// Whether @p pid is one the confirmation must always name, never fold into "and N more": TaskSmack's
/// own process (@p ownPid) or PID 1. Neither is refused -- the single-process actions are not -- but
/// acting on either has consequences the user must see before confirming.
[[nodiscard]] constexpr bool isNotableTarget(std::int32_t pid, std::int32_t ownPid) noexcept
{
    return pid == INIT_PID || (ownPid > 0 && pid == ownPid);
}

/// The selected processes still listed in @p snapshots, as targets, in @p snapshots' order. @p isSelected
/// is asked with each snapshot's uniqueKey (an O(1) set lookup in ProcessesPanel). A selected process
/// that has exited is simply not found, so it is never acted on.
template<typename SnapshotRange, typename SelectedPredicate>
[[nodiscard]] std::vector<BatchTarget> resolveTargets(const SnapshotRange& snapshots, SelectedPredicate isSelected)
{
    std::vector<BatchTarget> targets;
    for (const auto& snapshot : snapshots)
    {
        if (isSelected(snapshot.uniqueKey))
        {
            targets.push_back({.target = {.pid = snapshot.pid, .startTimeTicks = snapshot.startTimeTicks}, .name = snapshot.name});
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

/// The confirmation's body: the summary, then the processes by name and PID -- TaskSmack itself and
/// PID 1 first and always, then the others up to CONFIRM_LIST_LIMIT names in all, then "and N more" --
/// then a line for each of TaskSmack itself and PID 1 that is among them. @p ownPid is TaskSmack's own
/// PID (0 when unknown).
[[nodiscard]] inline std::string confirmBody(Detail::ProcessAction action, std::span<const BatchTarget> targets, std::int32_t ownPid)
{
    std::string body = confirmSummary(action, targets.size());
    body += '\n';
    std::size_t listed = 0;
    const BatchTarget* self = nullptr;
    const BatchTarget* init = nullptr;
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
                init = &t;
            }
            if (ownPid > 0 && t.target.pid == ownPid)
            {
                self = &t;
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
    if (self != nullptr)
    {
        std::format_to(std::back_inserter(body),
                       "\n\nThis includes TaskSmack itself (PID {}). It is {} last.",
                       self->target.pid,
                       action == Detail::ProcessAction::Resume ? "resumed" : "acted on");
    }
    if (init != nullptr)
    {
        std::format_to(std::back_inserter(body), "\n\nThis includes PID 1 ({}), the system's init process.", init->name);
    }
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

/// Sends @p action to every target through @p actions, one ProcessTarget at a time, each checked by
/// the platform exactly as a single action is. TaskSmack's own process (@p ownPid) goes last, so a Kill
/// or Terminate that ends TaskSmack has reached every other target first.
[[nodiscard]] inline BatchResult
runBatchAction(Platform::IProcessActions& actions, Detail::ProcessAction action, std::span<const BatchTarget> targets, std::int32_t ownPid)
{
    BatchResult result;
    const auto runOne = [&](const BatchTarget& t)
    {
        ++result.attempted;
        const Platform::ProcessActionResult r = Detail::dispatchProcessAction(actions, action, t.target);
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
    return {.ok = false, .text = std::move(text)};
}

} // namespace App::ProcessBatch
