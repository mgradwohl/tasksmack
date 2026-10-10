#include "WindowsServiceActions.h"

#include "Platform/IServiceActions.h"
#include "Platform/IServiceProbe.h"

#include <spdlog/spdlog.h>

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
#include "WindowsServiceActionsMath.h"

#include <cstdint>
#include <format>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
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

[[nodiscard]] OpenedService openService(const Windows::ServiceControlFunctions& api, std::string_view name, DWORD access)
{
    const Windows::InjectedServiceHandleCloser closer{.close = api.closeServiceHandle};
    OpenedService opened{.scm = Windows::InjectableServiceHandle(api.openSCManager(nullptr, nullptr, SC_MANAGER_CONNECT), closer),
                         .service = Windows::InjectableServiceHandle(nullptr, closer),
                         .error = ERROR_SUCCESS};
    if (!opened.scm)
    {
        opened.error = GetLastError();
        return opened;
    }
    const std::wstring wideName = WinString::utf8ToWide(name);
    opened.service.reset(api.openService(opened.scm.get(), wideName.c_str(), access));
    if (!opened.service)
    {
        opened.error = GetLastError();
    }
    return opened;
}

[[nodiscard]] ServiceActionResult lastError()
{
    return ServiceActionResult::failed(Math::errorText(GetLastError()));
}

/// Polls until the service reaches @p target, for at most Math::WAIT_TIMEOUT_MS. A service that falls
/// back to stopped while starting has failed to start, and says so at once rather than timing out. A
/// stop request on @p stopToken ends the wait at the next poll (#1591): the state is read once more,
/// so a service that got there still reports success; otherwise the wait is abandoned.
[[nodiscard]] ServiceActionResult waitForState(
    const Windows::ServiceControlFunctions& api, SC_HANDLE service, DWORD target, std::string_view verb, const std::stop_token& stopToken)
{
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
        if (waited >= Math::WAIT_TIMEOUT_MS)
        {
            return ServiceActionResult::failed(std::format("It didn't {} within {} s", verb, Math::WAIT_TIMEOUT_MS / 1000));
        }
        api.sleep(Math::WAIT_POLL_MS);
    }
}

/// The display names of @p name's running dependents, for a stop refused because of them. Empty when
/// they can't be listed: the caller then gives the plain "other services depend on it".
[[nodiscard]] std::vector<std::string> runningDependents(const Windows::ServiceControlFunctions& api, std::string_view name)
{
    const OpenedService opened = openService(api, name, SERVICE_ENUMERATE_DEPENDENTS);
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

} // namespace

WindowsServiceActions::WindowsServiceActions(bool elevated, Windows::ServiceControlFunctions api) : m_Elevated(elevated), m_Api(api)
{}

ServiceActionCapabilities WindowsServiceActions::capabilities() const
{
    return {.canStart = true, .canStop = true, .canRestart = true, .canSetStartType = true, .elevated = m_Elevated};
}

ServiceActionResult WindowsServiceActions::start(std::string_view name, const std::stop_token& stopToken)
{
    if (stopToken.stop_requested())
    {
        return ServiceActionResult::stopRequested(); // nothing sent yet, and nothing is
    }
    const OpenedService opened = openService(m_Api, name, SERVICE_START | SERVICE_QUERY_STATUS);
    if (opened.error != ERROR_SUCCESS)
    {
        return ServiceActionResult::failed(Math::errorText(opened.error));
    }
    if (m_Api.startService(opened.service.get(), 0, nullptr) == FALSE)
    {
        return lastError();
    }
    return waitForState(m_Api, opened.service.get(), SERVICE_RUNNING, "start", stopToken);
}

ServiceActionResult WindowsServiceActions::stop(std::string_view name, const std::stop_token& stopToken)
{
    if (stopToken.stop_requested())
    {
        return ServiceActionResult::stopRequested(); // nothing sent yet, and nothing is
    }
    const OpenedService opened = openService(m_Api, name, SERVICE_STOP | SERVICE_QUERY_STATUS);
    if (opened.error != ERROR_SUCCESS)
    {
        return ServiceActionResult::failed(Math::errorText(opened.error));
    }
    SERVICE_STATUS status{};
    if (m_Api.controlService(opened.service.get(), SERVICE_CONTROL_STOP, &status) == FALSE)
    {
        const DWORD error = GetLastError();
        if (error == ERROR_DEPENDENT_SERVICES_RUNNING)
        {
            // Never stopped along with it: the user decides, knowing which they are.
            return ServiceActionResult::failed(Math::dependentsText(runningDependents(m_Api, name)));
        }
        return ServiceActionResult::failed(Math::errorText(error));
    }
    return waitForState(m_Api, opened.service.get(), SERVICE_STOPPED, "stop", stopToken);
}

ServiceActionResult WindowsServiceActions::restart(std::string_view name, const std::stop_token& stopToken)
{
    if (ServiceActionResult stopped = stop(name, stopToken); !stopped.ok)
    {
        return stopped;
    }
    // A stop requested once the service has stopped leaves it stopped: start() sends nothing then.
    return start(name, stopToken);
}

ServiceActionResult WindowsServiceActions::setStartType(std::string_view name, ServiceStartType startType)
{
    const auto code = Math::startTypeCode(startType);
    if (!code)
    {
        return ServiceActionResult::failed("That startup type can't be set here");
    }
    const OpenedService opened = openService(m_Api, name, SERVICE_CHANGE_CONFIG);
    if (opened.error != ERROR_SUCCESS)
    {
        return ServiceActionResult::failed(Math::errorText(opened.error));
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

bool WindowsServiceActions::isCurrentProcessElevated()
{
    Windows::UniqueHandle token;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, token.put()) == FALSE)
    {
        spdlog::debug("WindowsServiceActions: OpenProcessToken failed (error {})", GetLastError());
        return false;
    }
    TOKEN_ELEVATION elevation{};
    DWORD size = sizeof(elevation);
    if (GetTokenInformation(token.get(), TokenElevation, &elevation, sizeof(elevation), &size) == FALSE)
    {
        return false;
    }
    return elevation.TokenIsElevated != 0;
}

} // namespace Platform
