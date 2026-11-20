// The Processes table's row actions (#3815): the row menu, its single-process actions, their worker and result line,
// and the confirmation. Split from ProcessesPanel.cpp.

#include "App/PanelPlatforms.h"
#include "App/Panels/PriorityChangesInFlight.h"
#include "App/Panels/ProcessActionConfirm.h"
#include "App/Panels/ProcessActionsView.h"
#include "App/Panels/ProcessAffinityDialog.h"
#include "App/Panels/ProcessBatchAction.h"
#include "App/Panels/ProcessBatchRun.h"
#include "App/Panels/ProcessDetailsPanel_ActionHelpers.h"
#include "App/Panels/ProcessDumpView.h"
#include "App/Panels/ProcessLookup.h"
#include "App/Panels/ProcessPins.h"
#include "App/Panels/ProcessSchedPolicyDialog.h"
#include "App/Panels/ProcessSelection.h"
#include "App/Panels/ProcessWaitChainDialog.h"
#include "App/Panels/ProcessesPanel.h"
#include "App/Panels/ProcessesPanelActions.h"
#include "App/Panels/RefusalOffer.h"
#include "App/Panels/VirusTotalLookup.h"
#include "App/PlatformOpen.h"
#include "App/UserConfig.h"
#include "Core/ApplicationEvents.h"
#include "Domain/PriorityConfig.h"
#include "Domain/ProcessSnapshot.h"
#include "Platform/IProcessActions.h"
#include "UI/Clipboard.h"
#include "UI/IconsFontAwesome6.h"
#include "UI/Theme.h"

// clang-format off
#include <imgui.h>
#include <imgui_internal.h> // ImGuiTable: the laid-out column widths, and the table background channel, have no public accessor
#include <misc/cpp/imgui_stdlib.h>
#include <spdlog/spdlog.h>
// clang-format on

#include <cstddef>
#include <format>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace App
{
void ProcessesPanel::renderRowContextMenu(const Domain::ProcessSnapshot& proc)
{
    // Opened on a row of a multi-selection (#804): its actions are for every selected process; Details
    // and Copy stay with the row it was opened on.
    const std::size_t batchCount =
        (m_Selection.size() > 1 && m_Selection.contains(ProcessSelection::identityOf(proc))) ? m_Selection.size() : 0;

    // Only while the menu is open, so formatting here costs nothing on an ordinary frame.
    if (batchCount > 0)
    {
        ImGui::TextDisabled("%zu processes selected", batchCount);
    }
    ImGui::TextDisabled("%s (PID %d)", proc.name.c_str(), proc.pid);
    ImGui::Separator();

    if (ImGui::MenuItem(ICON_FA_CIRCLE_INFO " Details"))
    {
        selectProcess(proc);
        Core::ShowProcessDetailsEvent event;
        raiseEvent(event);
    }
    // Kept at the top through every sort and filter (#3061): every process of this name.
    const bool pinned = m_Pins.contains(proc.name);
    if (ImGui::MenuItem(pinned ? ICON_FA_THUMBTACK " Unpin###Pin" : ICON_FA_THUMBTACK " Pin to top###Pin",
                        nullptr,
                        false,
                        pinned || proc.name.size() <= ProcessPins::MAX_NAME_BYTES))
    {
        togglePin(proc);
    }
    ImGui::SetItemTooltip(pinned ? "Stop keeping processes named \"%s\" at the top"
                                 : "Keep processes named \"%s\" at the top of the table, through any sort or filter",
                          proc.name.c_str());

    ImGui::Separator();
    if (ImGui::MenuItem(ICON_FA_COPY " Copy PID"))
    {
        UI::setClipboardUtf8(std::to_string(proc.pid));
    }
    if (ImGui::MenuItem(ICON_FA_COPY " Copy Name"))
    {
        UI::setClipboardUtf8(proc.name);
    }
    if (ImGui::MenuItem(ICON_FA_COPY " Copy Command Line", nullptr, false, !proc.command.empty()))
    {
        UI::setClipboardUtf8(proc.command);
    }

    // No other process action while one runs on its worker (#2142, #2305): its result is still to come.
    const bool actionsIdle = !rowActionRunning();

    // The row's executable in the file manager, and its name in a web search (#3064), as Task Manager's
    // menu has them: for the row the menu was opened on, each on the action worker.
    ImGui::Separator();
    if (m_ActionCapabilities.canOpenFileLocation &&
        ImGui::MenuItem(ICON_FA_FOLDER_OPEN " Open file location###OpenFileLocation", nullptr, false, actionsIdle))
    {
        openRowFileLocation(proc);
    }
    if (ImGui::MenuItem(ICON_FA_MAGNIFYING_GLASS " Search online###SearchOnline", nullptr, false, actionsIdle))
    {
        searchRowOnline(proc);
    }
    ImGui::SetItemTooltip("Searches the web for \"%s process\". Only the name is sent, never its path or command line.", proc.name.c_str());
    // Look up on VirusTotal (#3094): only with a key in Settings and a platform that can hash the image.
    if (VirusTotalLookup::offered(m_ActionCapabilities, m_LookupContext.get(m_LookupClient, UserConfig::get().settings().virusTotalApiKey)))
    {
        if (ImGui::MenuItem(ICON_FA_SHIELD_HALVED " Look up on VirusTotal###LookUpVirusTotal", nullptr, false, actionsIdle))
        {
            lookUpRowOnVirusTotal(proc);
        }
        ImGui::SetItemTooltip("%s", VirusTotalLookup::TOOLTIP);
    }
    // "Kill..." for one process, "Kill 5 processes..." for a selection; the "###" part keeps the item's
    // ID the same whichever it says.
    const auto actionItem = [batchCount, actionsIdle](Detail::ProcessAction action, const char* icon, const char* shortcut)
    {
        const std::string label =
            (batchCount > 0)
                ? std::format("{} {} {} processes...###{}", icon, Detail::actionLabel(action), batchCount, Detail::actionLabel(action))
                : std::format("{} {}...###{}", icon, Detail::actionLabel(action), Detail::actionLabel(action));
        return ImGui::MenuItem(label.c_str(), shortcut, false, actionsIdle);
    };
    const auto request = [this, &proc, batchCount](Detail::ProcessAction action)
    {
        if (batchCount > 0)
        {
            requestSelectionAction(action);
        }
        else
        {
            requestRowAction(action, proc);
        }
    };

    // Only what this platform can do (ProcessActionCapabilities), as in the Actions block.
    const Platform::ProcessActionCapabilities& can = m_ActionCapabilities;
    if (can.canStop || can.canContinue)
    {
        ImGui::Separator();
        if (can.canStop && actionItem(Detail::ProcessAction::Stop, ICON_FA_PAUSE, nullptr))
        {
            request(Detail::ProcessAction::Stop);
        }
        if (can.canContinue && actionItem(Detail::ProcessAction::Resume, ICON_FA_PLAY, nullptr))
        {
            request(Detail::ProcessAction::Resume);
        }
    }
    // The other signals (#3063, Linux): one process, each confirmed with what it usually does.
    if (can.canSendSignals && batchCount == 0 && ImGui::BeginMenu(ICON_FA_BOLT " Send signal###SendSignal", actionsIdle))
    {
        for (const auto [action, hint] : {
                 std::pair{Detail::ProcessAction::SignalHangup, "reload a daemon"},
                 std::pair{Detail::ProcessAction::SignalInterrupt, "as Ctrl+C"},
                 std::pair{Detail::ProcessAction::SignalQuit, "exit with a core dump"},
                 std::pair{Detail::ProcessAction::SignalUser1, "program-defined"},
                 std::pair{Detail::ProcessAction::SignalUser2, "program-defined"},
             })
        {
            const std::string label = std::format("{}...###{}", Detail::signalName(action), Detail::signalName(action));
            if (ImGui::MenuItem(label.c_str(), hint))
            {
                requestRowAction(action, proc);
            }
        }
        ImGui::EndMenu();
    }
    // Efficiency mode (#3071), as Task Manager's row menu has it: one process, ticked while it is on.
    // Turning it on is confirmed; turning it off isn't.
    const bool on = Detail::isInEfficiencyMode(proc);
    // Efficiency mode sets the priority: greyed out, too, while Process Details changes this process's (#3467).
    const Platform::ProcessTarget rowTarget{.pid = proc.pid, .startTimeTicks = proc.startTimeTicks};
    const char* const priorityHeld =
        priorityHeldElsewhere(rowTarget) ? PriorityChangesInFlight::waitReason(PriorityChangesInFlight::Panel::Processes) : nullptr;
    const Detail::EfficiencyModeToggle toggle =
        Detail::holdEfficiencyModeToggle(Detail::efficiencyModeToggle(can, rowTarget, on, !actionsIdle, batchCount), priorityHeld);
    if (toggle.state != Detail::EfficiencyModeToggleState::Hidden)
    {
        ImGui::Separator();
        if (ImGui::MenuItem(Detail::EFFICIENCY_MODE_LABEL, nullptr, on, toggle.state == Detail::EfficiencyModeToggleState::Enabled))
        {
            toggleRowEfficiencyMode(proc, on);
        }
        ImGui::SetItemTooltip("%s", toggle.tooltip); // a disabled item still says why
    }
    // The Realtime priority class (#3429, Windows): one process, always confirmed with a warning.
    // Greyed out, too, while Process Details changes this process's priority (#3467).
    if (const Detail::RealtimePriorityItem realtime = Detail::holdRealtimePriorityItem(
            Detail::realtimePriorityItem(
                can, rowTarget, proc.priorityClass == Domain::Priority::PriorityClass::Realtime, !actionsIdle, batchCount),
            priorityHeld);
        realtime.state != Detail::RealtimePriorityState::Hidden)
    {
        if (toggle.state == Detail::EfficiencyModeToggleState::Hidden) // else it shares Efficiency mode's group
        {
            ImGui::Separator();
        }
        if (ImGui::MenuItem(Detail::SET_REALTIME_PRIORITY_LABEL, nullptr, false, realtime.state == Detail::RealtimePriorityState::Enabled))
        {
            requestRowAction(Detail::ProcessAction::SetRealtimePriority, proc);
        }
        ImGui::SetItemTooltip("%s", realtime.tooltip);
    }
    // One priority for the whole selection (#1484): picked in its own dialog, then confirmed as a batch.
    // A single process's priority stays in Process Details' Actions block.
    if (ProcessBatch::offersBatchPriority(can, batchCount))
    {
        ImGui::Separator();
        const std::string label = std::format("{} Set priority for {} processes...###SetPriority", ICON_FA_GAUGE_HIGH, batchCount);
        if (ImGui::MenuItem(label.c_str(), nullptr, false, actionsIdle))
        {
            requestSelectionPriority();
        }
    }
    // And one I/O priority for the whole selection (#3551), in a dialog of its own; a single process's
    // stays in Process Details, as its priority does.
    if (ProcessBatch::offersBatchIoPriority(can, batchCount))
    {
        const std::string label = std::format("{} Set I/O priority for {} processes...###SetIoPriority", ICON_FA_HARD_DRIVE, batchCount);
        if (ImGui::MenuItem(label.c_str(), nullptr, false, actionsIdle))
        {
            requestSelectionIoPriority();
        }
    }
    // One process's CPUs (#1887), in its own dialog: a selection's processes rarely want the same set.
    if (can.canSetAffinity && batchCount == 0)
    {
        ImGui::Separator();
        if (ImGui::MenuItem(ICON_FA_MICROCHIP " Set affinity...###SetAffinity", nullptr, false, actionsIdle))
        {
            m_AffinityDialog.open({.pid = proc.pid, .startTimeTicks = proc.startTimeTicks},
                                  proc.name,
                                  proc.cpuAffinity,
                                  static_cast<std::size_t>(std::thread::hardware_concurrency()),
                                  // The groups and core kinds to arrange the boxes by (#1881): read once, on the
                                  // sampler thread with the actions, never here (#3182)
                                  m_AffinityLayout);
        }
    }
    // One process's scheduling policy (#3086, Linux): Normal, Batch, Idle, or real-time FIFO or RR (#3219), in its own
    // dialog, beside Set affinity.
    if (can.canSetSchedPolicy && batchCount == 0)
    {
        if (!can.canSetAffinity) // else it shares Set affinity...'s group
        {
            ImGui::Separator();
        }
        if (ImGui::MenuItem(ICON_FA_CLOCK " Set scheduling policy...###SetSchedPolicy", nullptr, false, actionsIdle))
        {
            m_SchedPolicyDialog.open({.pid = proc.pid, .startTimeTicks = proc.startTimeTicks},
                                     proc.name,
                                     proc.schedPolicy,
                                     proc.rtPriority,
                                     {.ownPid = m_OwnPid, .realtimeNeedsNoPassword = can.canSetRealtimeSchedPolicy});
        }
        ImGui::SetItemTooltip("Normal, Batch, Idle, or real-time FIFO or RR: how the kernel shares the CPU with this process");
    }
    // One process's dump file (#807), as Task Manager's row menu offers it; never for a selection.
    if (DumpDetail::offersCreateDump(can, batchCount))
    {
        if ((!can.canSetAffinity && !can.canSetSchedPolicy) || batchCount != 0) // else it shares Set affinity...'s group
        {
            ImGui::Separator();
        }
        if (ImGui::MenuItem(DumpDetail::CREATE_DUMP_LABEL, nullptr, false, actionsIdle))
        {
            // The remembered folder is checked on the confirm's worker, never here (#3155).
            m_DumpView.requestInRememberedFolder({.pid = proc.pid, .startTimeTicks = proc.startTimeTicks},
                                                 proc.name,
                                                 DumpDetail::rememberedDumpFolder(),
                                                 m_ProcessActions.get());
        }
        ImGui::SetItemTooltip("%s", DumpDetail::CREATE_DUMP_TOOLTIP);
    }
    // What its threads wait on (#3084), as Task Manager's row menu offers it; one process. It only reads, so
    // it waits on no row action.
    if (WaitChainDetail::offersAnalyzeWaitChain(can, batchCount))
    {
        if (!can.canSetAffinity && !can.canSetSchedPolicy && !DumpDetail::offersCreateDump(can, batchCount)) // else it shares their group
        {
            ImGui::Separator();
        }
        if (ImGui::MenuItem(WaitChainDetail::ANALYZE_WAIT_CHAIN_LABEL))
        {
            m_WaitChainDialog.open(m_ProcessActions.get(), {.pid = proc.pid, .startTimeTicks = proc.startTimeTicks}, proc.name, m_OwnPid);
        }
        ImGui::SetItemTooltip("%s", WaitChainDetail::ANALYZE_WAIT_CHAIN_TOOLTIP);
    }
    if (can.canTerminate || can.canKill)
    {
        // Ending a process can lose its work: in the danger colour, and confirmed in the dialog's
        // danger-coloured button, as in the Actions block (#1273).
        ImGui::Separator();
        ImGui::PushStyleColor(ImGuiCol_Text, UI::Theme::get().scheme().textError);
        if (can.canTerminate && actionItem(Detail::ProcessAction::Terminate, ICON_FA_XMARK, nullptr))
        {
            request(Detail::ProcessAction::Terminate);
        }
        if (can.canKill && actionItem(Detail::ProcessAction::Kill, ICON_FA_SKULL, "F9"))
        {
            request(Detail::ProcessAction::Kill);
        }
        // The process and everything it started (#3059), as Task Manager's End process tree: one process,
        // with children listed. Terminate where the platform has it, else Kill.
        if (batchCount == 0 && !proc.childrenIndices.empty() &&
            ImGui::MenuItem(ICON_FA_SITEMAP " End process tree###EndProcessTree", nullptr, false, actionsIdle))
        {
            requestTreeAction(can.canTerminate ? Detail::ProcessAction::Terminate : Detail::ProcessAction::Kill, proc);
        }
        ImGui::PopStyleColor();
    }
}

void ProcessesPanel::togglePin(const Domain::ProcessSnapshot& proc)
{
    const bool changed = m_Pins.contains(proc.name) ? m_Pins.unpin(proc.name) : m_Pins.pin(proc.name);
    if (!changed)
    {
        showRowActionResult({.ok = false, .text = std::format("Can't pin more than {} processes", ProcessPins::MAX_PINS)}, false);
        return;
    }
    UserConfig::get().settings().pinnedProcesses = m_Pins.names(); // saved with the other settings
    m_PinsChanged = true;
    spdlog::info("ProcessesPanel: {} {}", m_Pins.contains(proc.name) ? "pinned" : "unpinned", proc.name);
}

void ProcessesPanel::openRowFileLocation(const Domain::ProcessSnapshot& proc)
{
    // This row's identity: the platform refuses it if the PID has since been reused (#973).
    const Platform::ProcessTarget target{.pid = proc.pid, .startTimeTicks = proc.startTimeTicks};
    runRowAction(
        std::format("Opening the file location of {}...", proc.name),
        [actions = m_ProcessActions.get(), target] { return actions->openFileLocation(target); },
        [name = proc.name, pid = proc.pid](const Platform::ProcessActionResult& result)
        { return Detail::formatOpenFileLocationResult(name, pid, result); },
        std::nullopt); // the user's own file manager: nothing the helper does
}

void ProcessesPanel::searchRowOnline(const Domain::ProcessSnapshot& proc)
{
    runRowAction(
        std::format("Searching online for {}...", proc.name),
        [url = Detail::searchOnlineUrl(proc.name)]
        {
            switch (PlatformOpen::open(std::string_view{url}))
            {
            case PlatformOpen::OpenResult::Opened:
                return Platform::ProcessActionResult::ok();
            case PlatformOpen::OpenResult::Withheld: // an elevated TaskSmack never starts the browser elevated (#3658)
                return Platform::ProcessActionResult::error(PlatformOpen::withheldNotice(url));
            case PlatformOpen::OpenResult::Failed:
                break;
            }
            return Platform::ProcessActionResult::error("no web browser could be started");
        },
        [name = proc.name](const Platform::ProcessActionResult& result) { return Detail::formatSearchOnlineResult(name, result); },
        std::nullopt);
}

void ProcessesPanel::lookUpRowOnVirusTotal(const Domain::ProcessSnapshot& proc)
{
    // This row's identity: a reused PID's image is never hashed (#973).
    const Platform::ProcessTarget target{.pid = proc.pid, .startTimeTicks = proc.startTimeTicks};
    VirusTotalLookup::Context lookup = m_LookupContext.get(m_LookupClient, UserConfig::get().settings().virusTotalApiKey);
    runRowAction(
        std::format("Looking up {} on VirusTotal...", proc.name),
        [actions = m_ProcessActions.get(), lookup = std::move(lookup), target, name = proc.name](const std::stop_token& stop)
        { return VirusTotalLookup::lookUp(*actions, lookup, target, name, stop); },
        [](const Platform::ProcessActionResult& result) { return VirusTotalLookup::format(result); },
        std::nullopt);
}

void ProcessesPanel::requestRowAction(Detail::ProcessAction action, const Domain::ProcessSnapshot& proc)
{
    m_RowAction.action = action;
    // This row's identity: the platform refuses the action if the PID has since been reused (#973).
    m_RowAction.targets.assign(
        1,
        ProcessBatch::BatchTarget{.target = {.pid = proc.pid, .startTimeTicks = proc.startTimeTicks}, .name = proc.name, .priority = {}});
    m_RowAction.title = Detail::confirmTitle(action, proc.name, proc.pid);
    m_RowAction.question = Detail::confirmBody(action, proc.name, proc.pid);
    m_ShowRowActionConfirm = true;
}

void ProcessesPanel::toggleRowEfficiencyMode(const Domain::ProcessSnapshot& proc, bool on)
{
    if (!on)
    {
        requestRowAction(Detail::ProcessAction::EfficiencyModeOn, proc); // confirmed, as Task Manager does
        return;
    }
    // Off at once, for this row's identity: the platform refuses it if the PID has since been reused (#973).
    const Platform::ProcessTarget target{.pid = proc.pid, .startTimeTicks = proc.startTimeTicks};
    constexpr Detail::ProcessAction OFF = Detail::ProcessAction::EfficiencyModeOff;
    runRowAction(
        std::format("{} PID {}...", Detail::actionProgressLabel(OFF), target.pid),
        [actions = m_ProcessActions.get(), target] { return Detail::dispatchProcessAction(*actions, OFF, target); },
        [pid = target.pid](const Platform::ProcessActionResult& result) { return Detail::formatActionResultMessage(OFF, pid, result); },
        Detail::authorizedActionFor(OFF, target));
}

void ProcessesPanel::retryRowActionAuthorized()
{
    if (!offersAuthenticateAndRetry())
    {
        return;
    }
    std::optional<RowRetry> taken = m_RowOffer.takeRetry();
    if (!taken.has_value())
    {
        return;
    }
    RowRetry retry = std::move(*taken);
    const std::optional<Platform::ProcessTarget> setsPriorityOf =
        Detail::changesPriority(retry.action.kind) ? std::optional<Platform::ProcessTarget>{retry.action.target} : std::nullopt;
    runRowAction(
        std::format("Waiting for authorization to act on PID {}...", retry.action.target.pid),
        [actions = m_ProcessActions.get(), action = retry.action](const std::stop_token& stopToken)
        { return actions->runAuthorized(action, stopToken); },
        std::move(retry.format),
        std::nullopt, // a refusal to the administrator isn't offered again
        setsPriorityOf);
}

void ProcessesPanel::showRowActionResult(Detail::ActionResultMessage message, bool succeeded)
{
    m_RowWithheldPath.clear();
    m_RowActionResult = std::move(message);
    m_RowOffer.showLine(ProcessesPanelActions::ROW_ACTION_RESULT_SECONDS); // the last row action's offer goes with its line
    if (succeeded)
    {
        requestRefresh(); // Show the process changed or gone without waiting for the next sample
    }
}

void ProcessesPanel::renderRowActionConfirm()
{
    const ProcessActionConfirm::Outcome outcome =
        ProcessActionConfirm::renderText(m_ShowRowActionConfirm, m_RowAction.action, m_RowAction.title, m_RowAction.question);
    if (outcome == ProcessActionConfirm::Outcome::Cancelled)
    {
        m_RowAction = {}; // Nothing is pending any more: F9 may ask again (#170)
        return;
    }
    if (outcome != ProcessActionConfirm::Outcome::Confirmed || m_RowAction.targets.empty())
    {
        return;
    }
    if (m_RowAction.targets.size() > 1 && m_ProcessActions)
    {
        // Every selected process in turn, each checked by the platform as a single action is, then one
        // summary line rather than a message per process (#804). On the batch's worker (#2142): the
        // line comes on a later frame, and the toolbar shows the progress until then.
        std::optional<ProcessBatch::BatchRunOutcome> failed =
            m_BatchRun.startAction(*m_ProcessActions, m_RowAction.action, std::move(m_RowAction.targets), m_OwnPid);
        m_RowAction = {}; // Handed to the worker; nothing is pending any more
        if (failed.has_value())
        {
            showBatchOutcome(std::move(*failed));
        }
        return;
    }
    if (m_RowAction.targets.size() != 1)
    {
        showRowActionResult({.ok = false, .text = "Process actions unavailable"}, false);
        m_RowAction = {};
        return;
    }
    // One process, on the row action worker (#2305): "Killing PID 123..." in the toolbar until its
    // result comes on a later frame.
    const Detail::ProcessAction action = m_RowAction.action;
    const Platform::ProcessTarget target = m_RowAction.targets.front().target;
    m_RowAction = {}; // Acted on once; nothing is pending any more
    runRowAction(
        std::format("{} PID {}...", Detail::actionProgressLabel(action), target.pid),
        [actions = m_ProcessActions.get(), action, target] { return Detail::dispatchProcessAction(*actions, action, target); },
        [action, pid = target.pid](const Platform::ProcessActionResult& result)
        { return Detail::formatActionResultMessage(action, pid, result); },
        Detail::authorizedActionFor(action, target));
}

} // namespace App
