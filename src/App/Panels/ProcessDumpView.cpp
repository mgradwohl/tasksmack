#include "ProcessDumpView.h"

#include "App/PlatformOpen.h"
#include "Core/Utf8Path.h"
#include "Platform/IProcessActions.h"
#include "Platform/ThreadName.h"
#include "ProcessActionConfirm.h"
#include "ProcessActionsView.h"
#include "UI/ChromeWidgets.h"
#include "UI/DialogMetrics.h"
#include "UI/Format.h"
#include "UI/IconsFontAwesome6.h"
#include "UI/Theme.h"
#include "UI/Widgets.h"

#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <expected>
#include <filesystem>
#include <format>
#include <future>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace App
{

namespace DumpDetail
{

std::filesystem::path defaultDumpDirectory()
{
    std::error_code error;
    const std::filesystem::path directory = std::filesystem::temp_directory_path(error);
    return error ? std::filesystem::path{} : directory;
}

std::string confirmTitle(std::string_view processName, std::int32_t pid)
{
    return std::format("Create a dump file of {} (PID {})?", processName, pid);
}

std::string confirmQuestion(std::string_view processName, std::int32_t pid, const std::filesystem::path& directory)
{
    return std::format("A full memory dump of {} (PID {}) will be written to a new file in {}.\n\nIt can be as large as all the memory "
                       "the process uses, and the process may stop responding while it is written.",
                       processName,
                       pid,
                       directory.empty() ? std::string("the temporary folder") : Core::pathToUtf8(directory));
}

std::string progressText(std::string_view processName, std::int32_t pid, std::chrono::seconds elapsed)
{
    return std::format("Writing a dump file of {} (PID {})... {} s", processName, pid, elapsed.count());
}

DumpMessage resultMessage(std::string_view processName, std::int32_t pid, const Platform::ProcessDumpResult& result)
{
    if (!result.has_value())
    {
        return {.ok = false,
                .title = "Couldn't create a dump file",
                .text = std::format("Could not create a dump file of {} (PID {}): {}", processName, pid, result.error()),
                .file = {},
                .note = {}};
    }
    return {.ok = true,
            .title = "Dump file created",
            .text = std::format(
                "Wrote a dump file of {} (PID {}), {}:", processName, pid, UI::Format::formatBytes(static_cast<double>(result->sizeBytes))),
            .file = result->file,
            .note = result->note};
}

namespace
{

/// Runs on the worker: the dump, or the failure for one that threw. The exception's message is copied
/// here, on the thread that threw it, so the UI thread gets a plain result through the future (#1685).
[[nodiscard]] Platform::ProcessDumpResult dumpOrFailed(Platform::IProcessActions& actions,
                                                       const Platform::ProcessTarget& target,
                                                       const std::filesystem::path& directory,
                                                       const std::stop_token& stopToken)
{
    try
    {
        return actions.createDump(target, directory, stopToken);
    }
    catch (const std::exception& e)
    {
        return std::unexpected(std::string(e.what()));
    }
    catch (...)
    {
        return std::unexpected(std::string("Unknown error"));
    }
}

} // namespace

} // namespace DumpDetail

ProcessDumpView::~ProcessDumpView()
{
    shutdown();
}

ProcessDumpView& ProcessDumpView::operator=(ProcessDumpView&& other) noexcept
{
    if (this != &other)
    {
        shutdown();
        m_ShowConfirm = other.m_ShowConfirm;
        m_DismissConfirm = other.m_DismissConfirm;
        m_ConfirmPending = other.m_ConfirmPending;
        m_Target = other.m_Target;
        m_ProcessName = std::move(other.m_ProcessName);
        m_Directory = std::move(other.m_Directory);
        m_ConfirmTitle = std::move(other.m_ConfirmTitle);
        m_ConfirmQuestion = std::move(other.m_ConfirmQuestion);
        m_Worker = std::move(other.m_Worker);
        m_Stop = std::move(other.m_Stop);
        m_StartedAt = other.m_StartedAt;
        m_CancelRequested = other.m_CancelRequested;
        m_Result = std::move(other.m_Result);
    }
    return *this;
}

void ProcessDumpView::request(const Platform::ProcessTarget& target, std::string processName, std::filesystem::path directory)
{
    if (active() || target.pid <= 0)
    {
        return;
    }
    // Built once here, not every frame the dialog is up.
    m_ConfirmTitle = DumpDetail::confirmTitle(processName, target.pid);
    m_ConfirmQuestion = DumpDetail::confirmQuestion(processName, target.pid, directory);
    m_Target = target;
    m_ProcessName = std::move(processName);
    m_Directory = std::move(directory);
    m_ConfirmPending = true;
    m_ShowConfirm = true;
    m_DismissConfirm = false;
}

void ProcessDumpView::confirm(Platform::IProcessActions* actions)
{
    if (!m_ConfirmPending)
    {
        return;
    }
    m_ConfirmPending = false;
    m_ShowConfirm = false;
    if (actions == nullptr)
    {
        m_Result = DumpDetail::resultMessage(
            m_ProcessName, m_Target.pid, Platform::ProcessDumpResult(std::unexpected(std::string("Process actions unavailable"))));
        return;
    }
    m_Stop = std::stop_source{}; // a cancel() of an earlier dump does not carry over
    m_CancelRequested = false;
    m_StartedAt = std::chrono::steady_clock::now();
    try
    {
        // The worker owns copies of the target and folder; it shares only the future and the stop
        // token's state with this view, and @p actions, which outlives it (see the header).
        m_Worker = std::async(std::launch::async,
                              [actions, target = m_Target, directory = m_Directory, stopToken = m_Stop.get_token()]
                              {
                                  static_cast<void>(Platform::setCurrentThreadName(Platform::DUMP_THREAD_NAME));
                                  return DumpDetail::dumpOrFailed(*actions, target, directory, stopToken);
                              });
    }
    catch (const std::system_error& e)
    {
        m_Result =
            DumpDetail::resultMessage(m_ProcessName, m_Target.pid, Platform::ProcessDumpResult(std::unexpected(std::string(e.what()))));
    }
}

void ProcessDumpView::cancelConfirm() noexcept
{
    m_ConfirmPending = false;
    m_ShowConfirm = false;
}

void ProcessDumpView::cancel() noexcept
{
    if (busy())
    {
        m_CancelRequested = true;
        static_cast<void>(m_Stop.request_stop());
    }
}

bool ProcessDumpView::takeFinished(bool wait)
{
    if (!m_Worker.valid() || (!wait && m_Worker.wait_for(std::chrono::seconds(0)) != std::future_status::ready))
    {
        return false;
    }
    Platform::ProcessDumpResult result;
    try
    {
        result = m_Worker.get();
    }
    catch (...)
    {
        // The worker turns a throwing dump into a failure itself (dumpOrFailed()), so only building that
        // result can land here (bad_alloc). The exception object was thrown on the worker and is not read
        // here (#1685).
        result = std::unexpected(std::string("Unknown error"));
    }
    // A dump the user cancelled closes the dialog without a result to acknowledge; one that finished
    // before the cancel reached it still says where its file is.
    if (m_CancelRequested && !result.has_value())
    {
        m_Result.reset();
    }
    else
    {
        m_Result = DumpDetail::resultMessage(m_ProcessName, m_Target.pid, result);
    }
    m_CancelRequested = false;
    return true;
}

void ProcessDumpView::shutdown() noexcept
{
    cancel();
    try
    {
        if (m_Worker.valid())
        {
            m_Worker.wait();
        }
    }
    catch (...) // NOLINT(bugprone-empty-catch): wait() on a valid future does not throw
    {}
}

void ProcessDumpView::onSelectionChanged() noexcept
{
    m_DismissConfirm = m_DismissConfirm || confirmPending();
    cancelConfirm();
}

void ProcessDumpView::render(Platform::IProcessActions* actions, const Platform::ProcessTarget* liveTarget)
{
    // The confirm: closed unconfirmed after a selection change, or once the selection is another
    // process, so it never starts a dump of a process other than the one it names.
    bool dismiss = std::exchange(m_DismissConfirm, false);
    if (confirmPending() && liveTarget != nullptr && !Detail::isSameProcessTarget(m_Target, *liveTarget))
    {
        cancelConfirm();
        dismiss = true;
    }
    if (confirmPending() || dismiss)
    {
        // The shared confirm dialog under an ID of its own: the panel's own confirm (Kill, Suspend...)
        // is submitted every frame too, and must never see this one open as its own.
        ImGui::PushID("ProcessDump");
        const ProcessActionConfirm::Outcome outcome = ProcessActionConfirm::renderLabelled(
            m_ShowConfirm, DumpDetail::CONFIRM_LABEL, false, m_ConfirmTitle, m_ConfirmQuestion, dismiss);
        ImGui::PopID();
        switch (outcome)
        {
        case ProcessActionConfirm::Outcome::Confirmed:
            confirm(actions);
            break;
        case ProcessActionConfirm::Outcome::Cancelled:
            cancelConfirm();
            break;
        case ProcessActionConfirm::Outcome::None:
            break;
        }
    }

    static_cast<void>(takeFinished());
    renderProgressDialog();
}

namespace
{
// The progress-then-result dialog's ID; the "###" keeps it while the title changes with the outcome.
constexpr const char* DUMP_POPUP_ID = "###ProcessDump";
// The dialog's text wraps at this many ems, or at the main window's width when that is narrower.
constexpr float DUMP_TEXT_WIDTH_EM = 34.0F;
// Floor on the footer's buttons, in ems, as the confirm's.
constexpr float DUMP_BUTTON_MIN_EM = 11.25F;
} // namespace

void ProcessDumpView::renderProgressDialog()
{
    const bool wanted = busy() || m_Result.has_value();
    if (!wanted)
    {
        // A cancelled dump ends with nothing to show: the dialog still open from its progress is closed.
        // CloseCurrentPopup() acts on the popup being drawn, so it is entered to close it.
        if (ImGui::IsPopupOpen(DUMP_POPUP_ID) && ImGui::BeginPopupModal(DUMP_POPUP_ID, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        return;
    }
    if (!ImGui::IsPopupOpen(DUMP_POPUP_ID))
    {
        ImGui::OpenPopup(DUMP_POPUP_ID);
    }
    const std::string title = (m_Result.has_value() ? m_Result->title : std::string("Creating dump file")) + DUMP_POPUP_ID;

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    static ImVec2 lastWorkSize;
    UI::Widgets::centerNextDialog(lastWorkSize);
    UI::Widgets::setNextDialogSizeConstraints(ImVec2(UI::DialogMetrics::computeDialogMaxExtent(viewport->WorkSize.x),
                                                     UI::DialogMetrics::computeDialogMaxExtent(viewport->WorkSize.y)));
    if (!ImGui::BeginPopupModal(title.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        return;
    }
    UI::Widgets::keepCurrentWindowInViewport();
    // Escape is Cancel while it runs, and Close once it is done (#1944): never Open folder.
    const bool escaped = UI::Widgets::dialogEscapePressed();
    const float wrapWidth =
        std::min(ImGui::GetFontSize() * DUMP_TEXT_WIDTH_EM, UI::DialogMetrics::computeDialogMaxExtent(viewport->WorkSize.x) * 0.9F);
    const auto& scheme = UI::Theme::get().scheme();
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrapWidth);

    if (!m_Result.has_value())
    {
        const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - m_StartedAt);
        ImGui::TextUnformatted(DumpDetail::progressText(m_ProcessName, m_Target.pid, elapsed).c_str());
        ImGui::TextColored(scheme.textMuted, "%s", m_CancelRequested ? "Cancelling..." : Core::pathToUtf8(m_Directory).c_str());
        ImGui::PopTextWrapPos();
        const UI::Widgets::DialogFooterButton cancelButton{.label = "Cancel",
                                                           .fills = nullptr,
                                                           .tooltip = "Stop writing the dump and delete the partial file",
                                                           .disabled = m_CancelRequested};
        const float buttonWidth = UI::Widgets::footerButtonWidth({"Cancel"}, DUMP_BUTTON_MIN_EM);
        if (UI::Widgets::dialogFooter(cancelButton, {}, buttonWidth) == UI::Widgets::DialogFooterAction::Primary || escaped)
        {
            cancel();
        }
        ImGui::EndPopup();
        return;
    }

    const DumpDetail::DumpMessage& result = *m_Result;
    ImGui::TextColored(result.ok ? scheme.textSuccess : scheme.textError, "%s", result.text.c_str());
    if (result.ok)
    {
        ImGui::TextUnformatted(Core::pathToUtf8(result.file).c_str());
        if (!result.note.empty())
        {
            ImGui::TextColored(scheme.textMuted, "%s", result.note.c_str());
        }
    }
    ImGui::PopTextWrapPos();

    UI::Widgets::DialogFooterAction pressed = UI::Widgets::DialogFooterAction::None;
    if (result.ok)
    {
        const UI::Widgets::DialogFooterButton openButton{
            .label = ICON_FA_FOLDER_OPEN " Open folder", .fills = nullptr, .tooltip = "Show the dump file's folder", .disabled = false};
        const UI::Widgets::DialogFooterButton closeButton{.label = "Close", .fills = nullptr, .tooltip = nullptr, .disabled = false};
        pressed = UI::Widgets::dialogFooter(
            openButton, closeButton, UI::Widgets::footerButtonWidth({openButton.label, closeButton.label}, DUMP_BUTTON_MIN_EM));
    }
    else
    {
        const UI::Widgets::DialogFooterButton closeButton{.label = "Close", .fills = nullptr, .tooltip = nullptr, .disabled = false};
        pressed = UI::Widgets::dialogFooter(closeButton, {}, UI::Widgets::footerButtonWidth({closeButton.label}, DUMP_BUTTON_MIN_EM));
    }
    if (escaped)
    {
        pressed = result.ok ? UI::Widgets::DialogFooterAction::Secondary : UI::Widgets::DialogFooterAction::Primary;
    }
    if (pressed == UI::Widgets::DialogFooterAction::Primary && result.ok)
    {
        static_cast<void>(PlatformOpen::openWithSystemHandler(result.file.parent_path()));
    }
    if (pressed == UI::Widgets::DialogFooterAction::Primary || pressed == UI::Widgets::DialogFooterAction::Secondary)
    {
        dismissResult();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

} // namespace App
