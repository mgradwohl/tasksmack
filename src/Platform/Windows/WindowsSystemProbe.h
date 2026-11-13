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
#include <string_view>
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

/// Opens PDH's "% Processor Performance" counter for the current clock (#1184): the counter, or null
/// when it can't be opened. The default probe's opener opens pdh.dll's counter
/// (ProcessorPerformanceCounter::open()); tests pass a fake.
using ProcessorPerformanceOpener = std::unique_ptr<ProcessorPerformanceCounter> (*)();

/// The "% Processor Performance" counter, left for WindowsSystemProbe to open itself on its second read()
/// (#3154). PDH's first use in the process loads the perf counter name tables, which takes 130-300 ms,
/// and the first read() is the seed the system sampler publishes; the clock is the only figure that
/// needs PDH. A struct rather than the bare opener, so a probe built with nullptr for its counter still
/// picks the constructor that takes an open one.
struct DeferredProcessorPerformance
{
    ProcessorPerformanceOpener open = nullptr;
};

/// The Win32/NT calls WindowsSystemProbe makes for CPU, topology, memory (#1719) and network (#1395): the
/// system's by default (WindowsSystemProbe::systemFunctions()), fakes in tests, so each one failing alone,
/// the per-core fallbacks, several processor groups and removed network adapters can be tested. Grouped by
/// area. Every member must be set, except the two NT exports, which are nullable as ntdll's lookups are, and
/// the NUMA calls (#3088): without GetNumaHighestNodeNumber or GetNumaAvailableMemoryNodeEx, or without both
/// processor-mask calls, the NUMA nodes aren't read.
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

    struct Numa
    {
        BOOL(WINAPI* getNumaHighestNodeNumber)(PULONG) = nullptr;
        /// Every processor group's mask for a node; kernel32 has it from Windows 11 and Windows Server 2022
        /// (null before: GetNumaNodeProcessorMaskEx is used, which gives one group's).
        BOOL(WINAPI* getNumaNodeProcessorMask2)(USHORT, PGROUP_AFFINITY, USHORT, PUSHORT) = nullptr;
        BOOL(WINAPI* getNumaNodeProcessorMaskEx)(USHORT, PGROUP_AFFINITY) = nullptr;
        BOOL(WINAPI* getNumaAvailableMemoryNodeEx)(USHORT, PULONGLONG) = nullptr;
    } numa;

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

    /// The monotonic clock: read()'s sub-read timing (#2731) and the network adapter cache's retry and
    /// backstop times. A test's fake clock stands still or jumps.
    std::chrono::steady_clock::time_point (*now)() noexcept = nullptr;
};

/// How long WindowsSystemProbe::read() spent in its sub-reads (#2731): the slowest one, by name, and
/// all of them together. Read it after read(), on the same thread, so an overrun names "pdh-freq" or
/// "ifTable" rather than "system".
struct SystemReadTiming
{
    /// "cpu" (GetSystemTimes and the per-core NT query), "cpu-details" (the topology re-read, rare),
    /// "memory" (GlobalMemoryStatusEx, GetPerformanceInfo, the page files, the process list for
    /// compressed memory and the NUMA nodes), "uptime", "pdh-freq"
    /// (the PDH collect) or "ifTable" (GetIfTable2 and the device lookups); empty before the first read().
    std::string_view slowestSubRead;
    std::chrono::steady_clock::duration slowest{};
    std::chrono::steady_clock::duration total{};
};

/// Windows implementation of ISystemProbe.
/// Reads system metrics from Windows APIs (GetSystemTimes, GlobalMemoryStatusEx, etc).
class WindowsSystemProbe : public ISystemProbe
{
  public:
    /// The system's probe: the rated base clock (readNominalCpuBaseMHz in CpuBaseClock.h:
    /// CallNtPowerInformation's MaxMhz, else the registry's ~MHz; #1530), and pdh.dll's counter, opened
    /// on the second read() (#3154).
    WindowsSystemProbe();
    /// The probe with its current-clock inputs supplied (#1184): the base clock in MHz (0 = unknown) and
    /// the "% Processor Performance" counter (null = none, and the base clock is reported), already open,
    /// so the first read() uses it. Tests inject fakes here.
    WindowsSystemProbe(std::uint64_t baseCpuMHz, std::unique_ptr<ProcessorPerformanceCounter> processorPerformance);
    /// As above, with the counter opened by @p processorPerformance on the second read() (#3154): the
    /// first read() makes no PDH call and reports the base clock, as does the read() that opens it,
    /// whose collection only primes the rate. An opener that returns null isn't called again.
    WindowsSystemProbe(std::uint64_t baseCpuMHz, DeferredProcessorPerformance processorPerformance);
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

    /// The last read()'s sub-read timing (#2731); the thread that calls read() reads it.
    [[nodiscard]] SystemReadTiming lastReadTiming() const noexcept
    {
        return m_LastReadTiming;
    }

  private:
    // The adapter behind a network interface (#1284): its PnP device instance id and, for a down
    // interface, whether that device is still in the system (#2731). An id once read is kept while the
    // LUID still names the same interface -- its GUID, held as two halves, is checked, since Windows
    // can give a freed LUID to a later interface. An empty read -- no value yet, or a failed read -- is
    // retried no sooner than retryAt, so a value that appears later is still found without a registry
    // read every sample. The presence, a PnP call, is kept until the interface set or a status changes
    // (every entry's is dropped), the id is read again, or presenceExpiresAt, the backstop.
    struct AdapterDevice
    {
        std::uint64_t guidLow = 0;
        std::uint64_t guidHigh = 0;
        std::wstring id;
        std::chrono::steady_clock::time_point retryAt;
        DevicePresence presence = DevicePresence::Unknown;
        bool presenceKnown = false;
        std::chrono::steady_clock::time_point presenceExpiresAt;
    };

    void readCpuCounters(SystemCounters& counters) const;
    void readPerCoreCpuCounters(SystemCounters& counters) const;
    void readMemoryCounters(SystemCounters& counters) const;
    /// Compressed memory (#3363): the "Memory Compression" process's working set; not set when the
    /// process isn't there (compression is off) or the query fails. Once a whole SystemProcessInformation
    /// snapshot has found the process, each sample reads only its session's process list
    /// (SystemSessionProcessInformation) for that process id (#3483); the whole snapshot is taken again
    /// only when the process is gone from it, and, while none is known, no more than every
    /// MEMORY_COMPRESSION_SEARCH_INTERVAL_SECONDS.
    void readMemoryCompression(SystemCounters& counters);
    /// The whole-snapshot search readMemoryCompression() makes: the process's working set into
    /// @p counters and its id and session into m_MemoryCompression, or, when there is none or the query
    /// fails, neither, and the next search scheduled.
    void searchMemoryCompression(SystemCounters& counters, std::chrono::steady_clock::time_point now);
    /// The NUMA nodes (#3088): each node's available memory every sample, with its processors as the
    /// last listing found them. The nodes and their processor masks are listed again every
    /// NUMA_RESCAN_INTERVAL_SECONDS, since only a hot-add changes them. Nothing when m_HasNuma is false.
    void readNuma(SystemCounters& counters);
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
    /// The interface's cache entry, its device instance id read from the registry when due (#1284).
    [[nodiscard]] AdapterDevice&
    adapterDevice(std::uint64_t interfaceLuid, const GUID& interfaceGuid, std::chrono::steady_clock::time_point now);
    /// Whether a down interface's adapter device is in the system: the cached answer, or the PnP lookup
    /// when the cache has none (#2731).
    [[nodiscard]] DevicePresence
    adapterDevicePresence(std::uint64_t interfaceLuid, const GUID& interfaceGuid, std::chrono::steady_clock::time_point now);

    WindowsSystemProbeFunctions m_Fns; // Every CPU, topology, memory and network call goes through these (#1719)
    std::size_t m_NumCores{0};
    // Each processor group's first coreId, fixed for the boot session (#1107)
    std::vector<std::size_t> m_GroupFirstCoreIds;

    // The last all-group Total on a machine with several processor groups (#1107). A sample whose
    // group query fails repeats it rather than switching Total to a one-group source, which the
    // model would compare against the all-group sum. Sampler thread only.
    mutable std::optional<CpuCounters> m_LastAllGroupTotal;

    // The adapter behind each network interface, by interface LUID (AdapterDevice). Sampler thread only.
    std::unordered_map<std::uint64_t, AdapterDevice> m_AdapterDevices;
    // The interfaces and statuses GetIfTable2 listed last pass, sorted, and the scratch this pass's are
    // collected in (networkInterfacesChanged() swaps them, so neither allocates once grown). Sampler
    // thread only.
    std::vector<NetworkInterfaceKey> m_InterfaceKeys;
    std::vector<NetworkInterfaceKey> m_InterfaceKeysScratch;
    // The process list readMemoryCompression() reads (#3363): the whole SystemProcessInformation snapshot
    // when it searches, else the Memory Compression process's session's (#3483). Kept between samples so
    // it isn't allocated again once grown. Sampler thread only.
    std::vector<std::byte> m_ProcessSnapshot;
    // The Memory Compression process the last search found (#3483): its id and session, which each sample
    // reads; nullopt while none is known. Sampler thread only.
    struct MemoryCompressionTarget
    {
        std::uint64_t processId = 0;
        std::uint32_t sessionId = 0;
    };
    std::optional<MemoryCompressionTarget> m_MemoryCompression;
    // While none is known, the earliest the next whole-snapshot search may run (#3483). Sampler thread only.
    std::chrono::steady_clock::time_point m_NextMemoryCompressionSearch;
    // The last read()'s sub-read timing (#2731). Sampler thread only.
    SystemReadTiming m_LastReadTiming;
    // The nominal base clock (nominalCpuBaseMHz), read once: it is fixed for the boot session
    std::uint64_t m_BaseCpuMHz{0};
    // PDH's "% Processor Performance", which scales the base clock to the current one (#1184); null
    // when PDH is unavailable or not open yet, and the base clock is reported instead. Sampler thread only.
    std::unique_ptr<ProcessorPerformanceCounter> m_ProcessorPerformance;
    // What opens m_ProcessorPerformance on the second read() (#3154); null once called, or when the
    // counter was supplied open. Sampler thread only.
    ProcessorPerformanceOpener m_OpenProcessorPerformance = nullptr;
    // Cached static info (read once)
    std::string m_Hostname;
    std::string m_CpuModel;
    // Topology, caches, rated base clock and virtualization status (#809), with the active processor
    // ids they describe; read at construction and again when the set of active processors the
    // per-core read samples changes (a processor hot-added). Sampler thread only.
    CpuTopology::CachedCpuDetails m_CpuDetails;
    // The NUMA nodes (#3088): whether they're read (the calls are there and GetNumaHighestNodeNumber
    // answered at construction), decided once, so capabilities() reads it without a lock; the last
    // listing's nodes, each with its id and processors and its memory left at 0; and when it was taken.
    // Sampler thread only, but for m_HasNuma.
    bool m_HasNuma = false;
    bool m_NumaScanned = false;
    std::vector<NumaNode> m_NumaLayout;
    std::chrono::steady_clock::time_point m_NumaScannedAt;
    // Set by the first read(), so the second opens m_OpenProcessorPerformance's counter (#3154)
    bool m_FirstReadDone = false;
    // Set when SystemSessionProcessInformation fails (#3483): each sample then searches the whole
    // snapshot, as before it. Sampler thread only.
    bool m_SessionProcessQueryFails = false;
};

} // namespace Platform
