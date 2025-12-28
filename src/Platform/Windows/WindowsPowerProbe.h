#pragma once

#include "Platform/IPowerProbe.h"

namespace Platform
{

/// Windows implementation of IPowerProbe.
/// Reads power/battery metrics from GetSystemPowerStatus and related APIs.
class WindowsPowerProbe : public IPowerProbe
{
  public:
    WindowsPowerProbe();
    ~WindowsPowerProbe() override = default;

    WindowsPowerProbe(const WindowsPowerProbe&) = delete;
    WindowsPowerProbe& operator=(const WindowsPowerProbe&) = delete;
    WindowsPowerProbe(WindowsPowerProbe&&) = default;
    WindowsPowerProbe& operator=(WindowsPowerProbe&&) = default;

    [[nodiscard]] PowerCounters read() override;
    [[nodiscard]] PowerCapabilities capabilities() const override;

  private:
    PowerCapabilities m_Capabilities;
};

} // namespace Platform
