#pragma once

#include "Platform/ISystemProbe.h"
#include "Platform/Windows/WindowsSystemProbeMath.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace Platform
{

/// Windows implementation of ISystemProbe.
/// Reads system metrics from Windows APIs (GetSystemTimes, GlobalMemoryStatusEx, etc).
class WindowsSystemProbe : public ISystemProbe
{
  public:
    WindowsSystemProbe();
    ~WindowsSystemProbe() override = default;

    WindowsSystemProbe(const WindowsSystemProbe&) = delete;
    WindowsSystemProbe& operator=(const WindowsSystemProbe&) = delete;
    WindowsSystemProbe(WindowsSystemProbe&&) noexcept = default;
    WindowsSystemProbe& operator=(WindowsSystemProbe&&) noexcept = default;

    [[nodiscard]] SystemCounters read() override;
    [[nodiscard]] SystemCapabilities capabilities() const override;
    [[nodiscard]] long ticksPerSecond() const override;

  private:
    void readCpuCounters(SystemCounters& counters) const;
    void readPerCoreCpuCounters(SystemCounters& counters) const;
    static void readMemoryCounters(SystemCounters& counters);
    /// Page-file totals from NtQuerySystemInformation; zero when there is no page file or the
    /// query fails.
    [[nodiscard]] static SwapBytes readSwap();
    static void readUptime(SystemCounters& counters);
    void readStaticInfo(SystemCounters& counters) const;
    static void readCpuFreq(SystemCounters& counters);
    void readNetworkCounters(SystemCounters& counters);

    std::size_t m_NumCores{0};
    // Each processor group's first coreId, fixed for the boot session (#1107)
    std::vector<std::size_t> m_GroupFirstCoreIds;

    // The last all-group Total on a machine with several processor groups (#1107). A sample whose
    // group query fails repeats it rather than switching Total to a one-group source, which the
    // model would compare against the all-group sum. Sampler thread only.
    mutable std::optional<CpuCounters> m_LastAllGroupTotal;

    // The PnP device instance id of the adapter behind each network interface, by interface LUID
    // (#1284). An id once read is kept while the LUID still names the same interface -- its GUID,
    // held as two halves, is checked, since Windows can give a freed LUID to a later interface. An
    // empty read -- no value yet, or a failed read -- is retried no sooner than retryAt, so a value
    // that appears later is still found without a registry read every sample. Sampler thread only.
    struct AdapterDeviceInstanceId
    {
        std::uint64_t guidLow = 0;
        std::uint64_t guidHigh = 0;
        std::wstring id;
        std::chrono::steady_clock::time_point retryAt;
    };
    std::unordered_map<std::uint64_t, AdapterDeviceInstanceId> m_AdapterDeviceInstanceIds;
    // Cached static info (read once)
    std::string m_Hostname;
    std::string m_CpuModel;
};

} // namespace Platform
