#include "ProcessBatchIoPriorityDialog.h"

#include "Domain/PriorityConfig.h"
#include "Platform/IoPriorityTypes.h"
#include "ProcessBatchAction.h"
#include "ProcessBatchPriorityDialog.h"
#include "ProcessDetailsPanel_PriorityHelpers.h"
#include "ProcessIoPriorityView.h"
#include "ProcessPriorityView.h"
#include "UI/ChromeWidgets.h"
#include "UI/DialogMetrics.h"
#include "UI/IconsFontAwesome6.h"
#include "UI/Theme.h"
#include "UI/Widgets.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace App
{

namespace
{

// The "###" part keeps the popup's ID fixed while its title names the count.
constexpr const char* POPUP_ID = "###BatchIoPriority";

constexpr const char* CANCEL_LABEL = "Cancel";
constexpr const char* RESET_LABEL = "Reset to default";

// Floor on the footer's buttons, in ems, as for the batch priority dialog.
constexpr float BUTTON_MIN_EM = 11.25F;

// The dialog's width, in ems, unless the slider needs more for its stop names (#1984); room for the
// list's three columns too.
constexpr float DIALOG_WIDTH_EM = 30.0F;

/// The slider Process Details draws for the same platform and privilege (ProcessIoPriorityView::render()).
[[nodiscard]] const Detail::DiscretePrioritySlider& sliderFor(bool realtimeSettable, bool windowsHints) noexcept
{
    if (windowsHints)
    {
        return Detail::IO_HINT_SLIDER;
    }
    return realtimeSettable ? Detail::IO_PRIORITY_SLIDER : Detail::IO_PRIORITY_SLIDER_NO_REALTIME;
}

/// The dialog's width: room for the slider's stop names and the process list, never wider than the
/// viewport allows.
[[nodiscard]] float dialogWidth(const Detail::DiscretePrioritySlider& slider)
{
    const float contentWidth = std::max(DIALOG_WIDTH_EM * ImGui::GetFontSize(), Detail::discretePrioritySliderMinWidth(slider));
    // In whole pixels up: ImGui truncates a window's size, and a fraction short of the slider's minimum
    // width drops its stop names (#1984).
    const float wanted = std::ceil(contentWidth + (ImGui::GetStyle().WindowPadding.x * 2.0F));
    return std::min(wanted, UI::DialogMetrics::computeDialogMaxExtent(ImGui::GetMainViewport()->WorkSize.x));
}

} // namespace

Platform::IoPriority ProcessBatchIoPriorityDialog::defaultValue(bool windowsHints) noexcept
{
    if (windowsHints)
    {
        return {.ioClass = Platform::IoPriorityClass::WindowsHint, .level = static_cast<std::int32_t>(Platform::IoPriorityHint::Normal)};
    }
    return {.ioClass = Platform::IoPriorityClass::None, .level = 0};
}

void ProcessBatchIoPriorityDialog::open(std::vector<ProcessBatch::BatchTarget> targets,
                                        std::int32_t ownPid,
                                        bool realtimeSettable,
                                        bool windowsHints)
{
    m_Targets = std::move(targets);
    // Built once here, not every frame the dialog is up: the list points into m_Targets.
    m_Listed = ProcessBatch::listTargets(m_Targets, ownPid);
    m_Warnings.clear();
    ProcessBatch::appendNotableWarnings(m_Warnings, m_Listed.notable, "changed");
    // appendNotableWarnings() writes paragraphs for a confirmation's body; here they stand alone.
    const std::size_t firstText = m_Warnings.find_first_not_of('\n');
    m_Warnings.erase(0, firstText == std::string::npos ? m_Warnings.size() : firstText);
    m_ApplyLabel = std::format("Apply to {}", ProcessBatch::processCountText(m_Targets.size()));
    m_RealtimeSettable = realtimeSettable;
    m_WindowsHints = windowsHints;
    m_Priority = defaultValue(windowsHints);
    m_ShowRequested = true;
}

void ProcessBatchIoPriorityDialog::pick(const Platform::IoPriority& priority) noexcept
{
    m_Priority = Detail::normalizeIoPriority(priority);
}

std::optional<Platform::IoPriority> ProcessBatchIoPriorityDialog::render()
{
    if (m_ShowRequested)
    {
        ImGui::OpenPopup(POPUP_ID);
        m_ShowRequested = false;
    }
    m_Open = ImGui::IsPopupOpen(POPUP_ID);
    if (!m_Open)
    {
        return std::nullopt; // Nothing to draw; skip building the title every frame
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const Detail::DiscretePrioritySlider& slider = sliderFor(m_RealtimeSettable, m_WindowsHints);
    const std::string title =
        std::format("{}  Set I/O priority for {}{}", ICON_FA_HARD_DRIVE, ProcessBatch::processCountText(m_Targets.size()), POPUP_ID);
    // Centred over the main window, a fixed width so the slider is laid out at its own width from the
    // first frame, and a height fitted to the contents; both capped at the viewport, as the batch
    // priority dialog is.
    UI::Widgets::centerNextDialog(m_LastWorkSize);
    ImGui::SetNextWindowSize(ImVec2(dialogWidth(slider), 0.0F));
    UI::Widgets::setNextDialogSizeConstraints(ImVec2(UI::DialogMetrics::computeDialogMaxExtent(viewport->WorkSize.x),
                                                     UI::DialogMetrics::computeDialogMaxExtent(viewport->WorkSize.y)));
    std::optional<Platform::IoPriority> chosen;
    if (!ImGui::BeginPopupModal(title.c_str(), nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings))
    {
        return chosen;
    }
    UI::Widgets::keepCurrentWindowInViewport();
    // Escape cancels, as Cancel does: nothing is changed (#1944).
    const bool escaped = UI::Widgets::dialogEscapePressed();
    const auto& theme = UI::Theme::get();

    // What will happen: Process Details' own slider, so the batch is picked exactly as one process is.
    // The class never set shows hollow at the level the default nice value derives, as Process Details
    // shows it for a process at nice 0.
    const Detail::IoStop shown = m_WindowsHints ? Detail::IoStop{.index = Detail::ioHintStopFor(m_Priority), .inherited = false}
                                                : Detail::ioStopFor(m_Priority, Domain::Priority::NORMAL_NICE, m_RealtimeSettable);
    const Detail::DiscretePick picked = Detail::renderDiscretePrioritySlider(slider, shown.index, shown.inherited);
    // A click on the hollow stop chooses it too (#3566's review): it stands for the class never set at nice 0's level,
    // but each process inherits from its own nice value, so choosing it makes that level explicit for all of them.
    if (picked.index >= 0 && (picked.index != shown.index || (picked.selected && shown.inherited)))
    {
        pick(m_WindowsHints ? Detail::ioHintForStop(picked.index) : Detail::ioPriorityForStop(picked.index, m_RealtimeSettable));
    }
    // Back to the class never set, which follows each process's own nice value (Linux only: Windows'
    // default, Normal, is a stop like any other).
    if (!m_WindowsHints && m_Priority.ioClass != Platform::IoPriorityClass::None)
    {
        ImGui::Spacing();
        if (ImGui::Button(RESET_LABEL))
        {
            pick(defaultValue(false));
        }
        // The class never set follows the CPU policy as well as nice (inheritedIoClass(), #3566's review).
        ImGui::SetItemTooltip("Follow each process's CPU scheduling policy and nice value again: best-effort at the level nice "
                              "derives, or realtime under a real-time policy, idle under SCHED_IDLE");
    }
    ImGui::Spacing();
    ImGui::PushTextWrapPos(0.0F);
    ImGui::TextUnformatted(std::format("Applies: {}", ProcessBatch::ioPriorityValueText(m_Priority)).c_str());
    ImGui::PopTextWrapPos();

    // To which processes: listed here, so Apply is the confirmation (no second dialog, #1539).
    ImGui::Spacing();
    (void) UI::Widgets::sectionHeader(ICON_FA_LIST, "Processes");
    ProcessBatch::renderTargetList(m_Listed);
    if (!m_Warnings.empty())
    {
        ImGui::PushTextWrapPos(0.0F);
        ImGui::TextColored(theme.scheme().textWarning, "%s", m_Warnings.c_str());
        ImGui::PopTextWrapPos();
    }

    // Apply is the primary action, in the success fill, Cancel the plain one: as the batch priority dialog.
    const UI::Widgets::ButtonFills applyFills{
        .resting = theme.scheme().successButton,
        .hovered = theme.scheme().successButtonHovered,
        .pressed = theme.scheme().successButtonActive,
    };
    const UI::Widgets::DialogFooterButton applyButton{.label = m_ApplyLabel.c_str(), .fills = &applyFills, .tooltip = nullptr};
    const UI::Widgets::DialogFooterButton cancelButton{.label = CANCEL_LABEL, .fills = nullptr, .tooltip = nullptr};
    UI::Widgets::DialogFooterAction pressed = UI::Widgets::dialogFooter(
        applyButton, cancelButton, UI::Widgets::footerButtonWidth({m_ApplyLabel.c_str(), CANCEL_LABEL}, BUTTON_MIN_EM));
    if (escaped)
    {
        pressed = UI::Widgets::DialogFooterAction::Secondary;
    }
    switch (pressed)
    {
    case UI::Widgets::DialogFooterAction::Primary:
        chosen = m_Priority;
        m_Open = false;
        ImGui::CloseCurrentPopup();
        break;
    case UI::Widgets::DialogFooterAction::Secondary:
        m_Open = false;
        ImGui::CloseCurrentPopup();
        break;
    case UI::Widgets::DialogFooterAction::None:
    case UI::Widgets::DialogFooterAction::Leading:
        break;
    }
    ImGui::EndPopup();
    return chosen;
}

} // namespace App
