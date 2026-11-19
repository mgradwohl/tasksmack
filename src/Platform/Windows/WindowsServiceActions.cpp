#include "WindowsServiceActions.h"

#include "Platform/IServiceActions.h"
#include "Platform/IServiceProbe.h"
#include "Platform/NativeId.h"

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winsvc.h>
// clang-format on

#pragma comment(lib, "advapi32.lib")

#include "WinString.h"
#include "WindowsHandles.h"
#include "WindowsNativeIds.h"
#include "WindowsServiceActionsMath.h"

#include <cstdint>
#include <expected>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Platform
{

namespace
{

namespace Math = Windows::ServiceActionMath;

/// The manager and one service opened with the rights an action needs, or the error that stopped it.
/// The service is declared second, so it is closed before the manager.
struct OpenedService
{
    Windows::InjectableServiceHandle scm;
    Windows::InjectableServiceHandle service;
    DWORD error = ERROR_SUCCESS;
};

[[nodiscard]] OpenedService openService(const Windows::ServiceControlFunctions& api, const std::wstring& wideName, DWORD access)
{
    const Windows::InjectedServiceHandleCloser closer{.close = api.closeServiceHandle};
    OpenedService opened{
        .scm = Windows::InjectableServiceHandle(api.openSCManager(nullptr, nullptr, SC_MANAGER_CONNECT), closer),
        .service = Windows::InjectableServiceHandle(nullptr, closer),
        .error = ERROR_SUCCESS,
    };
    if (!opened.scm)
    {
        opened.error = GetLastError();
        return opened;
    }
    opened.service.reset(api.openService(opened.scm.get(), wideName.c_str(), access));
    if (!opened.service)
    {
        opened.error = GetLastError();
    }
    return opened;
}

[[nodiscard]] ServiceActionResult lastError()
{
    return Math::errorResult(GetLastError());
}

/// Polls until the service reaches @p target, following the SCM's documented wait (#2127): while the
/// service raises dwCheckPoint it is making progress and the wait goes on, for up to its dwWaitHint
/// (at least Math::WAIT_STALL_MIN_MS) without progress and Math::WAIT_TOTAL_MAX_MS in all; see
/// Math::PendingWait. A service that falls back to stopped while starting has failed to start, and
/// says so at once rather than timing out. A stop request on @p stopToken ends the wait at the next
/// poll (#1591): the state is read once more, so a service that got there still reports success;
/// otherwise the wait is abandoned.
[[nodiscard]] ServiceActionResult waitForState(
    const Windows::ServiceControlFunctions& api, SC_HANDLE service, DWORD target, std::string_view verb, const std::stop_token& stopToken)
{
    Math::PendingWait wait;
    for (std::uint32_t waited = 0;; waited += Math::WAIT_POLL_MS)
    {
        SERVICE_STATUS status{};
        if (api.queryServiceStatus(service, &status) == FALSE)
        {
            return lastError();
        }
        if (status.dwCurrentState == target)
        {
            return ServiceActionResult::succeeded();
        }
        if (target == SERVICE_RUNNING && status.dwCurrentState == SERVICE_STOPPED)
        {
            return ServiceActionResult::failed(status.dwWin32ExitCode != NO_ERROR
                                                   ? std::format("It stopped while starting: {}", Math::errorText(status.dwWin32ExitCode))
                                                   : std::string("It stopped while starting"));
        }
        if (stopToken.stop_requested())
        {
            return ServiceActionResult::stopRequested();
        }
        switch (wait.observe(waited, status.dwCurrentState, status.dwCheckPoint, status.dwWaitHint))
        {
        case Math::WaitVerdict::Stalled:
            return ServiceActionResult::failed(
                std::format("It didn't {}: no progress in {} s", verb, Math::stallWindowMs(status.dwWaitHint) / 1000));
        case Math::WaitVerdict::TooLong:
            return ServiceActionResult::failed(std::format("It didn't {} within {} s", verb, Math::WAIT_TOTAL_MAX_MS / 1000));
        case Math::WaitVerdict::KeepWaiting:
            break;
        }
        api.sleep(Math::WAIT_POLL_MS);
    }
}

/// The display names of @p wideName's running dependents, for a stop refused because of them. Empty when
/// they can't be listed: the caller then gives the plain "other services depend on it".
[[nodiscard]] std::vector<std::string> runningDependents(const Windows::ServiceControlFunctions& api, const std::wstring& wideName)
{
    const OpenedService opened = openService(api, wideName, SERVICE_ENUMERATE_DEPENDENTS);
    if (!opened.service)
    {
        return {};
    }
    DWORD needed = 0;
    DWORD count = 0;
    if (api.enumDependentServices(opened.service.get(), SERVICE_ACTIVE, nullptr, 0, &needed, &count) != FALSE || needed == 0)
    {
        return {};
    }
    std::vector<ENUM_SERVICE_STATUSW> buffer((needed + sizeof(ENUM_SERVICE_STATUSW) - 1) / sizeof(ENUM_SERVICE_STATUSW));
    if (api.enumDependentServices(opened.service.get(),
                                  SERVICE_ACTIVE,
                                  buffer.data(),
                                  static_cast<DWORD>(buffer.size() * sizeof(ENUM_SERVICE_STATUSW)),
                                  &needed,
                                  &count) == FALSE)
    {
        return {};
    }
    std::vector<std::string> names;
    for (const ENUM_SERVICE_STATUSW& entry : std::span(buffer.data(), count))
    {
        const wchar_t* shown = (entry.lpDisplayName != nullptr) ? entry.lpDisplayName : entry.lpServiceName;
        names.push_back(shown != nullptr ? WinString::wideToUtf8(shown) : std::string{});
    }
    return names;
}

/// The exact name @p service names (#4304): the one its id was issued for, while WindowsServiceProbe still lists it;
/// without an id, its name, when that can be exact. A refusal is the result to give: never another service's name.
[[nodiscard]] std::expected<std::wstring, ServiceActionResult> resolveName(const Windows::ServiceIdTable* ids, const ServiceTarget& service)
{
    if (service.nativeId != NativeId::None)
    {
        std::optional<std::wstring> listed = (ids != nullptr) ? ids->resolve(service.nativeId) : std::nullopt;
        if (!listed)
        {
            return std::unexpected(
                ServiceActionResult::failed("The service is no longer listed as it was. Try again once the list updates"));
        }
        if (Windows::hasEmbeddedNul(*listed))
        {
            return std::unexpected(ServiceActionResult::failed("The service's name holds a NUL character, so it can't be opened by name"));
        }
        return std::move(*listed);
    }
    if (std::optional<std::wstring> exact = Windows::exactWideName(service.name))
    {
        return std::move(*exact);
    }
    return std::unexpected(ServiceActionResult::failed("The service's name couldn't be read exactly, so it isn't acted on"));
}

/// Starts @p wideName and waits until it runs, or until @p stopToken is stopped.
[[nodiscard]] ServiceActionResult
startService(const Windows::ServiceControlFunctions& api, const std::wstring& wideName, const std::stop_token& stopToken)
{
    if (stopToken.stop_requested())
    {
        return ServiceActionResult::stopRequested(); // nothing sent yet, and nothing is
    }
    const OpenedService opened = openService(api, wideName, SERVICE_START | SERVICE_QUERY_STATUS);
    if (opened.error != ERROR_SUCCESS)
    {
        return Math::errorResult(opened.error);
    }
    if (api.startService(opened.service.get(), 0, nullptr) == FALSE)
    {
        return lastError();
    }
    return waitForState(api, opened.service.get(), SERVICE_RUNNING, "start", stopToken);
}

/// Asks @p wideName to stop and waits until it has, or until @p stopToken is stopped. Running dependents fail it.
[[nodiscard]] ServiceActionResult
stopService(const Windows::ServiceControlFunctions& api, const std::wstring& wideName, const std::stop_token& stopToken)
{
    if (stopToken.stop_requested())
    {
        return ServiceActionResult::stopRequested(); // nothing sent yet, and nothing is
    }
    const OpenedService opened = openService(api, wideName, SERVICE_STOP | SERVICE_QUERY_STATUS);
    if (opened.error != ERROR_SUCCESS)
    {
        return Math::errorResult(opened.error);
    }
    SERVICE_STATUS status{};
    if (api.controlService(opened.service.get(), SERVICE_CONTROL_STOP, &status) == FALSE)
    {
        const DWORD error = GetLastError();
        if (error == ERROR_DEPENDENT_SERVICES_RUNNING)
        {
            // Never stopped along with it: the user decides, knowing which they are.
            return ServiceActionResult::failed(Math::dependentsText(runningDependents(api, wideName)));
        }
        return Math::errorResult(error);
    }
    return waitForState(api, opened.service.get(), SERVICE_STOPPED, "stop", stopToken);
}

} // namespace

WindowsServiceActions::WindowsServiceActions(bool elevated,
                                             Windows::ServiceControlFunctions api,
                                             std::shared_ptr<const Windows::ServiceIdTable> ids)
    : m_Elevated(elevated), m_Api(api), m_Ids(std::move(ids))
{}

ServiceActionCapabilities WindowsServiceActions::capabilities() const
{
    return {.canStart = true, .canStop = true, .canRestart = true, .canSetStartType = true, .elevated = m_Elevated};
}

ServiceActionResult WindowsServiceActions::start(const ServiceTarget& service, const std::stop_token& stopToken)
{
    const std::expected<std::wstring, ServiceActionResult> name = resolveName(m_Ids.get(), service);
    return name ? startService(m_Api, *name, stopToken) : name.error();
}

ServiceActionResult WindowsServiceActions::stop(const ServiceTarget& service, const std::stop_token& stopToken)
{
    const std::expected<std::wstring, ServiceActionResult> name = resolveName(m_Ids.get(), service);
    return name ? stopService(m_Api, *name, stopToken) : name.error();
}

ServiceActionResult WindowsServiceActions::restart(const ServiceTarget& service, const std::stop_token& stopToken)
{
    // Resolved once, so the start goes to the very service the stop did.
    const std::expected<std::wstring, ServiceActionResult> name = resolveName(m_Ids.get(), service);
    if (!name)
    {
        return name.error();
    }
    // stopService() returns only once the service is STOPPED (or the wait failed): start is never sent to a
    // service still stopping, and a slow but advancing stop is waited out rather than ending the
    // restart with the service left stopped (#2127).
    if (ServiceActionResult stopped = stopService(m_Api, *name, stopToken); !stopped.ok)
    {
        return stopped;
    }
    // A stop requested once the service has stopped leaves it stopped: startService() sends nothing then.
    return startService(m_Api, *name, stopToken);
}

ServiceActionResult WindowsServiceActions::setStartType(const ServiceTarget& service, ServiceStartType startType)
{
    const auto code = Math::startTypeCode(startType);
    if (!code)
    {
        return ServiceActionResult::failed("That startup type can't be set here");
    }
    const std::expected<std::wstring, ServiceActionResult> name = resolveName(m_Ids.get(), service);
    if (!name)
    {
        return name.error();
    }
    const OpenedService opened = openService(m_Api, *name, SERVICE_CHANGE_CONFIG);
    if (opened.error != ERROR_SUCCESS)
    {
        return Math::errorResult(opened.error);
    }
    if (m_Api.changeServiceConfig(opened.service.get(),
                                  SERVICE_NO_CHANGE,
                                  code->code,
                                  SERVICE_NO_CHANGE,
                                  nullptr,
                                  nullptr,
                                  nullptr,
                                  nullptr,
                                  nullptr,
                                  nullptr,
                                  nullptr) == FALSE)
    {
        return lastError();
    }
    // Delayed auto-start means something only for an automatic service; set it either way for one.
    if (code->code == SERVICE_AUTO_START)
    {
        SERVICE_DELAYED_AUTO_START_INFO delayed{.fDelayedAutostart = code->delayed ? TRUE : FALSE};
        if (m_Api.changeServiceConfig2(opened.service.get(), SERVICE_CONFIG_DELAYED_AUTO_START_INFO, &delayed) == FALSE)
        {
            return lastError();
        }
    }
    return ServiceActionResult::succeeded();
}

} // namespace Platform
