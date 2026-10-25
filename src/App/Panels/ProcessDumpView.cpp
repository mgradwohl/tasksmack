#include "ProcessDumpView.h"

#include "App/NativeWindow.h"
#include "App/PlatformOpen.h"
#include "App/UserConfig.h"
#include "Core/Application.h"
#include "Core/SaveFileDialog.h"
#include "Core/Utf8Path.h"
#include "Core/Window.h"
#include "Platform/IProcessActions.h"
#include "ProcessActionConfirm.h"
#include "ProcessActionsView.h"
#include "UI/ChromeWidgets.h"
#include "UI/DialogMetrics.h"
#include "UI/Format.h"
#include "UI/IconsFontAwesome6.h"
#include "UI/Theme.h"
#include "UI/Widgets.h"

#include <SDL3/SDL_video.h>
#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace App
{

namespace DumpDetail
{

std::string lowSpaceWarning(const Platform::DumpSpace& space, const std::filesystem::path& directory, bool canChooseFolder)
{
    if (!dumpMayNotFit(space))
    {
        return {};
    }
    return std::format("Only {} is free on the drive holding {}, and a full dump of this process may need about {} (its {} of "
                       "private memory, and more). It may not fit: {}",
                       UI::Format::formatBytes(static_cast<double>(space.freeBytes.value_or(0))),
                       directory.empty() ? std::string("the temporary folder") : Core::pathToUtf8(directory),
                       UI::Format::formatBytes(static_cast<double>(estimatedDumpBytes(space.processBytes.value_or(0)))),
                       UI::Format::formatBytes(static_cast<double>(space.processBytes.value_or(0))),
                       canChooseFolder ? "choose another folder with Save dump to..., or free some space." : "free some space first.");
}

std::filesystem::path defaultDumpDirectory()
{
    std::error_code error;
    const std::filesystem::path directory = std::filesystem::temp_directory_path(error);
    return error ? std::filesystem::path{} : directory;
}

std::filesystem::path dumpDirectory(std::string_view remembered)
{
    if (!remembered.empty())
    {
        std::filesystem::path directory = Core::utf8ToPath(remembered);
        std::error_code error;
        if (std::filesystem::is_directory(directory, error))
        {
            return directory;
        }
    }
    return defaultDumpDirectory();
}

std::filesystem::path rememberedDumpDirectory()
{
    return dumpDirectory(UserConfig::get().settings().dumpFolder);
}

void rememberDumpDirectory(const std::filesystem::path& directory)
{
    auto& config = UserConfig::get();
    std::string folder = Core::pathToUtf8(directory);
    if (config.settings().dumpFolder == folder)
    {
        return;
    }
    config.settings().dumpFolder = std::move(folder);
    config.saveInBackground(); // called on the UI thread, which never writes the file (#2916)
}

std::string confirmTitle(std::string_view processName, std::int32_t pid)
{
    return std::format("Create a dump file of {} (PID {})?", processName, pid);
}

std::string confirmQuestion(std::string_view processName,
                            std::int32_t pid,
                            const std::filesystem::path& directory,
                            bool dumpsFromSnapshot,
                            std::string_view warning)
{
    // Windows takes a snapshot first (#2001); Linux's gcore stops the process for the whole write (#2241).
    const std::string_view pause = dumpsFromSnapshot ? "The process pauses while a snapshot of it is taken; where none can be, it may "
                                                       "stop responding while the dump is written."
                                                     : "The process is stopped while the dump is written, and doesn't respond until it "
                                                       "is done.";
    std::string question = std::format("A full memory dump of {} (PID {}) will be written to a new file in {}.\n\nIt can be as large as "
                                       "all the memory the process uses. {}",
                                       processName,
                                       pid,
                                       directory.empty() ? std::string("the temporary folder") : Core::pathToUtf8(directory),
                                       pause);
    if (!warning.empty())
    {
        question += "\n\n";
        question += warning;
    }
    return question;
}

std::string progressText(std::string_view processName, std::int32_t pid, std::chrono::seconds elapsed)
{
    return std::format("Writing a dump file of {} (PID {})... {} s", processName, pid, elapsed.count());
}

DumpMessage resultMessage(std::string_view processName, std::int32_t pid, const Platform::ProcessDumpResult& result, bool dumpsFromSnapshot)
{
    if (!result.has_value())
    {
        return {.ok = false,
                .title = "Couldn't create a dump file",
                .text = std::format("Could not create a dump file of {} (PID {}): {}", processName, pid, result.error()),
                .file = {},
                .note = {},
                .method = {}};
    }
    std::string method;
    if (result->fromSnapshot)
    {
        method = "Written from a snapshot: the process paused only while the snapshot was taken.";
    }
    else if (!dumpsFromSnapshot)
    {
        // The platform's only way (Linux's gcore, #2241): not a fallback, so no "no snapshot could be taken".
        method = result->liveReason.empty()
                   ? std::string("Written from the running process, which was stopped while it was written.")
                   : std::format("Written from the running process, which was stopped while it was written ({}).", result->liveReason);
    }
    else
    {
        method = std::format("Written from the running process, which may have stopped responding while it was written: "
                             "no snapshot could be taken ({}).",
                             result->liveReason.empty() ? std::string("unknown reason") : result->liveReason);
    }
    return {.ok = true,
            .title = "Dump file created",
            .text = std::format(
                "Wrote a dump file of {} (PID {}), {}:", processName, pid, UI::Format::formatBytes(static_cast<double>(result->sizeBytes))),
            .file = result->file,
            .note = result->note,
            .method = std::move(method)};
}

std::string folderDialogFailure(std::string_view error)
{
    // SDL names its dialog drivers ("zenity" and "portal") when it can use neither: say what would help.
    if (error.contains("SDL_HINT_FILE_DIALOG_DRIVER") || error.contains("dialog driver unsupported"))
    {
        return "Couldn't show the folder dialog: no folder chooser is available here (it needs xdg-desktop-portal or zenity installed)";
    }
    return std::format("Couldn't show the folder dialog: {}", error);
}

void sdlFolderDialog(const std::filesystem::path& initial, std::function<void(Platform::FolderChoice)> answer)
{
    SDL_Window* window = nullptr;
    try
    {
        const auto& app = Core::Application::get();
        if (app.hasWindow())
        {
            window = app.getWindow().getHandle();
        }
    }
    catch (...) // NOLINT(bugprone-empty-catch): no Application (headless), so no window to show the dialog over
    {}
    if (window == nullptr)
    {
        answer(std::unexpected(std::string("Couldn't show the folder dialog: there is no window to show it over")));
        return;
    }
    Core::showFolderDialog(window,
                           Core::pathToUtf8(initial),
                           [answer = std::move(answer)](const Core::SaveFileResult& result)
                           {
                               if (!result.error.empty())
                               {
                                   answer(std::unexpected(folderDialogFailure(result.error)));
                               }
                               else if (result.path.empty())
                               {
                                   answer(std::optional<std::filesystem::path>{}); // cancelled
                               }
                               else
                               {
                                   answer(std::optional<std::filesystem::path>{Core::utf8ToPath(result.path)});
                               }
                           });
}

} // namespace DumpDetail

ProcessDumpView::ProcessDumpView() : ProcessDumpView(&DumpDetail::rememberDumpDirectory)
{
    m_FolderDialog = &DumpDetail::sdlFolderDialog;
}

bool ProcessDumpView::canChooseFolder(Platform::IProcessActions* actions) const
{
    return (actions != nullptr && actions->actionCapabilities().canChooseDumpFolder) || static_cast<bool>(m_FolderDialog);
}

ProcessDumpView::ProcessDumpView(FolderChosenHandler onFolderChosen) : m_OnFolderChosen(std::move(onFolderChosen))
{}

ProcessDumpView::~ProcessDumpView()
{
    shutdown();
}

ProcessDumpView& ProcessDumpView::operator=(ProcessDumpView&& other) noexcept
{
    if (this != &other)
    {
        shutdown();
        m_OnFolderChosen = std::move(other.m_OnFolderChosen);
        m_ShowConfirm = other.m_ShowConfirm;
        m_DismissConfirm = other.m_DismissConfirm;
        m_ConfirmPending = other.m_ConfirmPending;
        m_Target = other.m_Target;
        m_ProcessName = std::move(other.m_ProcessName);
        m_Directory = std::move(other.m_Directory);
        m_DumpsFromSnapshot = other.m_DumpsFromSnapshot;
        m_ConfirmTitle = std::move(other.m_ConfirmTitle);
        m_ConfirmQuestion = std::move(other.m_ConfirmQuestion);
        m_SpaceWarning = std::move(other.m_SpaceWarning);
        m_FolderFailure = std::move(other.m_FolderFailure);
        m_FolderPick = std::move(other.m_FolderPick);
        m_FolderDialog = std::move(other.m_FolderDialog);
        m_Worker = std::move(other.m_Worker);
        m_StartedAt = other.m_StartedAt;
        m_CancelRequested = other.m_CancelRequested;
        m_Result = std::move(other.m_Result);
    }
    return *this;
}

void ProcessDumpView::request(const Platform::ProcessTarget& target,
                              std::string processName,
                              std::filesystem::path directory,
                              Platform::IProcessActions* actions)
{
    if (active() || target.pid <= 0)
    {
        return;
    }
    m_Target = target;
    m_ProcessName = std::move(processName);
    m_Directory = std::move(directory);
    m_FolderFailure.clear();
    m_DumpsFromSnapshot = false;
    // Built once here, not every frame the dialog is up.
    m_ConfirmTitle = DumpDetail::confirmTitle(m_ProcessName, target.pid);
    refreshConfirm(actions);
    m_ConfirmPending = true;
    m_ShowConfirm = true;
    m_DismissConfirm = false;
}

void ProcessDumpView::refreshConfirm(Platform::IProcessActions* actions)
{
    // Read once per folder, never per frame: the free space and the process's private bytes, and
    // whether the platform dumps from a snapshot (the pause the confirm promises, #2241).
    if (actions != nullptr)
    {
        m_DumpsFromSnapshot = actions->actionCapabilities().dumpsFromSnapshot;
    }
    m_SpaceWarning = (actions != nullptr)
                       ? DumpDetail::lowSpaceWarning(actions->dumpSpace(m_Target, m_Directory), m_Directory, canChooseFolder(actions))
                       : std::string{};
    std::string warning = m_SpaceWarning;
    if (!m_FolderFailure.empty())
    {
        if (!warning.empty())
        {
            warning += "\n\n";
        }
        warning += m_FolderFailure;
    }
    m_ConfirmQuestion = DumpDetail::confirmQuestion(m_ProcessName, m_Target.pid, m_Directory, m_DumpsFromSnapshot, warning);
}

void ProcessDumpView::chooseFolder(Platform::IProcessActions* actions, void* ownerWindow)
{
    if (!m_ConfirmPending || choosingFolder() || !canChooseFolder(actions))
    {
        return;
    }
    if (actions == nullptr || !actions->actionCapabilities().canChooseDumpFolder)
    {
        // No folder dialog in the platform layer (Linux): the fallback answers asynchronously, from SDL's
        // event pump or a thread of its own, into the slot the worker would have filled. shutdown() drops
        // that answer rather than waiting for it (BackgroundTask::adopt()).
        auto promise = std::make_shared<std::promise<Platform::FolderChoice>>();
        m_FolderPick.adopt(promise->get_future());
        m_FolderDialog(m_Directory,
                       [promise](Platform::FolderChoice choice)
                       {
                           try
                           {
                               promise->set_value(std::move(choice));
                           }
                           catch (const std::future_error&) // NOLINT(bugprone-empty-catch): a second answer is ignored
                           {}
                       });
        return;
    }
    // The worker owns a copy of the folder to start in; it shares only its result with this view, and
    // @p actions, which outlives it (see the header). A dialog that throws is a failure, built on the
    // worker (BackgroundTask, #1685).
    if (const auto failed =
            m_FolderPick.start([actions, initial = m_Directory, ownerWindow] { return actions->chooseDumpFolder(initial, ownerWindow); }))
    {
        m_FolderFailure = std::format("Couldn't show the folder dialog: {}", failed->error());
        refreshConfirm(actions);
    }
}

bool ProcessDumpView::takeFolderChoice(Platform::IProcessActions* actions, bool wait)
{
    std::optional<Platform::FolderChoice> taken = m_FolderPick.poll(wait);
    if (!taken.has_value())
    {
        return false;
    }
    Platform::FolderChoice choice = std::move(*taken);
    if (!choice.has_value())
    {
        m_FolderFailure = choice.error();
    }
    else if (choice->has_value())
    {
        m_FolderFailure.clear();
        // Remembered even when the confirm closed meanwhile (a selection change): the user chose it.
        if (m_OnFolderChosen)
        {
            m_OnFolderChosen(**choice);
        }
        m_Directory = std::move(**choice);
    }
    if (m_ConfirmPending)
    {
        refreshConfirm(actions);
    }
    return true;
}

void ProcessDumpView::confirm(Platform::IProcessActions* actions)
{
    if (!m_ConfirmPending || choosingFolder())
    {
        return;
    }
    m_ConfirmPending = false;
    m_ShowConfirm = false;
    if (actions == nullptr)
    {
        m_Result = DumpDetail::resultMessage(m_ProcessName,
                                             m_Target.pid,
                                             Platform::ProcessDumpResult(std::unexpected(std::string("Process actions unavailable"))),
                                             m_DumpsFromSnapshot);
        return;
    }
    // The result says how the dump was written by the actions that write it.
    m_DumpsFromSnapshot = actions->actionCapabilities().dumpsFromSnapshot;
    m_CancelRequested = false;
    m_StartedAt = std::chrono::steady_clock::now();
    // The worker owns copies of the target and folder; it shares only its result and a new stop token's
    // state (a cancel() of an earlier dump does not carry over) with this view, and @p actions, which
    // outlives it (see the header). A dump that throws is a failure, built on the worker (#1685).
    if (const auto failed = m_Worker.start([actions, target = m_Target, directory = m_Directory](const std::stop_token& stopToken)
                                           { return actions->createDump(target, directory, stopToken); }))
    {
        m_Result = DumpDetail::resultMessage(m_ProcessName, m_Target.pid, *failed, m_DumpsFromSnapshot);
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
        m_Worker.requestStop();
    }
}

bool ProcessDumpView::takeFinished(bool wait)
{
    std::optional<Platform::ProcessDumpResult> taken = m_Worker.poll(wait);
    if (!taken.has_value())
    {
        return false;
    }
    const Platform::ProcessDumpResult& result = *taken;
    // A dump the user cancelled closes the dialog without a result to acknowledge; one that finished
    // before the cancel reached it still says where its file is.
    if (m_CancelRequested && !result.has_value())
    {
        m_Result.reset();
    }
    else
    {
        m_Result = DumpDetail::resultMessage(m_ProcessName, m_Target.pid, result, m_DumpsFromSnapshot);
    }
    m_CancelRequested = false;
    return true;
}

void ProcessDumpView::shutdown() noexcept
{
    cancel();
    m_Worker.wait();
    // A folder dialog can't be cancelled from here; it is modal to the main window, so it is answered
    // before the window can close, and this wait is short. The fallback's (SDL's) is answered from the
    // event pump on this thread, so waiting would never end: wait() drops that adopted answer instead
    // (the promise it fills is shared, so a late answer is harmless).
    m_FolderPick.wait();
}

void ProcessDumpView::onSelectionChanged() noexcept
{
    m_DismissConfirm = m_DismissConfirm || confirmPending();
    cancelConfirm();
}

void ProcessDumpView::render(Platform::IProcessActions* actions, const Platform::ProcessTarget* liveTarget)
{
    static_cast<void>(takeFolderChoice(actions));

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
        const bool offerFolder = canChooseFolder(actions);
        bool choosePressed = false;
        const ProcessActionConfirm::LeadingButton chooseButton{.label = offerFolder ? DumpDetail::CHOOSE_FOLDER_LABEL : nullptr,
                                                               .tooltip = DumpDetail::CHOOSE_FOLDER_TOOLTIP,
                                                               .busy = choosingFolder(),
                                                               .pressed = &choosePressed};
        // The shared confirm dialog under an ID of its own: the panel's own confirm (Kill, Suspend...)
        // is submitted every frame too, and must never see this one open as its own.
        ImGui::PushID("ProcessDump");
        const ProcessActionConfirm::Outcome outcome = ProcessActionConfirm::renderLabelled(
            m_ShowConfirm, DumpDetail::CONFIRM_LABEL, false, m_ConfirmTitle, m_ConfirmQuestion, dismiss, chooseButton);
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
        if (choosePressed)
        {
            chooseFolder(actions, mainNativeWindow());
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
        if (!result.method.empty())
        {
            ImGui::TextColored(scheme.textMuted, "%s", result.method.c_str());
        }
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
