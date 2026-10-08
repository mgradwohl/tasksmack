#pragma once

#include "Platform/IServiceProbe.h"

#include <memory>
#include <vector>

namespace Platform
{

/// Windows services from the Service Control Manager (#800), read-only and without elevation.
///
/// The manager is opened once with SC_MANAGER_CONNECT | SC_MANAGER_ENUMERATE_SERVICE. Each
/// enumerate() lists the services with their state and PID through EnumServicesStatusExW
/// (SC_ENUM_PROCESS_INFO), paging with its resume handle: the first call only sizes the buffer, then
/// each call returns as many entries as fit (the SCM caps one call at 256 KiB, a few hundred
/// services), so a typical system takes two or three calls per poll, more with many services.
/// The configuration (start type, command line, account, description, delayed auto-start) needs an
/// OpenServiceW and three queries per service, so it is cached per service and re-read only every
/// ServiceMath::CONFIG_REFRESH, successful or not (readServiceConfig(), WindowsServiceConfig.h); a
/// service whose configuration is denied keeps those fields empty.
class WindowsServiceProbe : public IServiceProbe
{
  public:
    WindowsServiceProbe();
    ~WindowsServiceProbe() override;

    WindowsServiceProbe(const WindowsServiceProbe&) = delete;
    WindowsServiceProbe& operator=(const WindowsServiceProbe&) = delete;
    WindowsServiceProbe(WindowsServiceProbe&&) = delete;
    WindowsServiceProbe& operator=(WindowsServiceProbe&&) = delete;

    [[nodiscard]] ServiceCapabilities capabilities() const override;
    [[nodiscard]] ServiceEnumeration enumerate() override;

  private:
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
};

} // namespace Platform
