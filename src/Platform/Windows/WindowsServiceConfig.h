#pragma once

// One service's configuration read (#800): start type (with delayed auto-start), command line,
// account, description and svchost group. The SCM calls it makes are a table of function pointers,
// advapi32's by default, so tests can feed it fixed answers and denials (test_WindowsServiceConfig.cpp).

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

#include "Platform/IServiceProbe.h"

#include <string>

namespace Platform::Windows
{

/// The Service Control Manager calls readServiceConfig() makes, and the probe's open and list calls:
/// advapi32's, or a test's fakes.
struct ServiceConfigFunctions
{
    using OpenServiceFn = SC_HANDLE(WINAPI*)(SC_HANDLE, LPCWSTR, DWORD);
    using QueryServiceConfigFn = BOOL(WINAPI*)(SC_HANDLE, LPQUERY_SERVICE_CONFIGW, DWORD, LPDWORD);
    using QueryServiceConfig2Fn = BOOL(WINAPI*)(SC_HANDLE, DWORD, LPBYTE, DWORD, LPDWORD);
    using CloseServiceHandleFn = BOOL(WINAPI*)(SC_HANDLE);
    using OpenSCManagerFn = SC_HANDLE(WINAPI*)(LPCWSTR, LPCWSTR, DWORD);
    using EnumServicesStatusExFn = BOOL(WINAPI*)(SC_HANDLE, SC_ENUM_TYPE, DWORD, DWORD, LPBYTE, DWORD, LPDWORD, LPDWORD, LPDWORD, LPCWSTR);

    OpenServiceFn openService = &::OpenServiceW;
    QueryServiceConfigFn queryServiceConfig = &::QueryServiceConfigW;
    QueryServiceConfig2Fn queryServiceConfig2 = &::QueryServiceConfig2W;
    CloseServiceHandleFn closeServiceHandle = &::CloseServiceHandle;
    OpenSCManagerFn openSCManager = &::OpenSCManagerW;
    EnumServicesStatusExFn enumServicesStatusEx = &::EnumServicesStatusExW;
};

/// A service's configuration fields. Those that could not be read are empty or Unknown.
struct ServiceConfig
{
    ServiceStartType startType = ServiceStartType::Unknown;
    std::string binaryPath;
    std::string account;
    std::string description;
    std::string group;
};

/// Reads `serviceName`'s configuration through `scm`, which must have been opened with
/// SC_MANAGER_CONNECT (OpenServiceW needs it). A denied open or query leaves its fields empty.
[[nodiscard]] ServiceConfig readServiceConfig(const ServiceConfigFunctions& api, SC_HANDLE scm, const wchar_t* serviceName);

} // namespace Platform::Windows

#endif // _WIN32
