#pragma once

// The row action worker's shared parts (#3815): ProcessesPanel::runRowAction(), a member template that
// ProcessesPanel.cpp, ProcessesPanelRowActions.cpp and ProcessesPanelBatch.cpp all call, and what its result line and
// the batch priority line share. Internal to the Processes panel's files.

#include "App/Panels/PriorityChangesInFlight.h"
#include "App/Panels/ProcessBatchAction.h"
#include "App/Panels/ProcessDetailsPanel_ActionHelpers.h"
#include "App/Panels/ProcessesPanel.h"
#include "App/Panels/RefusalOffer.h"
#include "Platform/IProcessActions.h"

#include <cstddef>
#include <format>
#include <optional>
#include <string>
#include <utility>

namespace App
{

namespace ProcessesPanelActions
{

// How long a row-menu action's result stays in the toolbar, like the Actions block's (#1209).
inline constexpr float ROW_ACTION_RESULT_SECONDS = 5.0F;

/// What a batch priority change's result line adds for the @p skipped processes it left out, each with a
/// priority change still running in Process Details (#3467): "; 1 process skipped: ...". Empty for none.
[[nodiscard]] inline std::string batchPrioritySkippedText(std::size_t skipped)
{
    if (skipped == 0)
    {
        return {};
    }
    return std::format("; {} skipped: {} priority change{} still running in Process Details",
                       ProcessBatch::processCountText(skipped),
                       skipped == 1 ? "its" : "their",
                       skipped == 1 ? " is" : "s are");
}

} // namespace ProcessesPanelActions

template<typename Work>
void ProcessesPanel::runRowAction(std::string pending,
                                  Work work,
                                  RowResultFormat format,
                                  std::optional<Platform::AuthorizedAction> retry,
                                  std::optional<Platform::ProcessTarget> setsPriorityOf)
{
    if (!m_ProcessActions)
    {
        showRowActionResult(format(Platform::ProcessActionResult::error("Process actions unavailable")), false);
        return;
    }
    if (rowActionRunning())
    {
        showRowActionResult(format(Platform::ProcessActionResult::error("Another action is still running")), false);
        return;
    }
    // F7/F8, Set Realtime priority, Efficiency mode and their retries set the priority, as Process Details' changes do: never
    // while one for the same process runs there, or the older of the two could land last (#3467).
    if (!setsPriorityOf.has_value() && retry.has_value() && Detail::changesPriority(retry->kind))
    {
        setsPriorityOf = retry->target;
    }
    if (setsPriorityOf.has_value() && priorityHeldElsewhere(*setsPriorityOf))
    {
        showRowActionResult(
            format(Platform::ProcessActionResult::error(PriorityChangesInFlight::refusal(PriorityChangesInFlight::Panel::Processes))),
            false);
        return;
    }
    m_RowRunPriorityTarget = setsPriorityOf;
    static_cast<void>(m_RowActionRun.start(
        std::move(pending),
        std::move(work),
        // NOLINTNEXTLINE(clang-analyzer-cplusplus.NewDeleteLeaks) - the std::function moved into the finish is owned by it, not leaked
        [this, format = std::move(format), retry = std::move(retry)](const Platform::ProcessActionResult& result)
        {
            showRowActionResult(format(result), result.success);
            if (!result.withheldPath.empty())
            {
                // Not shown, so no Explorer window runs elevated (#3669): Copy beside it, with time to use it.
                m_RowWithheldPath = result.withheldPath;
                m_RowOffer.holdLine(RefusalOffer<RowRetry>::HOLD_SECONDS);
            }
            // Authenticate and retry through the helper, or with none, Restart as administrator... (#3658).
            std::optional<RowRetry> redo;
            if (retry.has_value())
            {
                redo = RowRetry{.action = *retry, .format = format};
            }
            const bool helperCanRedo = m_ProcessActions && m_ProcessActions->actionCapabilities().canAuthorizeInteractively;
            static_cast<void>(m_RowOffer.offer(result.permissionDenied, std::move(redo), helperCanRedo));
        }));
}

} // namespace App
