#pragma once

#include "Platform/IPowerProbe.h"
#include "WindowsBatteryInfo.h"

#include <chrono>
#include <mutex>
#include <optional>
#include <thread>

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
/// Reads the live charge state from GetSystemPowerStatus on every read(), and the battery's static facts
/// (design and full-charge capacity, cycle count, chemistry, manufacturer, model) from the battery device
/// (WindowsBatteryInfo.h, #1523): once at construction, then again only when a battery appears, the power
/// source changes or every BATTERY_INFO_REFRESH_SECONDS, never per sample. Each re-read runs on a one-shot
/// worker thread (ts-battery-read, #2731): the SetupAPI enumeration and the battery IOCTLs can stall on a
/// slow ACPI call, which must never stretch the sampling pass that calls read(). read() takes a finished
/// re-read's result at its next pass. The capabilities are recomputed on every read(), so a battery
/// inserted or removed after start, or details first answered late, show (#2413).
class WindowsPowerProbe : public IPowerProbe
{
  public:
    /// GetSystemPowerStatus's signature, so tests can report a battery, a failure or a power-source change.
    using GetSystemPowerStatusFn = BOOL(WINAPI*)(LPSYSTEM_POWER_STATUS);

    WindowsPowerProbe();
    /// With the battery device calls, and optionally GetSystemPowerStatus, injected (tests, #1395).
    explicit WindowsPowerProbe(const Windows::BatteryDeviceFunctions& batteryApi,
                               GetSystemPowerStatusFn getSystemPowerStatus = &::GetSystemPowerStatus);
    /// Joins a battery re-read still running.
    ~WindowsPowerProbe() override = default;

    WindowsPowerProbe(const WindowsPowerProbe&) = delete;
    WindowsPowerProbe& operator=(const WindowsPowerProbe&) = delete;
    // Not movable: the worker holds `this`, and nothing moves a probe (they live behind unique_ptr)
    WindowsPowerProbe(WindowsPowerProbe&&) = delete;
    WindowsPowerProbe& operator=(WindowsPowerProbe&&) = delete;

    [[nodiscard]] PowerCounters read() override;
    [[nodiscard]] PowerCapabilities capabilities() const override;

  private:
    /// Start the worker that reads the battery's static facts or, while one runs, have another start once
    /// its result is taken: the running read may predate the change that asked for this one.
    void requestBatteryInfoRefresh(bool isOnAc);
    void startBatteryInfoRefresh();
    /// A finished worker's result into m_BatteryInfo, and the re-read deferred behind it started.
    void takeFinishedBatteryInfo();
    /// m_Capabilities from this power status and m_BatteryInfo; read() calls it every sample (#2413).
    void updateCapabilities(const SYSTEM_POWER_STATUS& status);

    PowerCapabilities m_Capabilities;
    Windows::BatteryDeviceFunctions m_BatteryApi; // Fixed after construction: the worker reads it
    GetSystemPowerStatusFn m_GetSystemPowerStatus;
    // The thread that calls read() owns these: the facts read() reports, when the last re-read started
    // and under which power source, and whether a worker's result is still to come
    Windows::BatteryInfo m_BatteryInfo;
    std::chrono::steady_clock::time_point m_BatteryInfoReadAt;
    bool m_BatteryInfoOnAc = false;
    bool m_BatteryInfoRefreshing = false;   ///< A worker's result has not been taken yet
    bool m_BatteryInfoRefreshAgain = false; ///< Once it is, read again: a change came while it ran
    // The worker's result, handed over under the mutex
    std::mutex m_FinishedBatteryInfoMutex;
    std::optional<Windows::BatteryInfo> m_FinishedBatteryInfo;
    std::jthread m_BatteryInfoWorker; // Last: joined first, while the rest is alive
};

} // namespace Platform
