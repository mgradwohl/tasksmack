#include "ServiceActionsView.h"

#include "App/Panels/ProcessActionConfirm.h"
#include "App/Panels/ServicesView.h"
#include "Platform/IServiceActions.h"
#include "Platform/IServiceProbe.h"
#include "UI/IconsFontAwesome6.h"
#include "UI/Theme.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <format>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
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

/// systemd units a Linux desktop or server can't run well without (#2118): the bus, logins and the
/// display manager, logging and devices, the network and remote access. Unit names are case-sensitive.
constexpr std::array<std::string_view, 16> CRITICAL_UNITS{
    "dbus.service",
    "dbus-broker.service",
    "display-manager.service",
    "gdm.service",
    "gdm3.service",
    "lightdm.service",
    "NetworkManager.service",
    "polkit.service",
    "sddm.service",
    "ssh.service",
    "sshd.service",
    "systemd-journald.service",
    "systemd-logind.service",
    "systemd-networkd.service",
    "systemd-resolved.service",
    "systemd-udevd.service",
};

/// Template units every instance of which is critical (#2471): a user's systemd manager (user@1000.service), which
/// runs all their user services and, on GNOME, the Shell, so stopping it ends their session; and their runtime
/// directory. Process Details' "Show in Services" leads to user@<uid>.service from any user service's process (#2463).
constexpr std::array<std::string_view, 2> CRITICAL_UNIT_TEMPLATES{
    "user@",
    "user-runtime-dir@",
};

/// What a critical service is critical to, in the confirm's warning.
#ifdef _WIN32
constexpr std::string_view SYSTEM_NAME = "Windows";
#else
constexpr std::string_view SYSTEM_NAME = "the system";
#endif

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

[[nodiscard]] Platform::ServiceActionResult
dispatch(Platform::IServiceActions& actions, const ServiceActionRequest& request, const std::stop_token& stopToken, bool authorized)
{
    if (authorized)
    {
        constexpr std::array<Platform::ServiceAction, 4> ACTIONS{Platform::ServiceAction::Start,
                                                                 Platform::ServiceAction::Stop,
                                                                 Platform::ServiceAction::Restart,
                                                                 Platform::ServiceAction::SetStartType};
        return actions.runAuthorized(ACTIONS.at(static_cast<std::size_t>(request.kind)), request.name, request.startType, stopToken);
    }
    switch (request.kind)
    {
    case ServiceActionKind::Start:
        return actions.start(request.name, stopToken);
    case ServiceActionKind::Stop:
        return actions.stop(request.name, stopToken);
    case ServiceActionKind::Restart:
        return actions.restart(request.name, stopToken);
    case ServiceActionKind::SetStartType:
        break;
    }
    return actions.setStartType(request.name, request.startType);
}

} // namespace

bool isCriticalService(std::string_view name, std::string_view serviceType)
{
    return serviceType == "Driver" ||
           std::ranges::any_of(CRITICAL_SERVICES, [name](std::string_view critical) { return equalsIgnoringCase(name, critical); }) ||
           std::ranges::find(CRITICAL_UNITS, name) != CRITICAL_UNITS.end() ||
           (name.ends_with(".service") &&
            std::ranges::any_of(CRITICAL_UNIT_TEMPLATES, [name](std::string_view prefix) { return name.starts_with(prefix); }));
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
    using enum Platform::ServiceStartType;
    if (request.kind == ServiceActionKind::Stop || request.kind == ServiceActionKind::Restart)
    {
        return true;
    }
    if (request.kind != ServiceActionKind::SetStartType)
    {
        return false;
    }
    // Manual stops a critical service starting at boot: after the next restart a headless machine may
    // have no network or SSH (#2118).
    const bool startedAtBoot = request.fromStartType == Automatic || request.fromStartType == AutomaticDelayed;
    return request.startType == Disabled || (request.startType == Manual && request.critical && startedAtBoot);
}

ServiceActionRequest makeRequest(ServiceActionKind kind, const Platform::ServiceInfo& service, Platform::ServiceStartType startType)
{
    return {.kind = kind,
            .name = service.name,
            .displayName = service.displayName.empty() ? service.name : service.displayName,
            .startType = startType,
            .fromStartType = service.startType,
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
    if (request.kind == ServiceActionKind::Start)
    {
        return "Start";
    }
    return request.startType == Platform::ServiceStartType::Manual ? "Set to Manual" : "Disable";
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
        question = std::format(ICON_FA_TRIANGLE_EXCLAMATION "  {} is critical to {}. Stopping or disabling it can make {} "
                                                            "unstable, cut network access or sign you out.\n\n",
                               request.name,
                               SYSTEM_NAME,
                               SYSTEM_NAME);
    }
    switch (request.kind)
    {
    case ServiceActionKind::Restart:
        question += std::format(
            "Restart {} ({})? It is stopped and started again; programs using it may be interrupted.", request.displayName, request.name);
        break;
    case ServiceActionKind::SetStartType:
        if (request.startType == Platform::ServiceStartType::Manual)
        {
            question += std::format("Set {} ({}) to Manual? It won't start at boot any more, only when something asks for it.",
                                    request.displayName,
                                    request.name);
            break;
        }
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

// A running action is cancelled, then waited for, by m_Worker's destructor: it holds its own
// shared_ptr to the actions, but its result must not outlive the view it reports to (and a detached
// worker could touch spdlog or other statics after exit destroyed them). Cancelled, it returns at the
// platform's next poll.
ServiceActionsView::~ServiceActionsView() = default;

void ServiceActionsView::cancel() noexcept
{
    m_Worker.requestStop();
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

void ServiceActionsView::run(ServiceActionRequest request, bool authorized)
{
    m_Result = {};
    m_Running = request;
    // The worker owns copies of everything it uses; it shares only its result and a new stop token's
    // state (a cancel() of an earlier action does not carry over) with this view. An action that throws
    // is a failed result, built on the worker (BackgroundTask, #1685).
    if (auto failed = m_Worker.start([actions = m_Actions, request = std::move(request), authorized](const std::stop_token& stopToken)
                                     { return SvcDetail::dispatch(*actions, request, stopToken, authorized); }))
    {
        m_Result = SvcDetail::resultMessage(m_Running, *failed);
        m_ResultSecondsLeft = SvcDetail::RESULT_SECONDS;
    }
}

bool ServiceActionsView::takeFinished()
{
    const std::optional<Platform::ServiceActionResult> result = m_Worker.poll();
    if (!result.has_value())
    {
        return false;
    }
    m_Result = SvcDetail::resultMessage(m_Running, *result);
    // A refusal a password prompt could allow offers the retry; one that found nothing to show a prompt
    // stops it being offered this session (#2304).
    m_AuthorizationUnavailable = m_AuthorizationUnavailable || result->authorizationUnavailable;
    m_Result.canAuthorize = result->needsAuthorization && m_Capabilities.canAuthorizeInteractively && !m_AuthorizationUnavailable;
    m_ResultSecondsLeft = m_Result.canAuthorize ? SvcDetail::AUTHORIZE_RESULT_SECONDS : SvcDetail::RESULT_SECONDS;
    return true;
}

void ServiceActionsView::retryAuthorized()
{
    if (busy() || !m_Result.canAuthorize)
    {
        return;
    }
    run(m_Running, true);
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
        if (type == AutomaticDelayed && !m_Capabilities.canDelayStart)
        {
            continue; // systemd has no delayed start (#2006)
        }
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
        ImGui::SetItemTooltip("%s",
                              m_Capabilities.canAuthorizeInteractively
                                  ? "TaskSmack isn't running as root. polkit may allow some services as they are; the others report "
                                    "\"Requires administrator\" and offer Authenticate and retry, which asks for your password."
                                  : "TaskSmack isn't running as administrator. Some services may still be allowed, by their own "
                                    "permissions on Windows or by polkit on Linux; the others report \"Requires administrator\".");
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

void ServiceActionsView::renderResultLine()
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
        if (m_Result.canAuthorize)
        {
            ImGui::SameLine();
            if (ImGui::SmallButton(ICON_FA_LOCK "  Authenticate and retry"))
            {
                retryAuthorized();
            }
            ImGui::SetItemTooltip("Shows your system's authentication prompt (polkit) and runs it again");
        }
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
