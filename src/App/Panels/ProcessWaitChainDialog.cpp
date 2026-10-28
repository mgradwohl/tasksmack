#include "ProcessWaitChainDialog.h"

#include "App/Panels/ProcessActionConfirm.h"
#include "Platform/IProcessActions.h"
#include "Platform/WaitChain.h"
#include "ProcessDetailsPanel_ActionHelpers.h"
#include "UI/ChromeWidgets.h"
#include "UI/DialogMetrics.h"
#include "UI/IconsFontAwesome6.h"
#include "UI/Theme.h"
#include "UI/Widgets.h"

#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace App
{

namespace
{

// The "###" part keeps the popup's ID fixed while its title names the process.
constexpr const char* POPUP_ID = "###WaitChain";
constexpr const char* CLOSE_LABEL = "Close";
constexpr const char* REFRESH_LABEL = ICON_FA_ARROWS_ROTATE " Refresh";
constexpr const char* END_PROCESS_LABEL = "End process...";

// Floor on the footer's buttons, in ems, as for the other action dialogs (ProcessActionConfirm).
constexpr float BUTTON_MIN_EM = 11.25F;
// The dialog's width in ems: a thread, an object's name and a process's on one line.
constexpr float DIALOG_WIDTH_EM = 52.0F;
// The tree's height in lines; more scroll.
constexpr float TREE_LINES = 18.0F;

/// The thread @p node's own figures, for its tooltip.
[[nodiscard]] std::string threadTooltip(const Platform::WaitChainNode& node)
{
    return std::format("Context switches: {}\nWait time (as Windows reports it): {}", node.contextSwitches, node.waitTime);
}

} // namespace

namespace WaitChainDetail
{

std::string objectTypeLabel(Platform::WaitObjectType type, std::uint32_t rawType)
{
    switch (type)
    {
    case Platform::WaitObjectType::CriticalSection:
        return "Critical section";
    case Platform::WaitObjectType::SendMessageCall:
        return "SendMessage to a window";
    case Platform::WaitObjectType::Mutex:
        return "Mutex";
    case Platform::WaitObjectType::Alpc:
        return "ALPC call";
    case Platform::WaitObjectType::Com:
        return "COM call";
    case Platform::WaitObjectType::ThreadWait:
        return "Wait for a thread";
    case Platform::WaitObjectType::ProcessWait:
        return "Wait for a process";
    case Platform::WaitObjectType::Thread:
        return "Thread";
    case Platform::WaitObjectType::ComActivation:
        return "COM activation";
    case Platform::WaitObjectType::SocketIo:
        return "Socket I/O";
    case Platform::WaitObjectType::SmbIo:
        return "SMB I/O";
    case Platform::WaitObjectType::Unknown:
        break;
    }
    return std::format("Unknown ({})", rawType);
}

std::string nodeText(const Platform::WaitChainNode& node, std::int32_t analyzedPid)
{
    std::string text;
    if (node.type == Platform::WaitObjectType::Thread)
    {
        const std::string process =
            node.processName.empty() ? std::format("PID {}", node.pid) : std::format("{} (PID {})", node.processName, node.pid);
        if (node.tid == 0)
        {
            // Only the process that owns the object is known (an ALPC or COM server).
            text = std::format("Process {}", process);
        }
        else if (node.pid == analyzedPid || node.pid <= 0)
        {
            text = std::format("Thread {}", node.tid);
        }
        else
        {
            text = std::format("Thread {} of {}", node.tid, process);
        }
    }
    else
    {
        text = objectTypeLabel(node.type, node.rawType);
        if (!node.objectName.empty())
        {
            text += std::format(" \"{}\"", node.objectName);
        }
    }
    if (const std::string_view status = nodeStatusLabel(node.status); !status.empty())
    {
        text += std::format(" ({})", status);
    }
    return text;
}

std::vector<ThreadRows>
buildThreadRows(const Platform::WaitChainReadResult& result, const Platform::ProcessTarget& analyzed, std::int32_t ownPid, bool canEnd)
{
    std::vector<ThreadRows> rows;
    rows.reserve(result.threads.size());
    for (const Platform::ThreadWaitChain& chain : result.threads)
    {
        ThreadRows thread;
        thread.deadlocked = chain.isCycle;
        thread.failed = !chain.error.empty();
        thread.waiting = chain.nodes.size() > 1;
        if (thread.failed)
        {
            thread.header = std::format("Thread {}: not read", chain.tid);
            thread.note = chain.error;
        }
        else if (thread.deadlocked)
        {
            thread.header = std::format(ICON_FA_TRIANGLE_EXCLAMATION " Thread {}: deadlocked", chain.tid);
        }
        else if (thread.waiting)
        {
            thread.header =
                std::format("Thread {}: waiting on {}", chain.tid, objectTypeLabel(chain.nodes[1].type, chain.nodes[1].rawType));
        }
        else
        {
            const std::string_view status = chain.nodes.empty() ? std::string_view{} : nodeStatusLabel(chain.nodes.front().status);
            thread.header = status.empty() ? std::format("Thread {}", chain.tid) : std::format("Thread {}: {}", chain.tid, status);
        }
        if (!chain.nodes.empty())
        {
            thread.tooltip = threadTooltip(chain.nodes.front());
        }
        for (std::size_t i = 1; i < chain.nodes.size(); ++i)
        {
            const Platform::WaitChainNode& node = chain.nodes[i];
            NodeRow row{.text = nodeText(node, analyzed.pid), .tooltip = {}, .endTarget = {.pid = -1, .startTimeTicks = 0}, .endName = {}};
            if (node.type == Platform::WaitObjectType::Thread)
            {
                row.tooltip = threadTooltip(node);
                const bool otherProcess = node.pid > 0 && node.pid != analyzed.pid && node.pid != ownPid;
                if (canEnd && otherProcess && node.processStartTimeTicks != 0)
                {
                    row.endTarget = {.pid = node.pid, .startTimeTicks = node.processStartTimeTicks};
                    row.endName = node.processName.empty() ? std::format("PID {}", node.pid) : node.processName;
                }
            }
            thread.nodes.push_back(std::move(row));
        }
        if (chain.truncated)
        {
            thread.note = std::format("The chain goes on past the {} nodes Windows follows.", Platform::WAIT_CHAIN_MAX_NODES);
        }
        rows.push_back(std::move(thread));
    }
    // Deadlocks first, then the threads waiting on something, then the rest, each in Windows' order.
    const auto rank = [](const ThreadRows& row)
    {
        if (row.deadlocked)
        {
            return 0;
        }
        return row.waiting ? 1 : 2;
    };
    std::ranges::stable_sort(rows, [&rank](const ThreadRows& a, const ThreadRows& b) { return rank(a) < rank(b); });
    return rows;
}

std::string summaryText(const Platform::WaitChainReadResult& result, std::span<const ThreadRows> threads)
{
    const auto deadlocked = std::ranges::count_if(threads, [](const ThreadRows& row) { return row.deadlocked; });
    const auto waiting = std::ranges::count_if(threads, [](const ThreadRows& row) { return row.waiting && !row.deadlocked; });
    std::string text = std::format("{} {}", threads.size(), threads.size() == 1 ? "thread" : "threads");
    if (deadlocked > 0 || waiting > 0)
    {
        text += std::format(": {} waiting on another thread or object", waiting);
        if (deadlocked > 0)
        {
            text += std::format(", {} deadlocked", deadlocked);
        }
    }
    else
    {
        text += ", none waiting on another thread or object";
    }
    if (result.threadsTruncated && result.threadCount > threads.size())
    {
        text +=
            std::format(". {} more not read: the process has too many threads, or they took too long", result.threadCount - threads.size());
    }
    return text;
}

} // namespace WaitChainDetail

void ProcessWaitChainDialog::open(Platform::IProcessActions* actions,
                                  const Platform::ProcessTarget& target,
                                  std::string name,
                                  std::int32_t ownPid)
{
    m_Actions = actions;
    m_Target = target;
    m_Name = std::move(name);
    m_OwnPid = ownPid;
    m_Title = std::format("{}  Wait chain of {} (PID {}){}", ICON_FA_HOURGLASS_HALF, m_Name, m_Target.pid, POPUP_ID);
    m_Result.reset();
    m_Threads.clear();
    m_Summary.clear();
    m_EndTarget = {.pid = -1, .startTimeTicks = 0};
    m_EndName.clear();
    m_EndResult = {};
    m_ShowEndConfirm = false;
    // Kill where the platform has it: a process that holds others up rarely answers a request to close.
    m_EndAction = Detail::ProcessAction::None;
    if (actions != nullptr)
    {
        const Platform::ProcessActionCapabilities capabilities = actions->actionCapabilities();
        if (capabilities.canKill)
        {
            m_EndAction = Detail::ProcessAction::Kill;
        }
        else if (capabilities.canTerminate)
        {
            m_EndAction = Detail::ProcessAction::Terminate;
        }
    }
    m_Requested = true;
    m_ShowRequested = true;
    m_CloseRequested = false;
    startRead();
}

void ProcessWaitChainDialog::startRead()
{
    ++m_Generation;
    launchRead();
}

void ProcessWaitChainDialog::launchRead()
{
    if (m_Actions == nullptr)
    {
        m_ReadPending = false;
        takeResult(failedRead("Process actions unavailable"));
        return;
    }
    if (m_Read.busy())
    {
        // The read in flight was for an earlier opening: stop it, and read once it has ended (update()).
        m_Read.requestStop();
        m_ReadPending = true;
        return;
    }
    m_ReadPending = false;
    m_ReadGeneration = m_Generation;
    if (auto failed = m_Read.start([actions = m_Actions, target = m_Target](const std::stop_token& stopToken)
                                   { return actions->readWaitChains(target, stopToken); }))
    {
        takeResult(std::move(*failed));
    }
}

void ProcessWaitChainDialog::update(bool wait)
{
    m_EndRun.update(wait);
    if (std::optional<Platform::WaitChainReadResult> result = m_Read.poll(wait))
    {
        if (m_ReadGeneration == m_Generation && m_Requested)
        {
            takeResult(std::move(*result));
        }
    }
    if (m_ReadPending && !m_Read.busy() && m_Requested)
    {
        launchRead(); // taken in by a later update()
    }
}

void ProcessWaitChainDialog::takeResult(Platform::WaitChainReadResult result)
{
    m_Threads = WaitChainDetail::buildThreadRows(result, m_Target, m_OwnPid, m_EndAction != Detail::ProcessAction::None);
    m_Summary = WaitChainDetail::summaryText(result, m_Threads);
    m_Result = std::move(result);
}

void ProcessWaitChainDialog::refresh()
{
    if (!m_Requested || m_Read.busy())
    {
        return;
    }
    startRead();
}

void ProcessWaitChainDialog::close() noexcept
{
    m_CloseRequested = m_Requested;
    m_Requested = false;
    m_ShowRequested = false;
    m_ShowEndConfirm = false;
    m_ReadPending = false;
    ++m_Generation; // a read in flight is dropped when it arrives
    m_Read.requestStop();
}

void ProcessWaitChainDialog::shutdown()
{
    close();
    m_EndRun.requestStop();
    m_EndRun.update(true);
    m_Read.wait();
    static_cast<void>(m_Read.poll());
}

void ProcessWaitChainDialog::requestEnd(const Platform::ProcessTarget& target, std::string name)
{
    if (m_EndAction == Detail::ProcessAction::None || m_EndRun.busy() || target.pid <= 0)
    {
        return;
    }
    m_EndTarget = target;
    m_EndName = std::move(name);
    m_ShowEndConfirm = true;
}

void ProcessWaitChainDialog::confirmEnd()
{
    if (m_EndTarget.pid <= 0 || m_Actions == nullptr || m_EndAction == Detail::ProcessAction::None)
    {
        return;
    }
    const Platform::ProcessTarget target = m_EndTarget;
    const Detail::ProcessAction action = m_EndAction;
    m_EndTarget = {.pid = -1, .startTimeTicks = 0};
    m_ShowEndConfirm = false;
    m_EndResult = {};
    static_cast<void>(m_EndRun.start(
        std::format("{} {} (PID {})...", Detail::actionProgressLabel(action), m_EndName, target.pid),
        [actions = m_Actions, action, target] { return Detail::dispatchProcessAction(*actions, action, target); },
        [this, action, pid = target.pid](const Platform::ProcessActionResult& result)
        {
            m_EndResult = Detail::formatActionResultMessage(action, pid, result);
            if (result.success)
            {
                refresh(); // the chains it held up have moved on
            }
        }));
}

void ProcessWaitChainDialog::render()
{
    update(false);
    if (m_ShowRequested)
    {
        ImGui::OpenPopup(POPUP_ID);
        m_ShowRequested = false;
    }
    if (!ImGui::IsPopupOpen(POPUP_ID))
    {
        m_CloseRequested = false;
        return;
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    UI::Widgets::centerNextDialog(m_LastWorkSize);
    const float width = std::min((DIALOG_WIDTH_EM * ImGui::GetFontSize()) + (ImGui::GetStyle().WindowPadding.x * 2.0F),
                                 UI::DialogMetrics::computeDialogMaxExtent(viewport->WorkSize.x));
    ImGui::SetNextWindowSize(ImVec2(width, 0.0F));
    UI::Widgets::setNextDialogSizeConstraints(ImVec2(UI::DialogMetrics::computeDialogMaxExtent(viewport->WorkSize.x),
                                                     UI::DialogMetrics::computeDialogMaxExtent(viewport->WorkSize.y)));
    if (!ImGui::BeginPopupModal(m_Title.c_str(), nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings))
    {
        return;
    }
    UI::Widgets::keepCurrentWindowInViewport();
    if (std::exchange(m_CloseRequested, false))
    {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    // Escape closes, as Close does, unless it was for the End process confirmation on top.
    const bool escaped = !m_ShowEndConfirm && UI::Widgets::dialogEscapePressed();
    const auto& theme = UI::Theme::get();

    ImGui::PushTextWrapPos(0.0F);
    ImGui::TextUnformatted("What each of this process's threads is waiting for, and the thread or process that holds it up. "
                           "A deadlock is a chain that loops back on itself: its threads wait on each other and never go on.");
    ImGui::PopTextWrapPos();
    UI::Widgets::sectionGap();

    if (!m_Result.has_value())
    {
        // The first read, still running: the pending state.
        ImGui::TextColored(theme.scheme().textMuted,
                           ICON_FA_HOURGLASS_HALF " %.*s",
                           static_cast<int>(WaitChainDetail::READING_TEXT.size()),
                           WaitChainDetail::READING_TEXT.data());
    }
    else if (m_Result->status != Platform::WaitChainReadStatus::Ok)
    {
        const std::string_view text = WaitChainDetail::readStatusText(m_Result->status);
        ImGui::PushTextWrapPos(0.0F);
        ImGui::TextColored(theme.scheme().textError, "%.*s", static_cast<int>(text.size()), text.data());
        if (!m_Result->detail.empty())
        {
            ImGui::TextColored(theme.scheme().textMuted, "%s", m_Result->detail.c_str());
        }
        ImGui::PopTextWrapPos();
    }
    else
    {
        ImGui::PushTextWrapPos(0.0F);
        ImGui::TextColored(theme.scheme().textMuted, "%s%s", m_Summary.c_str(), reading() ? ". Reading again..." : "");
        ImGui::PopTextWrapPos();
        renderThreads();
    }

    // End process's progress, then its result.
    if (m_EndRun.busy())
    {
        ImGui::TextColored(theme.scheme().textMuted, "%s", m_EndRun.pendingText().c_str());
    }
    else if (!m_EndResult.text.empty())
    {
        ImGui::PushTextWrapPos(0.0F);
        ImGui::TextColored(m_EndResult.ok ? theme.scheme().textSuccess : theme.scheme().textError, "%s", m_EndResult.text.c_str());
        ImGui::PopTextWrapPos();
    }

    const UI::Widgets::DialogFooterButton closeButton{.label = CLOSE_LABEL, .fills = nullptr, .tooltip = nullptr};
    const UI::Widgets::DialogFooterButton refreshButton{
        .label = REFRESH_LABEL, .fills = nullptr, .tooltip = "Read the wait chains again", .disabled = reading()};
    UI::Widgets::DialogFooterAction pressed =
        UI::Widgets::dialogFooter(closeButton, {}, UI::Widgets::footerButtonWidth({CLOSE_LABEL}, BUTTON_MIN_EM), refreshButton);
    if (escaped)
    {
        pressed = UI::Widgets::DialogFooterAction::Primary;
    }

    // The End process confirmation, over this dialog: the same one the Actions card and the row menu ask.
    if (m_EndAction != Detail::ProcessAction::None && (m_ShowEndConfirm || m_EndTarget.pid > 0))
    {
        const ProcessActionConfirm::Outcome outcome =
            ProcessActionConfirm::render(m_ShowEndConfirm, m_EndAction, m_EndName, m_EndTarget.pid);
        if (outcome == ProcessActionConfirm::Outcome::Confirmed)
        {
            confirmEnd();
        }
        else if (outcome == ProcessActionConfirm::Outcome::Cancelled)
        {
            m_EndTarget = {.pid = -1, .startTimeTicks = 0};
        }
    }

    switch (pressed)
    {
    case UI::Widgets::DialogFooterAction::Primary:
        close();
        m_CloseRequested = false;
        ImGui::CloseCurrentPopup();
        break;
    case UI::Widgets::DialogFooterAction::Leading:
        refresh();
        break;
    case UI::Widgets::DialogFooterAction::None:
    case UI::Widgets::DialogFooterAction::Secondary:
        break;
    }
    ImGui::EndPopup();
}

void ProcessWaitChainDialog::renderThreads()
{
    const auto& theme = UI::Theme::get();
    const float height = (TREE_LINES * ImGui::GetTextLineHeightWithSpacing()) + (ImGui::GetStyle().WindowPadding.y * 2.0F);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme.scheme().childBg);
    ImGui::PushStyleColor(ImGuiCol_Border, theme.scheme().separator);
    if (ImGui::BeginChild("##WaitChains", ImVec2(-FLT_MIN, height), ImGuiChildFlags_Borders))
    {
        if (m_Threads.empty())
        {
            ImGui::TextColored(theme.scheme().textMuted, "No threads were read.");
        }
        std::optional<std::pair<Platform::ProcessTarget, std::string>> endPressed;
        for (std::size_t i = 0; i < m_Threads.size(); ++i)
        {
            const WaitChainDetail::ThreadRows& thread = m_Threads[i];
            ImGui::PushID(static_cast<int>(i));
            ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth;
            if (thread.waiting || thread.deadlocked || thread.failed)
            {
                flags |= ImGuiTreeNodeFlags_DefaultOpen;
            }
            if (thread.nodes.empty() && thread.note.empty())
            {
                flags |= ImGuiTreeNodeFlags_Leaf;
            }
            // A deadlock in the danger colour; a chain that couldn't be read dimmed.
            const bool tinted = thread.deadlocked || thread.failed;
            if (tinted)
            {
                ImGui::PushStyleColor(ImGuiCol_Text, thread.deadlocked ? theme.scheme().textError : theme.scheme().textMuted);
            }
            const bool open = ImGui::TreeNodeEx("##Thread", flags, "%s", thread.header.c_str());
            if (tinted)
            {
                ImGui::PopStyleColor();
            }
            if (!thread.tooltip.empty())
            {
                ImGui::SetItemTooltip("%s", thread.tooltip.c_str());
            }
            if (open)
            {
                // Each node a level under the one it follows: the thread, what it waits on, who holds that...
                // Each one's ID is under the node before it, so one "##Node" a level never collides.
                int pushed = 0;
                for (std::size_t n = 0; n < thread.nodes.size(); ++n)
                {
                    const WaitChainDetail::NodeRow& node = thread.nodes[n];
                    ImGuiTreeNodeFlags nodeFlags = ImGuiTreeNodeFlags_DefaultOpen;
                    if (n + 1 == thread.nodes.size())
                    {
                        nodeFlags |= ImGuiTreeNodeFlags_Leaf;
                    }
                    if (thread.deadlocked)
                    {
                        ImGui::PushStyleColor(ImGuiCol_Text, theme.scheme().textError);
                    }
                    const bool nodeOpen = ImGui::TreeNodeEx("##Node", nodeFlags, "%s", node.text.c_str());
                    if (thread.deadlocked)
                    {
                        ImGui::PopStyleColor();
                    }
                    if (!node.tooltip.empty())
                    {
                        ImGui::SetItemTooltip("%s", node.tooltip.c_str());
                    }
                    if (node.endTarget.pid > 0)
                    {
                        ImGui::SameLine();
                        ImGui::BeginDisabled(m_EndRun.busy());
                        if (ImGui::SmallButton(END_PROCESS_LABEL))
                        {
                            endPressed.emplace(node.endTarget, node.endName);
                        }
                        ImGui::EndDisabled();
                        ImGui::SetItemTooltip(
                            "End %s (PID %d), which holds this chain up, after asking", node.endName.c_str(), node.endTarget.pid);
                    }
                    if (!nodeOpen)
                    {
                        break;
                    }
                    ++pushed;
                }
                if (!thread.note.empty())
                {
                    ImGui::TextColored(theme.scheme().textMuted, "%s", thread.note.c_str());
                }
                for (int p = 0; p < pushed; ++p)
                {
                    ImGui::TreePop();
                }
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
        if (endPressed.has_value())
        {
            requestEnd(endPressed->first, std::move(endPressed->second));
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor(2);
}

} // namespace App
