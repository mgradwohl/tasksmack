// The Processes table's actions on a selection or a tree (#3815): the batch priority and I/O priority dialogs, their
// outcome, and the Run task, Find handles, affinity and scheduling policy dialogs. Split from ProcessesPanel.cpp.

#include "App/PanelPlatforms.h"
#include "App/Panels/FindHandlesDialog.h"
#include "App/Panels/ProcessAffinityDialog.h"
#include "App/Panels/ProcessBatchAction.h"
#include "App/Panels/ProcessBatchRun.h"
#include "App/Panels/ProcessDetailsPanel_ActionHelpers.h"
#include "App/Panels/ProcessSchedPolicyDialog.h"
#include "App/Panels/ProcessSelection.h"
#include "App/Panels/ProcessesPanel.h"
#include "App/Panels/ProcessesPanelActions.h"
#include "App/Panels/RefusalOffer.h"
#include "App/Panels/RunTaskDialog.h"
#include "App/Panels/RunTaskHistory.h"
#include "App/UserConfig.h"
#include "Core/ApplicationEvents.h"
#include "Domain/HandleSearch.h"
#include "Domain/ProcessSnapshot.h"
#include "Platform/CpuAffinity.h"
#include "Platform/IProcessActions.h"
#include "Platform/IoPriorityTypes.h"

// clang-format off
#include <imgui.h>
#include <imgui_internal.h> // ImGuiTable: the laid-out column widths, and the table background channel, have no public accessor
#include <misc/cpp/imgui_stdlib.h>
#include <spdlog/spdlog.h>
// clang-format on

#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace App
{
void ProcessesPanel::requestSelectionAction(Detail::ProcessAction action)
{
    // The selected processes still listed, each by PID and start time (#973): one that has exited since
    // it was selected is not among them, and one whose PID is reused is refused by the platform.
    std::vector<ProcessBatch::BatchTarget> targets = ProcessBatch::resolveTargets(
        *m_CachedRenderSnapshots, [this](const Platform::ProcessTarget& target) { return m_Selection.contains(target); });
    if (targets.empty())
    {
        return;
    }
    if (targets.size() == 1)
    {
        const ProcessBatch::BatchTarget& only = targets.front();
        m_RowAction.title = Detail::confirmTitle(action, only.name, only.target.pid);
        m_RowAction.question = Detail::confirmBody(action, only.name, only.target.pid);
    }
    else
    {
        m_RowAction.title = ProcessBatch::confirmTitle(action, targets.size());
        m_RowAction.question = ProcessBatch::confirmBody(action, targets, m_OwnPid);
    }
    m_RowAction.action = action;
    m_RowAction.targets = std::move(targets);
    m_ShowRowActionConfirm = true;
}

void ProcessesPanel::requestTreeAction(Detail::ProcessAction action, const Domain::ProcessSnapshot& proc)
{
    std::vector<ProcessBatch::BatchTarget> targets =
        ProcessBatch::resolveTreeTargets(*m_CachedRenderSnapshots, {.pid = proc.pid, .startTimeTicks = proc.startTimeTicks});
    if (targets.size() < 2)
    {
        requestRowAction(action, proc); // its children have exited since the menu opened
        return;
    }
    m_RowAction.title = ProcessBatch::treeConfirmTitle(proc.name, proc.pid, targets.size());
    m_RowAction.question = ProcessBatch::confirmBody(action, targets, m_OwnPid);
    m_RowAction.action = action;
    m_RowAction.targets = std::move(targets);
    m_ShowRowActionConfirm = true;
}

void ProcessesPanel::requestSelectionPriority()
{
    // As requestSelectionAction(): the selected processes still listed, each by PID and start time, so
    // one that has exited is not among them and a reused PID is refused when the priority is set.
    std::vector<ProcessBatch::BatchTarget> targets = ProcessBatch::resolveTargets(
        *m_CachedRenderSnapshots, [this](const Platform::ProcessTarget& target) { return m_Selection.contains(target); });
    if (targets.empty())
    {
        return;
    }
    m_BatchPriorityDialog.open(std::move(targets), m_OwnPid);
}

void ProcessesPanel::renderBatchPriorityDialog()
{
    const std::optional<std::int32_t> nice = m_BatchPriorityDialog.render();
    if (!nice.has_value())
    {
        return;
    }
    // Apply: the dialog listed these processes, so it was the confirmation (#1539). Every one in turn,
    // by identity, TaskSmack itself last, on the batch's worker (#2142); one summary line naming the
    // priority applied (#1484) when it finishes.
    if (!m_ProcessActions)
    {
        showBatchOutcome({.message = {.ok = false, .text = "Process actions unavailable"}, .anySucceeded = false});
        return;
    }
    // A process Process Details is changing the priority of is left out, and the result line says so
    // (#3467): set now as well, the older of the two changes could land last.
    std::vector<ProcessBatch::BatchTarget> targets;
    std::size_t skipped = 0;
    for (const ProcessBatch::BatchTarget& target : m_BatchPriorityDialog.targets())
    {
        if (priorityHeldElsewhere(target.target))
        {
            ++skipped;
        }
        else
        {
            targets.push_back(target);
        }
    }
    if (targets.empty())
    {
        showBatchOutcome({
            .message =
                {
                    .ok = false,
                    .text = std::format("Priority not set{}", ProcessesPanelActions::batchPrioritySkippedText(skipped)),
                },
            .anySucceeded = false,
        });
        return;
    }
    if (m_BatchRun.busy())
    {
        return; // startPriority() starts nothing while another batch runs, and the menu offers none meanwhile
    }
    std::vector<Platform::ProcessTarget> identities;
    identities.reserve(targets.size());
    for (const ProcessBatch::BatchTarget& target : targets)
    {
        identities.push_back(target.target);
    }
    if (std::optional<ProcessBatch::BatchRunOutcome> failed =
            m_BatchRun.startPriority(*m_ProcessActions, std::move(targets), *nice, m_OwnPid);
        failed.has_value())
    {
        showBatchOutcome(std::move(*failed));
        return;
    }
    m_BatchPriorityTargets = std::move(identities);
    m_BatchPrioritySkipped = skipped;
}

void ProcessesPanel::requestSelectionIoPriority()
{
    // As requestSelectionPriority(), with each process's current I/O priority for the dialog's list.
    std::vector<ProcessBatch::BatchTarget> targets = ProcessBatch::resolveIoPriorityTargets(
        *m_CachedRenderSnapshots, [this](const Platform::ProcessTarget& target) { return m_Selection.contains(target); });
    if (targets.empty())
    {
        return;
    }
    // The slider Process Details draws: Realtime only where it can be set, Windows' hints there. Asked now, not
    // from m_ActionCapabilities: authenticating on Linux connects the helper later, which can set Realtime
    // (#3566's review). actionCapabilities() is cheap and never blocks, as Process Details reads it each frame.
    const Platform::ProcessActionCapabilities can = m_ProcessActions ? m_ProcessActions->actionCapabilities() : m_ActionCapabilities;
    m_BatchIoPriorityDialog.open(std::move(targets), m_OwnPid, can.canSetRealtimeIoPriority, can.ioPriorityHints);
}

void ProcessesPanel::renderBatchIoPriorityDialog()
{
    const std::optional<Platform::IoPriority> priority = m_BatchIoPriorityDialog.render();
    if (!priority.has_value())
    {
        return;
    }
    // Apply: the dialog listed these processes, so it was the confirmation. Every one in turn, by
    // identity, TaskSmack itself last, on the batch's worker (#2142); one summary line naming the I/O
    // priority applied when it finishes.
    if (!m_ProcessActions)
    {
        showBatchOutcome({.message = {.ok = false, .text = "Process actions unavailable"}, .anySucceeded = false});
        return;
    }
    if (m_BatchRun.busy())
    {
        return; // startIoPriority() starts nothing while another batch runs, and the menu offers none meanwhile
    }
    std::vector<ProcessBatch::BatchTarget> targets(m_BatchIoPriorityDialog.targets().begin(), m_BatchIoPriorityDialog.targets().end());
    if (std::optional<ProcessBatch::BatchRunOutcome> failed =
            m_BatchRun.startIoPriority(*m_ProcessActions, std::move(targets), *priority, m_OwnPid);
        failed.has_value())
    {
        showBatchOutcome(std::move(*failed));
    }
}

void ProcessesPanel::showBatchOutcome(ProcessBatch::BatchRunOutcome outcome)
{
    m_RowWithheldPath.clear();
    m_RowActionResult = std::move(outcome.message);
    m_RowOffer.showLine(ProcessesPanelActions::ROW_ACTION_RESULT_SECONDS); // the last row action's offer goes with its line
    if (outcome.anySucceeded)
    {
        requestRefresh(); // Show the processes acted on without waiting for the next sample
    }
}

void ProcessesPanel::renderRunTaskDialog()
{
    const std::optional<Platform::RunProgramRequest> request = m_RunTaskDialog.render();
    if (!request.has_value())
    {
        return;
    }
    // Remembered whether or not it starts: a typo is easier to fix from the list than to retype.
    UserConfig::get().settings().recentCommands =
        RunTaskHistory::remember(UserConfig::get().settings().recentCommands, request->commandLine);
    std::string shown = runTaskShortName(request->commandLine);
    runRowAction(
        std::format("Starting {}...", shown),
        [actions = m_ProcessActions.get(), request = *request] { return actions->runProgram(request); },
        [shown = std::move(shown), admin = request->asAdministrator](const Platform::ProcessActionResult& result)
        { return runTaskResultMessage(shown, admin, result); },
        std::nullopt); // its own pkexec or UAC prompt when elevated (#3427): nothing the helper does
}

void ProcessesPanel::renderFindHandlesDialog(const std::vector<Domain::ProcessSnapshot>& snapshots)
{
    const std::optional<FindHandlesAction> action = m_FindHandlesDialog.render(
        [&snapshots]
        {
            std::vector<Domain::HandleSearch::SearchProcess> processes;
            processes.reserve(snapshots.size());
            for (const Domain::ProcessSnapshot& snapshot : snapshots)
            {
                processes.push_back({.target = ProcessSelection::identityOf(snapshot), .name = snapshot.name});
            }
            return processes;
        });
    if (!action.has_value())
    {
        return;
    }
    m_Selection.selectOnly(action->target);
    selectProcess(action->target);
    if (action->showDetails)
    {
        Core::ShowProcessDetailsEvent event;
        raiseEvent(event);
    }
}

void ProcessesPanel::renderAffinityDialog()
{
    const std::optional<Platform::CpuAffinity> cpus = m_AffinityDialog.render();
    if (!cpus.has_value())
    {
        return;
    }
    Detail::DialogApply apply = affinityApply(m_ProcessActions.get(), m_AffinityDialog, *cpus);
    runRowAction(std::move(apply.pending), std::move(apply.work), std::move(apply.format), std::move(apply.retry));
}

void ProcessesPanel::renderSchedPolicyDialog()
{
    const std::optional<SchedPolicySetting> setting = m_SchedPolicyDialog.render();
    if (!setting.has_value())
    {
        return;
    }
    Detail::DialogApply apply = schedPolicyApply(m_ProcessActions.get(), m_SchedPolicyDialog, *setting);
    runRowAction(std::move(apply.pending), std::move(apply.work), std::move(apply.format), std::move(apply.retry));
}

} // namespace App
