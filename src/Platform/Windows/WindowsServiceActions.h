#pragma once

// Service control through the Service Control Manager (#1577). The SCM calls are a table of function
// pointers, advapi32's by default, so tests drive every action and failure path with fakes
// (test_WindowsServiceActions.cpp) and never touch a real service.

#ifdef _WIN32
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

#include "Platform/IServiceActions.h"

#include <string_view>

namespace Platform
{

namespace Windows
{

/// The calls WindowsServiceActions makes: advapi32's (and kernel32's Sleep), or a test's fakes.
struct ServiceControlFunctions
{
    using OpenSCManagerFn = SC_HANDLE(WINAPI*)(LPCWSTR, LPCWSTR, DWORD);
    using OpenServiceFn = SC_HANDLE(WINAPI*)(SC_HANDLE, LPCWSTR, DWORD);
    using CloseServiceHandleFn = BOOL(WINAPI*)(SC_HANDLE);
    using StartServiceFn = BOOL(WINAPI*)(SC_HANDLE, DWORD, LPCWSTR*);
    using ControlServiceFn = BOOL(WINAPI*)(SC_HANDLE, DWORD, LPSERVICE_STATUS);
    using QueryServiceStatusFn = BOOL(WINAPI*)(SC_HANDLE, LPSERVICE_STATUS);
    using EnumDependentServicesFn = BOOL(WINAPI*)(SC_HANDLE, DWORD, LPENUM_SERVICE_STATUSW, DWORD, LPDWORD, LPDWORD);
    using ChangeServiceConfigFn =
        BOOL(WINAPI*)(SC_HANDLE, DWORD, DWORD, DWORD, LPCWSTR, LPCWSTR, LPDWORD, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR);
    using ChangeServiceConfig2Fn = BOOL(WINAPI*)(SC_HANDLE, DWORD, LPVOID);
    using SleepFn = void(WINAPI*)(DWORD);

    OpenSCManagerFn openSCManager = &::OpenSCManagerW;
    OpenServiceFn openService = &::OpenServiceW;
    CloseServiceHandleFn closeServiceHandle = &::CloseServiceHandle;
    StartServiceFn startService = &::StartServiceW;
    ControlServiceFn controlService = &::ControlService;
    QueryServiceStatusFn queryServiceStatus = &::QueryServiceStatus;
    EnumDependentServicesFn enumDependentServices = &::EnumDependentServicesW;
    ChangeServiceConfigFn changeServiceConfig = &::ChangeServiceConfigW;
    ChangeServiceConfig2Fn changeServiceConfig2 = &::ChangeServiceConfig2W;
    SleepFn sleep = &::Sleep;
};

} // namespace Windows

/// Starts, stops, restarts and reconfigures Windows services. Each action opens the manager with
/// SC_MANAGER_CONNECT and the service with only the rights it needs, then waits (polling, bounded by
/// ServiceActionMath::WAIT_TIMEOUT_MS) for the new state. Stateless beyond its fixed function table,
/// so safe to call from any thread.
class WindowsServiceActions final : public IServiceActions
{
  public:
    /// @param elevated Whether TaskSmack runs elevated, for capabilities().elevated.
    explicit WindowsServiceActions(bool elevated, Windows::ServiceControlFunctions api = {});

    [[nodiscard]] ServiceActionCapabilities capabilities() const override;
    [[nodiscard]] ServiceActionResult start(std::string_view name) override;
    [[nodiscard]] ServiceActionResult stop(std::string_view name) override;
    [[nodiscard]] ServiceActionResult restart(std::string_view name) override;
    [[nodiscard]] ServiceActionResult setStartType(std::string_view name, ServiceStartType startType) override;

    /// Whether the current process token is elevated (TokenElevation); false if it can't be read.
    [[nodiscard]] static bool isCurrentProcessElevated();

  private:
    bool m_Elevated;
    Windows::ServiceControlFunctions m_Api;
};

} // namespace Platform

#endif // _WIN32
