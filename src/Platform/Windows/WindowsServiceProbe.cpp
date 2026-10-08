#include "WindowsServiceProbe.h"

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
#include "WindowsServiceProbeMath.h"

#include <chrono>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Platform
{

namespace
{

/// Calls a "size query, then fill" Win32 API into a buffer of T, so the result is aligned for T.
/// `query(buffer, bytes, &needed)` returns the API's BOOL. Empty when the call fails.
template<typename T, typename Query> [[nodiscard]] std::vector<T> queryAligned(Query query)
{
    DWORD needed = 0;
    static_cast<void>(query(nullptr, 0, &needed));
    if (needed == 0)
    {
        return {};
    }
    std::vector<T> buffer((needed + sizeof(T) - 1) / sizeof(T));
    if (query(buffer.data(), static_cast<DWORD>(buffer.size() * sizeof(T)), &needed) == FALSE)
    {
        return {};
    }
    return buffer;
}

[[nodiscard]] std::string toUtf8(const wchar_t* text)
{
    return text != nullptr ? WinString::wideToUtf8(text) : std::string{};
}

/// The configuration fields of one service, cached between enumerations.
struct CachedConfig
{
    std::chrono::steady_clock::time_point readAt; ///< When the last read was attempted, whatever its outcome.
    bool attempted = false;                       ///< False until the first read: a new or reappeared service.
    ServiceStartType startType = ServiceStartType::Unknown;
    std::string binaryPath;
    std::string account;
    std::string description;
    std::string group;
};

} // namespace

struct WindowsServiceProbe::Impl
{
    Windows::UniqueServiceHandle scm;
    DWORD scmOpenError = ERROR_SUCCESS; ///< OpenSCManagerW's error at construction; nonzero disables the probe.
    std::unordered_map<std::string, CachedConfig> configs;

    /// Read one service's configuration. Fields stay empty where the open or a query is denied.
    [[nodiscard]] CachedConfig readConfig(const wchar_t* serviceName) const
    {
        CachedConfig config;
        config.attempted = true;
        config.readAt = std::chrono::steady_clock::now();
        const Windows::UniqueServiceHandle service(OpenServiceW(scm.get(), serviceName, SERVICE_QUERY_CONFIG));
        if (!service)
        {
            return config;
        }

        const auto qsc = queryAligned<QUERY_SERVICE_CONFIGW>([&service](QUERY_SERVICE_CONFIGW* buffer, DWORD bytes, DWORD* needed)
                                                             { return QueryServiceConfigW(service.get(), buffer, bytes, needed); });
        if (!qsc.empty())
        {
            const auto& cfg = qsc.front();
            bool delayed = false;
            SERVICE_DELAYED_AUTO_START_INFO delayedInfo{};
            DWORD needed = 0;
            if (QueryServiceConfig2W(service.get(),
                                     SERVICE_CONFIG_DELAYED_AUTO_START_INFO,
                                     reinterpret_cast<LPBYTE>(&delayedInfo),
                                     sizeof(delayedInfo),
                                     &needed) != FALSE)
            {
                delayed = delayedInfo.fDelayedAutostart != FALSE;
            }
            config.startType = Windows::ServiceMath::startTypeFromCode(cfg.dwStartType, delayed);
            config.binaryPath = toUtf8(cfg.lpBinaryPathName);
            config.account = toUtf8(cfg.lpServiceStartName);
            config.group = Windows::ServiceMath::svchostGroup(config.binaryPath);
        }

        const auto desc = queryAligned<SERVICE_DESCRIPTIONW>(
            [&service](SERVICE_DESCRIPTIONW* buffer, DWORD bytes, DWORD* needed)
            { return QueryServiceConfig2W(service.get(), SERVICE_CONFIG_DESCRIPTION, reinterpret_cast<LPBYTE>(buffer), bytes, needed); });
        if (!desc.empty())
        {
            config.description = toUtf8(desc.front().lpDescription);
        }
        return config;
    }
};

WindowsServiceProbe::WindowsServiceProbe() : m_Impl(std::make_unique<Impl>())
{
    m_Impl->scm.reset(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ENUMERATE_SERVICE));
    if (!m_Impl->scm)
    {
        const DWORD error = GetLastError();
        m_Impl->scmOpenError = (error != ERROR_SUCCESS) ? error : ERROR_GEN_FAILURE;
        spdlog::warn("WindowsServiceProbe: OpenSCManagerW failed (error {})", m_Impl->scmOpenError);
    }
}

WindowsServiceProbe::~WindowsServiceProbe() = default;

ServiceCapabilities WindowsServiceProbe::capabilities() const
{
    return Windows::ServiceMath::capabilitiesForScmOpen(m_Impl->scmOpenError);
}

ServiceEnumeration WindowsServiceProbe::enumerate()
{
    if (m_Impl->scmOpenError != ERROR_SUCCESS)
    {
        return {.failureReason = capabilities().unavailableReason};
    }
    if (!m_Impl->scm)
    {
        m_Impl->scm.reset(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ENUMERATE_SERVICE));
        if (!m_Impl->scm)
        {
            return {.failureReason = Windows::ServiceMath::scmFailureReason(GetLastError(), "be opened")};
        }
    }

    std::vector<ServiceInfo> services;
    std::unordered_set<std::string> seen;
    const auto now = std::chrono::steady_clock::now();
    DWORD resume = 0;
    std::vector<ENUM_SERVICE_STATUS_PROCESSW> buffer;
    for (;;)
    {
        DWORD needed = 0;
        DWORD count = 0;
        const BOOL ok = EnumServicesStatusExW(m_Impl->scm.get(),
                                              SC_ENUM_PROCESS_INFO,
                                              SERVICE_WIN32,
                                              SERVICE_STATE_ALL,
                                              reinterpret_cast<LPBYTE>(buffer.data()),
                                              static_cast<DWORD>(buffer.size() * sizeof(ENUM_SERVICE_STATUS_PROCESSW)),
                                              &needed,
                                              &count,
                                              &resume,
                                              nullptr);
        const DWORD error = (ok != FALSE) ? ERROR_SUCCESS : GetLastError();
        if (error != ERROR_SUCCESS && error != ERROR_MORE_DATA)
        {
            spdlog::debug("WindowsServiceProbe: EnumServicesStatusExW failed (error {})", error);
            // A failed handle (e.g. the SCM restarted) is reopened on the next call.
            m_Impl->scm.reset();
            return {.failureReason = Windows::ServiceMath::scmFailureReason(error, "list the services")};
        }

        for (const auto& entry : std::span(buffer.data(), count))
        {
            ServiceInfo info;
            info.name = toUtf8(entry.lpServiceName);
            info.displayName = toUtf8(entry.lpDisplayName);
            info.state = Windows::ServiceMath::stateFromCode(entry.ServiceStatusProcess.dwCurrentState);
            info.serviceType = Windows::ServiceMath::serviceTypeText(entry.ServiceStatusProcess.dwServiceType);
            info.pid = entry.ServiceStatusProcess.dwProcessId;

            auto& config = m_Impl->configs[info.name];
            // A denied or failed read leaves the fields empty until the next attempt, CONFIG_REFRESH later.
            if (Windows::ServiceMath::shouldRefreshConfig(config.readAt, now, config.attempted))
            {
                config = m_Impl->readConfig(entry.lpServiceName);
            }
            info.startType = config.startType;
            info.binaryPath = config.binaryPath;
            info.account = config.account;
            info.description = config.description;
            info.group = config.group;
            seen.insert(info.name);
            services.push_back(std::move(info));
        }

        if (error == ERROR_SUCCESS)
        {
            break;
        }
        // More to come from `resume`: grow the buffer to what the call asked for (it is at most
        // 256 KiB per call) and continue.
        buffer.resize((needed + sizeof(ENUM_SERVICE_STATUS_PROCESSW) - 1) / sizeof(ENUM_SERVICE_STATUS_PROCESSW));
    }

    // Services that were deleted no longer need their configuration.
    std::erase_if(m_Impl->configs, [&seen](const auto& entry) { return !seen.contains(entry.first); });
    return {.ok = true, .failureReason = {}, .services = std::move(services)};
}

} // namespace Platform
