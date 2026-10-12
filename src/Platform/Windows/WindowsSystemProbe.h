#pragma once

#include "Platform/CpuDetails.h"
#include "Platform/ISystemProbe.h"
#include "Platform/SystemTypes.h"
#include "Platform/Windows/WindowsSystemProbeMath.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h> // PERFORMANCE_INFORMATION
// clang-format on

#include "Platform/Windows/WindowsNtQuery.h"

// MIB_IF_TABLE2 (netioapi.h), declared here so this header needn't pull in winsock2.h and iphlpapi.h.
struct _MIB_IF_TABLE2; // NOLINT(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,readability-identifier-naming) - the SDK's tag

namespace Platform
{

class ProcessorPerformanceCounter;

/// The Win32/NT calls WindowsSystemProbe makes for CPU, topology, memory (#1719) and network (#1395): the
/// system's by default (WindowsSystemProbe::systemFunctions()), fakes in tests, so each one failing alone,
/// the per-core fallbacks, several processor groups and removed network adapters can be tested. Grouped by
/// area. Every member must be set, except the two NT exports, which are nullable as ntdll's lookups are.
struct WindowsSystemProbeFunctions
{
    struct Cpu
    {
        BOOL(WINAPI* getSystemTimes)(PFILETIME, PFILETIME, PFILETIME) = nullptr;
        WORD(WINAPI* getActiveProcessorGroupCount)() = nullptr;
        DWORD(WINAPI* getActiveProcessorCount)(WORD) = nullptr;
        WORD(WINAPI* getMaximumProcessorGroupCount)() = nullptr;
        DWORD(WINAPI* getMaximumProcessorCount)(WORD) = nullptr;
        BOOL(WINAPI* getLogicalProcessorInformationEx)(LOGICAL_PROCESSOR_RELATIONSHIP,
                                                       PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX,
                                                       PDWORD) = nullptr;
        void(WINAPI* getSystemInfo)(LPSYSTEM_INFO) = nullptr; // Processor-count fallback and page size
    } cpu;

    struct Nt
    {
        /// Per-core fallback, page files and virtualization status. Nullable: those are then not read.
        Windows::NtQuerySystemInformationFn ntQuerySystemInformation = nullptr;
        /// Per-core counters by processor group (#1107). Nullable: the one-group query is used.
        Windows::NtQuerySystemInformationExFn ntQuerySystemInformationEx = nullptr;
    } nt;

    struct Memory
    {
        BOOL(WINAPI* globalMemoryStatusEx)(LPMEMORYSTATUSEX) = nullptr;
        BOOL(WINAPI* getPerformanceInfo)(PPERFORMANCE_INFORMATION, DWORD) = nullptr;
    } memory;

    struct Network
    {
        /// GetIfTable2, and FreeMibTable, which frees the table it hands out.
        DWORD(WINAPI* getIfTable2)(_MIB_IF_TABLE2** table) = nullptr;
        void(WINAPI* freeMibTable)(PVOID memory) = nullptr;
        /// The PnP device instance id of the adapter behind an interface, from the registry; empty when the
        /// interface has none (Teredo, 6to4) or it can't be read (#1284).
        std::wstring (*readAdapterDeviceInstanceId)(const GUID& interfaceGuid) = nullptr;
        /// Whether the device with that id is in the system (CM_Locate_DevNodeW); Unknown for an empty id.
        DevicePresence (*devicePresence)(const std::wstring& deviceInstanceId) = nullptr;
    } network;
};

/// Windows implementation of ISystemProbe.
/// Reads system metrics from Windows APIs (GetSystemTimes, GlobalMemoryStatusEx, etc).
class WindowsSystemProbe : public ISystemProbe
{
  public:
    WindowsSystemProbe();
    /// The probe with its current-clock inputs supplied (#1184): the base clock in MHz (0 = unknown) and
    /// the "% Processor Performance" counter (null = none, and the base clock is reported). The default
    /// constructor reads the rated base (readNominalCpuBaseMHz in CpuBaseClock.h: CallNtPowerInformation's
    /// MaxMhz, else the registry's ~MHz; #1530) and opens pdh.dll's counter; tests inject fakes here.
    WindowsSystemProbe(std::uint64_t baseCpuMHz, std::unique_ptr<ProcessorPerformanceCounter> processorPerformance);
    /// As above, making every CPU, topology, memory and network call through @p functions (tests, #1719).
    WindowsSystemProbe(std::uint64_t baseCpuMHz,
                       std::unique_ptr<ProcessorPerformanceCounter> processorPerformance,
                       const WindowsSystemProbeFunctions& functions);
    ~WindowsSystemProbe() override;

    /// The real calls: the Win32 APIs themselves, and ntdll's exports resolved once.
    [[nodiscard]] static WindowsSystemProbeFunctions systemFunctions();

    WindowsSystemProbe(const WindowsSystemProbe&) = delete;
    WindowsSystemProbe& operator=(const WindowsSystemProbe&) = delete;
    // Not movable: nothing moves a probe (they live behind unique_ptr), and a defaulted noexcept move
    // could throw from the unordered_map member (bugprone-exception-escape)
    WindowsSystemProbe(WindowsSystemProbe&&) = delete;
    WindowsSystemProbe& operator=(WindowsSystemProbe&&) = delete;

    [[nodiscard]] SystemCounters read() override;
    [[nodiscard]] SystemCapabilities capabilities() const override;
    [[nodiscard]] long ticksPerSecond() const override;

    /// The nominal base clock in MHz the current clock is scaled from (#1530); 0 = unknown.
    [[nodiscard]] std::uint64_t baseCpuMHz() const noexcept
    {
        return m_BaseCpuMHz;
    }

  private:
    void readCpuCounters(SystemCounters& counters) const;
    void readPerCoreCpuCounters(SystemCounters& counters) const;
    void readMemoryCounters(SystemCounters& counters) const;
    /// Page-file totals from NtQuerySystemInformation; zero when there is no page file or the
    /// query fails.
    [[nodiscard]] SwapBytes readSwap() const;
    static void readUptime(SystemCounters& counters);
    void readStaticInfo(SystemCounters& counters) const;
    /// When the per-core read sampled a different set of active processors than m_CpuDetails
    /// describes, re-read them and commit the re-read only if its topology describes exactly this
    /// sample's processors (CpuTopology::commitIfConsistent(), #809); otherwise keep the previous
    /// details and retry on the next sample. m_NumCores (the published cpuCoreCount and the fallback
    /// buffer size) follows every non-empty sample.
    void refreshCpuDetailsIfProcessorsChanged(std::span<const CpuCounters> perCore);
    void readCpuFreq(SystemCounters& counters);
    void readNetworkCounters(SystemCounters& counters);

    WindowsSystemProbeFunctions m_Fns; // Every CPU, topology, memory and network call goes through these (#1719)
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
    // The nominal base clock (nominalCpuBaseMHz), read once: it is fixed for the boot session
    std::uint64_t m_BaseCpuMHz{0};
    // PDH's "% Processor Performance", which scales the base clock to the current one (#1184); null
    // when PDH is unavailable, and the base clock is reported instead. Sampler thread only.
    std::unique_ptr<ProcessorPerformanceCounter> m_ProcessorPerformance;
    // Cached static info (read once)
    std::string m_Hostname;
    std::string m_CpuModel;
    // Topology, caches, rated base clock and virtualization status (#809), with the active processor
    // ids they describe; read at construction and again when the set of active processors the
    // per-core read samples changes (a processor hot-added). Sampler thread only.
    CpuTopology::CachedCpuDetails m_CpuDetails;
};

} // namespace Platform
