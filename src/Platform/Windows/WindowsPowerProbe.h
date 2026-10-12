#pragma once

#include "Platform/IPowerProbe.h"
#include "WindowsBatteryInfo.h"

#include <chrono>

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h> // GetSystemPowerStatus
// clang-format on

namespace Platform
{

/// Windows implementation of IPowerProbe.
/// Reads the live charge state from GetSystemPowerStatus, and the battery's static facts (design
/// and full-charge capacity, cycle count, chemistry, manufacturer, model) from the battery device
/// (WindowsBatteryInfo.h, #1523): at construction, then again only when the power source changes
/// or every few minutes, never per sample.
class WindowsPowerProbe : public IPowerProbe
{
  public:
    /// GetSystemPowerStatus's signature, so tests can report a battery, a failure or a power-source change.
    using GetSystemPowerStatusFn = BOOL(WINAPI*)(LPSYSTEM_POWER_STATUS);

    WindowsPowerProbe();
    /// With the battery device calls, and optionally GetSystemPowerStatus, injected (tests, #1395).
    explicit WindowsPowerProbe(const Windows::BatteryDeviceFunctions& batteryApi,
                               GetSystemPowerStatusFn getSystemPowerStatus = &::GetSystemPowerStatus);
    ~WindowsPowerProbe() override = default;

    WindowsPowerProbe(const WindowsPowerProbe&) = delete;
    WindowsPowerProbe& operator=(const WindowsPowerProbe&) = delete;
    WindowsPowerProbe(WindowsPowerProbe&&) noexcept = default;
    WindowsPowerProbe& operator=(WindowsPowerProbe&&) noexcept = default;

    [[nodiscard]] PowerCounters read() override;
    [[nodiscard]] PowerCapabilities capabilities() const override;

  private:
    void refreshBatteryInfo(bool isOnAc);

    PowerCapabilities m_Capabilities;
    Windows::BatteryDeviceFunctions m_BatteryApi;
    GetSystemPowerStatusFn m_GetSystemPowerStatus;
    Windows::BatteryInfo m_BatteryInfo;
    std::chrono::steady_clock::time_point m_BatteryInfoReadAt;
    bool m_BatteryInfoOnAc = false;
};

} // namespace Platform
