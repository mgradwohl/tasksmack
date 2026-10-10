#include "StartupActionsView.h"

#include "App/Panels/ProcessActionConfirm.h"
#include "Platform/IStartupActions.h"
#include "Platform/IStartupProbe.h"
#include "Platform/ThreadName.h"
#include "UI/IconsFontAwesome6.h"
#include "UI/Theme.h"

#include <imgui.h>

#include <chrono>
#include <exception>
#include <format>
#include <future>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace App
{

namespace StartupActionsDetail
{

std::string_view blockedReason(const Platform::StartupEntry& entry, const Platform::StartupActionCapabilities& caps)
{
    if (!caps.canSetEnabled)
    {
        return "Startup apps can't be changed on this platform yet";
    }
    if (!Platform::hasSwitchableState(entry.location))
    {
        return "Run-once entries run at the next sign-in only and have no enabled state";
    }
    if (entry.scope == Platform::StartupScope::Machine && !caps.elevated)
    {
        return "Requires administrator";
    }
    return {};
}

bool isApplicable(bool enable, const Platform::StartupEntry& entry, const Platform::StartupActionCapabilities& caps)
{
    return entry.enabled != enable && blockedReason(entry, caps).empty();
}

std::string confirmTitle(const StartupActionRequest& request)
{
    return std::format("Disable {}?", request.entry.name);
}

std::string confirmQuestion(const StartupActionRequest& request)
{
    return std::format("Disable {}? It won't start when you sign in until it is enabled again. Its registry entry or shortcut is "
                       "kept, so nothing is removed.",
                       request.entry.name);
}

std::string progressText(const StartupActionRequest& request)
{
    return std::format("{} {}...", request.enable ? "Enabling" : "Disabling", request.entry.name);
}

StartupActionMessage resultMessage(const StartupActionRequest& request, const Platform::StartupActionResult& result)
{
    if (result.ok)
    {
        return {.ok = true, .text = std::format("{} {}", request.enable ? "Enabled" : "Disabled", request.entry.name)};
    }
    return {.ok = false,
            .text = std::format("Could not {} {}: {}", request.enable ? "enable" : "disable", request.entry.name, result.message)};
}

} // namespace StartupActionsDetail

namespace AutoDetail = StartupActionsDetail;

namespace
{

/// Runs on the worker: the action, or a failed result for one that threw. The exception is caught and
/// its message copied here, on the thread that threw it, so the UI thread gets a plain result through
/// the future and never an exception object (#1685, #1706). Only building the failed result itself
/// (bad_alloc) can still escape, to takeFinished().
[[nodiscard]] Platform::StartupActionResult setEnabledOrFailed(Platform::IStartupActions& actions, const StartupActionRequest& request)
{
    try
    {
        return actions.setEnabled(request.entry, request.enable);
    }
    catch (const std::exception& e)
    {
        return Platform::StartupActionResult::failed(std::string(e.what()));
    }
    catch (...)
    {
        return Platform::StartupActionResult::failed("Unknown error");
    }
}

} // namespace

StartupActionsView::StartupActionsView(std::shared_ptr<Platform::IStartupActions> actions)
    : m_Actions(actions ? std::move(actions) : std::make_shared<Platform::UnsupportedStartupActions>()),
      m_Capabilities(m_Actions->capabilities())
{}

StartupActionsView::~StartupActionsView()
{
    // A running action is waited for: its result must not outlive the view it reports to.
    try
    {
        if (m_Worker.valid())
        {
            m_Worker.wait();
        }
    }
    catch (...) // NOLINT(bugprone-empty-catch): a destructor; wait() on a valid future does not throw
    {}
}

void StartupActionsView::request(StartupActionRequest request)
{
    if (busy() || m_ShowConfirm)
    {
        return;
    }
    if (!AutoDetail::needsConfirm(request))
    {
        run(std::move(request));
        return;
    }
    // Built once here, not every frame the dialog is up.
    m_ConfirmTitle = AutoDetail::confirmTitle(request);
    m_ConfirmQuestion = AutoDetail::confirmQuestion(request);
    m_Confirm = std::move(request);
    m_ShowConfirm = true;
}

void StartupActionsView::run(StartupActionRequest request)
{
    m_Result = {};
    m_Running = request;
    try
    {
        // The worker owns copies of everything it uses; nothing here is shared with it but the future.
        m_Worker = std::async(std::launch::async,
                              [actions = m_Actions, request = std::move(request)]
                              {
                                  static_cast<void>(Platform::setCurrentThreadName(Platform::STARTUP_ACTION_THREAD_NAME));
                                  return setEnabledOrFailed(*actions, request);
                              });
    }
    catch (const std::system_error& e)
    {
        m_Result = AutoDetail::resultMessage(m_Running, Platform::StartupActionResult::failed(e.what()));
        m_ResultSecondsLeft = AutoDetail::RESULT_SECONDS;
    }
}

bool StartupActionsView::takeFinished()
{
    if (!m_Worker.valid() || m_Worker.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
    {
        return false;
    }
    Platform::StartupActionResult result;
    try
    {
        result = m_Worker.get();
    }
    catch (...)
    {
        // The worker turns a throwing action into a failed result itself (setEnabledOrFailed()), so only
        // building that result can land here (bad_alloc). The exception object was thrown on the worker
        // and is not read here (#1685, #1706).
        result = Platform::StartupActionResult::failed("Unknown error");
    }
    m_Result = AutoDetail::resultMessage(m_Running, result);
    m_ResultSecondsLeft = AutoDetail::RESULT_SECONDS;
    return true;
}

void StartupActionsView::tick(float deltaSeconds) noexcept
{
    if (m_ResultSecondsLeft > 0.0F)
    {
        m_ResultSecondsLeft -= deltaSeconds;
        if (m_ResultSecondsLeft <= 0.0F)
        {
            m_Result.text.clear();
        }
    }
}

void StartupActionsView::renderActionBar(const Platform::StartupEntry* selected)
{
    if (!supported())
    {
        return;
    }
    const std::string reason((selected != nullptr) ? AutoDetail::blockedReason(*selected, m_Capabilities) : std::string_view());
    const auto button = [&](bool enable, const char* label, const char* tooltip)
    {
        const bool enabled = selected != nullptr && !busy() && AutoDetail::isApplicable(enable, *selected, m_Capabilities);
        ImGui::BeginDisabled(!enabled);
        if (ImGui::Button(label) && enabled)
        {
            request({.enable = enable, .entry = *selected});
        }
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("%s", reason.empty() ? tooltip : reason.c_str());
        ImGui::SameLine();
    };
    button(true, ICON_FA_CIRCLE_CHECK " Enable", "Start the selected app when you sign in");
    button(false, ICON_FA_POWER_OFF " Disable", "Don't start the selected app when you sign in (it can be enabled again)");

    const auto& scheme = UI::Theme::get().scheme();
    if (selected == nullptr)
    {
        ImGui::TextColored(scheme.textMuted, "Select a startup app");
    }
    else if (!reason.empty())
    {
        ImGui::TextColored(scheme.textMuted, ICON_FA_CIRCLE_INFO "  %s", reason.c_str());
    }
    else
    {
        ImGui::TextColored(scheme.textMuted, "%s", selected->name.c_str());
    }
}

void StartupActionsView::renderContextMenu(const Platform::StartupEntry& entry)
{
    if (!supported() || !ImGui::BeginPopupContextItem("##StartupRowMenu"))
    {
        return;
    }
    ImGui::TextDisabled("%s", entry.name.c_str());
    ImGui::Separator();
    const std::string reason(AutoDetail::blockedReason(entry, m_Capabilities));
    const auto item = [&](bool enable, const char* label)
    {
        if (ImGui::MenuItem(label, nullptr, false, !busy() && AutoDetail::isApplicable(enable, entry, m_Capabilities)))
        {
            request({.enable = enable, .entry = entry});
        }
        if (!reason.empty())
        {
            ImGui::SetItemTooltip("%s", reason.c_str());
        }
    };
    item(true, ICON_FA_CIRCLE_CHECK "  Enable");
    item(false, ICON_FA_POWER_OFF "  Disable");
    if (!reason.empty())
    {
        ImGui::Separator();
        ImGui::TextDisabled("%s", reason.c_str());
    }
    ImGui::EndPopup();
}

void StartupActionsView::renderResultLine() const
{
    const auto& scheme = UI::Theme::get().scheme();
    if (busy())
    {
        ImGui::TextColored(scheme.textMuted, "%s", AutoDetail::progressText(m_Running).c_str());
    }
    else if (!m_Result.text.empty())
    {
        // The colour comes from the result's flag, never from its text.
        ImGui::TextColored(m_Result.ok ? scheme.textSuccess : scheme.textError, "%s", m_Result.text.c_str());
    }
}

void StartupActionsView::renderConfirmation()
{
    if (!m_Confirm)
    {
        return;
    }
    switch (ProcessActionConfirm::renderLabelled(m_ShowConfirm, "Disable", true, m_ConfirmTitle, m_ConfirmQuestion))
    {
    case ProcessActionConfirm::Outcome::Confirmed:
        run(m_Confirm.value());
        m_Confirm.reset();
        break;
    case ProcessActionConfirm::Outcome::Cancelled:
        m_Confirm.reset();
        break;
    case ProcessActionConfirm::Outcome::None:
        break;
    }
}

} // namespace App
