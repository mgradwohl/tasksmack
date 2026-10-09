#include "ServiceActionsView.h"

#include "App/Panels/ProcessActionConfirm.h"
#include "App/Panels/ServicesView.h"
#include "Platform/IServiceActions.h"
#include "Platform/IServiceProbe.h"
#include "Platform/ThreadName.h"
#include "UI/IconsFontAwesome6.h"
#include "UI/Theme.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <exception>
#include <format>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace App
{

namespace ServiceActionsDetail
{

namespace
{

/// Services Windows can't run well without (short names). Kept short and well known: a service missing
/// here still gets the ordinary confirm.
constexpr std::array<std::string_view, 20> CRITICAL_SERVICES{
    "BFE",
    "BrokerInfrastructure",
    "CoreMessagingRegistrar",
    "CryptSvc",
    "DcomLaunch",
    "Dhcp",
    "Dnscache",
    "EventLog",
    "gpsvc",
    "LSM",
    "mpssvc",
    "PlugPlay",
    "Power",
    "ProfSvc",
    "RpcEptMapper",
    "RpcSs",
    "SamSs",
    "Schedule",
    "SystemEventsBroker",
    "Winmgmt",
};

[[nodiscard]] bool equalsIgnoringCase(std::string_view a, std::string_view b)
{
    return std::ranges::equal(a, b, [](unsigned char x, unsigned char y) { return std::tolower(x) == std::tolower(y); });
}

[[nodiscard]] std::string_view verb(ServiceActionKind kind)
{
    switch (kind)
    {
    case ServiceActionKind::Start:
        return "start";
    case ServiceActionKind::Stop:
        return "stop";
    case ServiceActionKind::Restart:
        return "restart";
    case ServiceActionKind::SetStartType:
        break;
    }
    return "change";
}

[[nodiscard]] Platform::ServiceActionResult dispatch(Platform::IServiceActions& actions, const ServiceActionRequest& request)
{
    switch (request.kind)
    {
    case ServiceActionKind::Start:
        return actions.start(request.name);
    case ServiceActionKind::Stop:
        return actions.stop(request.name);
    case ServiceActionKind::Restart:
        return actions.restart(request.name);
    case ServiceActionKind::SetStartType:
        break;
    }
    return actions.setStartType(request.name, request.startType);
}

} // namespace

bool isCriticalService(std::string_view name, std::string_view serviceType)
{
    return serviceType == "Driver" ||
           std::ranges::any_of(CRITICAL_SERVICES, [name](std::string_view critical) { return equalsIgnoringCase(name, critical); });
}

bool isApplicable(ServiceActionKind kind, Platform::ServiceState state, const Platform::ServiceActionCapabilities& caps)
{
    using enum Platform::ServiceState;
    switch (kind)
    {
    case ServiceActionKind::Start:
        return caps.canStart && state == Stopped;
    case ServiceActionKind::Stop:
        return caps.canStop && (state == Running || state == Paused);
    case ServiceActionKind::Restart:
        return caps.canRestart && state == Running;
    case ServiceActionKind::SetStartType:
        break;
    }
    return caps.canSetStartType;
}

bool needsConfirm(const ServiceActionRequest& request)
{
    return request.kind == ServiceActionKind::Stop || request.kind == ServiceActionKind::Restart ||
           (request.kind == ServiceActionKind::SetStartType && request.startType == Platform::ServiceStartType::Disabled);
}

ServiceActionRequest makeRequest(ServiceActionKind kind, const Platform::ServiceInfo& service, Platform::ServiceStartType startType)
{
    return {.kind = kind,
            .name = service.name,
            .displayName = service.displayName.empty() ? service.name : service.displayName,
            .startType = startType,
            .critical = isCriticalService(service.name, service.serviceType)};
}

const char* confirmLabel(const ServiceActionRequest& request)
{
    switch (request.kind)
    {
    case ServiceActionKind::Stop:
        return "Stop";
    case ServiceActionKind::Restart:
        return "Restart";
    case ServiceActionKind::Start:
    case ServiceActionKind::SetStartType:
        break;
    }
    return request.kind == ServiceActionKind::Start ? "Start" : "Disable";
}

std::string confirmTitle(const ServiceActionRequest& request)
{
    return std::format("{} {}?", confirmLabel(request), request.name);
}

std::string confirmQuestion(const ServiceActionRequest& request)
{
    std::string question;
    if (request.critical)
    {
        question = std::format(ICON_FA_TRIANGLE_EXCLAMATION "  {} is critical to Windows. Stopping or disabling it can make Windows "
                                                            "unstable, cut network access or sign you out.\n\n",
                               request.name);
    }
    switch (request.kind)
    {
    case ServiceActionKind::Restart:
        question += std::format(
            "Restart {} ({})? It is stopped and started again; programs using it may be interrupted.", request.displayName, request.name);
        break;
    case ServiceActionKind::SetStartType:
        question += std::format("Disable {} ({})? It won't start at all, at boot or on demand, until its startup type is changed back.",
                                request.displayName,
                                request.name);
        break;
    case ServiceActionKind::Start:
    case ServiceActionKind::Stop:
        question += std::format("{} {} ({})? Programs using it may stop working until it is started again.",
                                confirmLabel(request),
                                request.displayName,
                                request.name);
        break;
    }
    return question;
}

std::string progressText(const ServiceActionRequest& request)
{
    switch (request.kind)
    {
    case ServiceActionKind::Start:
        return std::format("Starting {}...", request.name);
    case ServiceActionKind::Stop:
        return std::format("Stopping {}...", request.name);
    case ServiceActionKind::Restart:
        return std::format("Restarting {}...", request.name);
    case ServiceActionKind::SetStartType:
        break;
    }
    return std::format("Setting {} to {}...", request.name, serviceStartTypeLabel(request.startType));
}

ServiceActionMessage resultMessage(const ServiceActionRequest& request, const Platform::ServiceActionResult& result)
{
    const bool setType = request.kind == ServiceActionKind::SetStartType;
    if (result.ok)
    {
        if (setType)
        {
            return {.ok = true, .text = std::format("Set {} to {}", request.name, serviceStartTypeLabel(request.startType))};
        }
        constexpr std::array<std::string_view, 3> DONE{"Started", "Stopped", "Restarted"};
        return {.ok = true, .text = std::format("{} {}", DONE.at(static_cast<std::size_t>(request.kind)), request.name)};
    }
    if (setType)
    {
        return {.ok = false,
                .text = std::format("Could not set {} to {}: {}", request.name, serviceStartTypeLabel(request.startType), result.message)};
    }
    return {.ok = false, .text = std::format("Could not {} {}: {}", verb(request.kind), request.name, result.message)};
}

} // namespace ServiceActionsDetail

namespace SvcDetail = ServiceActionsDetail;

ServiceActionsView::ServiceActionsView(std::shared_ptr<Platform::IServiceActions> actions)
    : m_Actions(actions ? std::move(actions) : std::make_shared<Platform::UnsupportedServiceActions>()),
      m_Capabilities(m_Actions->capabilities())
{}

ServiceActionsView::~ServiceActionsView()
{
    // A running action is waited for: it holds its own shared_ptr to the actions, but its result must
    // not outlive the view it reports to. The wait is bounded by the platform's own timeout.
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

bool ServiceActionsView::supported() const noexcept
{
    return m_Capabilities.canStart || m_Capabilities.canStop || m_Capabilities.canRestart || m_Capabilities.canSetStartType;
}

void ServiceActionsView::request(ServiceActionRequest request)
{
    if (busy() || m_ShowConfirm)
    {
        return;
    }
    if (!SvcDetail::needsConfirm(request))
    {
        run(std::move(request));
        return;
    }
    // Built once here, not every frame the dialog is up.
    m_ConfirmTitle = SvcDetail::confirmTitle(request);
    m_ConfirmQuestion = SvcDetail::confirmQuestion(request);
    m_Confirm = std::move(request);
    m_ShowConfirm = true;
}

void ServiceActionsView::run(ServiceActionRequest request)
{
    m_Result = {};
    m_Running = request;
    try
    {
        // The worker owns copies of everything it uses; nothing here is shared with it but the future.
        m_Worker = std::async(std::launch::async,
                              [actions = m_Actions, request = std::move(request)]
                              {
                                  static_cast<void>(Platform::setCurrentThreadName(Platform::SERVICE_ACTION_THREAD_NAME));
                                  return SvcDetail::dispatch(*actions, request);
                              });
    }
    catch (const std::system_error& e)
    {
        m_Result = SvcDetail::resultMessage(m_Running, Platform::ServiceActionResult::failed(e.what()));
        m_ResultSecondsLeft = SvcDetail::RESULT_SECONDS;
    }
}

bool ServiceActionsView::takeFinished()
{
    if (!m_Worker.valid() || m_Worker.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
    {
        return false;
    }
    Platform::ServiceActionResult result;
    try
    {
        result = m_Worker.get();
    }
    catch (const std::exception& e)
    {
        result = Platform::ServiceActionResult::failed(e.what());
    }
    m_Result = SvcDetail::resultMessage(m_Running, result);
    m_ResultSecondsLeft = SvcDetail::RESULT_SECONDS;
    return true;
}

void ServiceActionsView::tick(float deltaSeconds) noexcept
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

void ServiceActionsView::renderStartTypeItems(const Platform::ServiceInfo& service)
{
    using enum Platform::ServiceStartType;
    // The labels serviceStartTypeLabel() gives, as C strings for ImGui.
    constexpr std::array<std::pair<Platform::ServiceStartType, const char*>, 4> CHOICES{{
        {Automatic, "Automatic"},
        {AutomaticDelayed, "Automatic (delayed)"},
        {Manual, "Manual"},
        {Disabled, "Disabled"},
    }};
    for (const auto& [type, label] : CHOICES)
    {
        const bool current = service.startType == type;
        if (ImGui::MenuItem(label, nullptr, current, !current && !busy()))
        {
            request(SvcDetail::makeRequest(ServiceActionKind::SetStartType, service, type));
        }
    }
}

void ServiceActionsView::renderActionBar(const Platform::ServiceInfo* selected)
{
    if (!supported())
    {
        return;
    }
    const Platform::ServiceState state = (selected != nullptr) ? selected->state : Platform::ServiceState::Unknown;
    const auto button = [&](ServiceActionKind kind, const char* label, const char* tooltip)
    {
        const bool enabled = selected != nullptr && !busy() && SvcDetail::isApplicable(kind, state, m_Capabilities);
        ImGui::BeginDisabled(!enabled);
        if (ImGui::Button(label) && enabled)
        {
            request(SvcDetail::makeRequest(kind, *selected));
        }
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("%s", tooltip);
        ImGui::SameLine();
    };
    button(ServiceActionKind::Start, ICON_FA_PLAY " Start", "Start the selected service (it must be stopped)");
    button(ServiceActionKind::Stop, ICON_FA_STOP " Stop", "Stop the selected service (it must be running)");
    button(ServiceActionKind::Restart, ICON_FA_ARROWS_ROTATE " Restart", "Stop and start the selected service (it must be running)");

    const bool typeEnabled = selected != nullptr && !busy() && m_Capabilities.canSetStartType;
    ImGui::BeginDisabled(!typeEnabled);
    if (ImGui::Button("Startup type " ICON_FA_CARET_DOWN))
    {
        ImGui::OpenPopup("##ServiceStartTypeMenu");
    }
    ImGui::EndDisabled();
    if (selected != nullptr && ImGui::BeginPopup("##ServiceStartTypeMenu"))
    {
        renderStartTypeItems(*selected);
        ImGui::EndPopup();
    }

    const auto& scheme = UI::Theme::get().scheme();
    ImGui::SameLine();
    if (!m_Capabilities.elevated)
    {
        // Kept enabled: a service's own permissions may still allow the change.
        ImGui::TextColored(scheme.textMuted, ICON_FA_CIRCLE_INFO "  Requires administrator for most services");
        ImGui::SetItemTooltip("TaskSmack isn't running as administrator. Windows may still allow some services, by their own "
                              "permissions; the others report \"Requires administrator\".");
    }
    else if (selected == nullptr)
    {
        ImGui::TextColored(scheme.textMuted, "Select a service");
    }
    else
    {
        ImGui::TextColored(scheme.textMuted, "%s", selected->name.c_str());
    }
}

void ServiceActionsView::renderContextMenu(const Platform::ServiceInfo& service)
{
    if (!supported() || !ImGui::BeginPopupContextItem("##ServiceRowMenu"))
    {
        return;
    }
    ImGui::TextDisabled("%s", service.name.c_str());
    ImGui::Separator();
    const auto item = [&](ServiceActionKind kind, const char* label)
    {
        if (ImGui::MenuItem(label, nullptr, false, !busy() && SvcDetail::isApplicable(kind, service.state, m_Capabilities)))
        {
            request(SvcDetail::makeRequest(kind, service));
        }
    };
    item(ServiceActionKind::Start, ICON_FA_PLAY "  Start");
    item(ServiceActionKind::Stop, ICON_FA_STOP "  Stop");
    item(ServiceActionKind::Restart, ICON_FA_ARROWS_ROTATE "  Restart");
    if (ImGui::BeginMenu("Startup type", !busy() && m_Capabilities.canSetStartType))
    {
        renderStartTypeItems(service);
        ImGui::EndMenu();
    }
    if (!m_Capabilities.elevated)
    {
        ImGui::Separator();
        ImGui::TextDisabled("Requires administrator for most services");
    }
    ImGui::EndPopup();
}

void ServiceActionsView::renderResultLine() const
{
    const auto& scheme = UI::Theme::get().scheme();
    if (busy())
    {
        ImGui::TextColored(scheme.textMuted, "%s", SvcDetail::progressText(m_Running).c_str());
    }
    else if (!m_Result.text.empty())
    {
        // The colour comes from the result's flag, never from its text.
        ImGui::TextColored(m_Result.ok ? scheme.textSuccess : scheme.textError, "%s", m_Result.text.c_str());
    }
}

void ServiceActionsView::renderConfirmation()
{
    if (!m_Confirm)
    {
        return;
    }
    switch (
        ProcessActionConfirm::renderLabelled(m_ShowConfirm, SvcDetail::confirmLabel(*m_Confirm), true, m_ConfirmTitle, m_ConfirmQuestion))
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
