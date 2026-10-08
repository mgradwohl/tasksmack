#pragma once

#include "Platform/IServiceProbe.h"

#include <memory>
#include <vector>

namespace Platform
{

/// Windows services from the Service Control Manager (#800), read-only and without elevation.
///
/// The list and each service's state and PID come from one EnumServicesStatusExW per call. The
/// configuration (start type, command line, account, description, delayed auto-start) needs a
/// per-service open, so it is cached per service and re-read only every CONFIG_REFRESH; a service
/// whose configuration is denied keeps those fields empty.
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
    [[nodiscard]] std::vector<ServiceInfo> enumerate() override;

  private:
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
};

} // namespace Platform
