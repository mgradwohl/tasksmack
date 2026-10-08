#include "WindowsSystemProbe.h"

#include "Platform/CpuDetails.h"
#include "Platform/SystemTypes.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>

// clang-format off
// Windows headers - version macros set via CMake compile definitions
// (WIN32_LEAN_AND_MEAN, NOMINMAX, _WIN32_WINNT, WINVER, NTDDI_VERSION)
#include <winsock2.h>    // Must come before windows.h
#include <ws2ipdef.h>    // Required for MIB_IF_ROW2/MIB_IF_TABLE2 definitions in netioapi.h
#include <windows.h>
#include <winternl.h>
#include <iphlpapi.h>    // Network interface APIs (includes netioapi.h)
#include <cfgmgr32.h>    // CM_Locate_DevNodeW: whether an adapter's device is present (#1284)
// clang-format on

#undef max
#undef min

#include "CpuBaseClock.h"
#include "ProcessorPerformanceCounter.h"
#include "WinString.h"
#include "WindowsNtQuery.h"
#include "WindowsSystemProbeMath.h"

#include <array>
#include <bit>
#include <chrono>
#include <concepts>
#include <cwchar>
#include <format>
#include <ranges>
#include <span>
#include <type_traits>
#include <vector>

#include <psapi.h> // GetPerformanceInfo (K32GetPerformanceInfo, in kernel32)

#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
#include <intrin.h> // __cpuid: the hypervisor-present bit (#809)
#endif

namespace Platform
{

namespace
{

/// Convert FILETIME to 100-nanosecond intervals (ticks)
[[nodiscard]] uint64_t filetimeToTicks(const FILETIME& ft)
{
    ULARGE_INTEGER uli{};
    uli.LowPart = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;
    return uli.QuadPart;
}

template<std::integral T> [[nodiscard]] constexpr auto toU64NonNegative(T value) noexcept -> uint64_t
{
    if constexpr (std::is_signed_v<T>)
    {
        if (value < 0)
        {
            return 0;
        }
    }

    return static_cast<uint64_t>(value);
}

[[nodiscard]] uint64_t largeIntegerToTicks(const LARGE_INTEGER& value)
{
    return toU64NonNegative(value.QuadPart);
}

// System information class for per-processor performance
constexpr ULONG SystemProcessorPerformanceInformation = 8;

// System information class for page-file sizes (SYSTEM_PAGEFILE_INFORMATION chain)
constexpr ULONG SystemPageFileInformationClass = 18;

// STATUS_INFO_LENGTH_MISMATCH, spelled out: ntstatus.h conflicts with windows.h's subset.
constexpr NTSTATUS STATUS_INFO_LENGTH_MISMATCH_VALUE = static_cast<NTSTATUS>(0xC0000004L);

// Per-processor performance information structure
// This matches the undocumented SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION
struct ProcessorPerformanceInfo
{
    LARGE_INTEGER IdleTime;
    LARGE_INTEGER KernelTime; // Includes idle time
    LARGE_INTEGER UserTime;
    LARGE_INTEGER DpcTime;
    LARGE_INTEGER InterruptTime;
    ULONG InterruptCount;
};

/// Run one SystemProcessorPerformanceInformation query into `buffer`, growing it and retrying on
/// a length mismatch (the processor count can change between sizing and querying).
/// @param query  Calls NtQuerySystemInformation(Ex) with (buffer, byteLength, &returnLength).
template<typename Query>
[[nodiscard]] NTSTATUS queryProcessorPerformance(std::vector<ProcessorPerformanceInfo>& buffer, ULONG& returnLength, Query query)
{
    NTSTATUS status = 0;
    for (int attempt = 0; attempt < 3; ++attempt)
    {
        returnLength = 0;
        status = query(buffer.data(), static_cast<ULONG>(buffer.size() * sizeof(ProcessorPerformanceInfo)), &returnLength);
        if (status != STATUS_INFO_LENGTH_MISMATCH_VALUE)
        {
            break;
        }
        // Grow with headroom: the processor count can change between calls.
        const std::size_t neededEntries = (static_cast<std::size_t>(returnLength) / sizeof(ProcessorPerformanceInfo)) + 8;
        buffer.assign(neededEntries, ProcessorPerformanceInfo{});
    }
    return status;
}

/// Logical processors across every processor group. dwNumberOfProcessors counts only the calling
/// thread's group (at most 64), so a machine with more under-reported its core count (#1107).
[[nodiscard]] std::size_t logicalProcessorCount()
{
    const DWORD allGroups = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    if (allGroups != 0)
    {
        return allGroups;
    }
    SYSTEM_INFO sysInfo{};
    GetSystemInfo(&sysInfo);
    return sysInfo.dwNumberOfProcessors;
}

/// Each processor group's first coreId, from the groups' maximum sizes (#1107; see
/// processorGroupFirstCoreIds()). Read once: the maximums are fixed for the boot session.
[[nodiscard]] std::vector<std::size_t> groupFirstCoreIds()
{
    const WORD groups = GetMaximumProcessorGroupCount();
    std::vector<std::uint32_t> maximums;
    maximums.reserve(groups);
    for (WORD group = 0; group < groups; ++group)
    {
        maximums.push_back(GetMaximumProcessorCount(group));
    }
    return processorGroupFirstCoreIds(maximums);
}

/// The registry's ~MHz; 0 if it can't be read. Not the rated base on hybrid parts (#1530): only the
/// fallback when CallNtPowerInformation has no MaxMhz.
[[nodiscard]] std::uint64_t readRegistryCpuMHz()
{
    DWORD mhz = 0;
    DWORD dataSize = sizeof(mhz);
    if (RegGetValueW(HKEY_LOCAL_MACHINE,
                     LR"(HARDWARE\DESCRIPTION\System\CentralProcessor\0)",
                     L"~MHz",
                     RRF_RT_REG_DWORD,
                     nullptr,
                     &mhz,
                     &dataSize) != ERROR_SUCCESS)
    {
        return 0;
    }
    return toU64NonNegative(mhz);
}

// System information classes for the virtualization-based security status (#809)
constexpr ULONG SystemCodeIntegrityInformationClass = 103;
constexpr ULONG SystemIsolatedUserModeInformationClass = 165;

/// Sockets, cores, logical processors and cache sizes from one GetLogicalProcessorInformationEx call
/// (#809). Fields stay nullopt if the call fails. `groupFirstCoreIds` numbers each logical
/// processor as the per-core counters do (#1107), for efficiencyClassByCoreId and `activeIds`, which
/// receives every active logical processor's id, ascending (empty if the call fails).
void readProcessorTopology(CpuDetails& details, std::span<const std::size_t> groupFirstCoreIds, std::vector<std::size_t>& activeIds)
{
    DWORD length = 0;
    if (GetLogicalProcessorInformationEx(RelationAll, nullptr, &length) != FALSE || GetLastError() != ERROR_INSUFFICIENT_BUFFER ||
        length == 0)
    {
        spdlog::debug("GetLogicalProcessorInformationEx sizing failed: {}", GetLastError());
        return;
    }
    std::vector<std::byte> buffer(length);
    if (GetLogicalProcessorInformationEx(RelationAll, reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data()), &length) ==
        FALSE)
    {
        spdlog::debug("GetLogicalProcessorInformationEx failed: {}", GetLastError());
        return;
    }
    const std::size_t total = std::min<std::size_t>(length, buffer.size());

    std::size_t packages = 0;
    std::vector<CpuTopology::CoreRecord> cores;
    std::vector<CpuTopology::CacheInstance> caches;
    constexpr std::size_t RECORD_HEADER_BYTES = offsetof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX, Processor);
    std::size_t offset = 0;
    while (offset + RECORD_HEADER_BYTES <= total)
    {
        // Copied out rather than cast in place: records are variable-sized, and a copy needs no
        // alignment. A core's processors are in one group, so its first GroupMask is all of them.
        SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX record{};
        const std::size_t available = total - offset;
        std::memcpy(&record, &buffer[offset], std::min(available, sizeof(record)));
        if (record.Size == 0 || record.Size > available)
        {
            break;
        }
        // NOLINTBEGIN(cppcoreguidelines-pro-type-union-access) - Relationship selects the union member
        switch (record.Relationship)
        {
        case RelationProcessorPackage:
            ++packages;
            break;
        case RelationProcessorCore:
            if (record.Processor.GroupCount > 0 && record.Processor.GroupMask[0].Group < groupFirstCoreIds.size())
            {
                const std::size_t firstId = groupFirstCoreIds[record.Processor.GroupMask[0].Group];
                for (KAFFINITY mask = record.Processor.GroupMask[0].Mask; mask != 0; mask &= mask - 1)
                {
                    const auto bit = static_cast<std::size_t>(std::countr_zero(mask));
                    CpuTopology::setEfficiencyClass(details.efficiencyClassByCoreId, firstId + bit, record.Processor.EfficiencyClass);
                    activeIds.push_back(firstId + bit);
                }
            }
            cores.push_back({.efficiencyClass = record.Processor.EfficiencyClass,
                             .logicalProcessors = (record.Processor.GroupCount > 0)
                                                    ? static_cast<std::uint32_t>(std::popcount(record.Processor.GroupMask[0].Mask))
                                                    : 0U});
            break;
        case RelationCache:
            caches.push_back({.level = record.Cache.Level, .bytes = record.Cache.CacheSize});
            break;
        default:
            break;
        }
        // NOLINTEND(cppcoreguidelines-pro-type-union-access)
        offset += record.Size;
    }

    if (packages > 0)
    {
        details.sockets = packages;
    }
    CpuTopology::summarizeCores(cores, details);
    CpuTopology::keepOnlyIfHybrid(details.efficiencyClassByCoreId);
    std::ranges::sort(activeIds);
    CpuTopology::sumCacheInstances(caches, details);
}

/// Firmware virtualization, SLAT, a running hypervisor and virtualization-based security (#809):
/// processor-feature flags, CPUID, and two NtQuerySystemInformation classes a standard user may
/// query -- no WMI, no elevation.
void readVirtualization(CpuDetails& details)
{
    details.virtualizationFirmwareEnabled = IsProcessorFeaturePresent(PF_VIRT_FIRMWARE_ENABLED) != FALSE;
    details.slatSupported = IsProcessorFeaturePresent(PF_SECOND_LEVEL_ADDRESS_TRANSLATION) != FALSE;
#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
    std::array<int, 4> registers{};
    __cpuid(registers.data(), 1);
    details.hypervisorPresent = hypervisorPresentBit(static_cast<std::uint32_t>(registers[2]));
#endif

    const auto ntQuery = Windows::ntQuerySystemInformation();
    if (ntQuery == nullptr)
    {
        return;
    }
    struct CodeIntegrityInformation // SYSTEM_CODEINTEGRITY_INFORMATION
    {
        ULONG Length;
        ULONG CodeIntegrityOptions;
    };
    CodeIntegrityInformation codeIntegrity{.Length = sizeof(CodeIntegrityInformation), .CodeIntegrityOptions = 0};
    if (ntQuery(SystemCodeIntegrityInformationClass, &codeIntegrity, sizeof(codeIntegrity), nullptr) == 0)
    {
        details.hvciEnabled = hvciEnabled(codeIntegrity.CodeIntegrityOptions);
    }
    // SYSTEM_ISOLATED_USER_MODE_INFORMATION: two flag bytes, six spare bytes and a spare ULONGLONG.
    std::array<std::uint8_t, 16> isolatedUserMode{};
    if (ntQuery(SystemIsolatedUserModeInformationClass, isolatedUserMode.data(), static_cast<ULONG>(isolatedUserMode.size()), nullptr) == 0)
    {
        details.vbsRunning = secureKernelRunning(isolatedUserMode[0]);
    }
}

/// Every CpuDetails fact this probe reports (#809). Base speed is the rated MaxMhz from powrprof
/// (#1530) and stays unknown without it: the registry's ~MHz fallback is not a base clock.
/// `activeIds` receives the active logical processors' ids the topology describes (ascending).
[[nodiscard]] CpuDetails readCpuDetails(std::span<const std::size_t> groupFirstCoreIds, std::vector<std::size_t>& activeIds)
{
    CpuDetails details;
    activeIds.clear();
    readProcessorTopology(details, groupFirstCoreIds, activeIds);
    readVirtualization(details);
    const std::vector<std::uint32_t> processorMaxMHz = readProcessorMaxMHz(&CallNtPowerInformation);
    if (const std::uint64_t ratedMHz = nominalCpuBaseMHz(processorMaxMHz, 0); ratedMHz > 0)
    {
        details.baseSpeedMHz = ratedMHz;
    }
    return details;
}

/// The nominal base clock "% Processor Performance" scales (#1530): powrprof's rated MaxMhz, else ~MHz.
[[nodiscard]] std::uint64_t readBaseCpuMHz()
{
    return readNominalCpuBaseMHz(&CallNtPowerInformation, readRegistryCpuMHz());
}

} // namespace

WindowsSystemProbe::~WindowsSystemProbe() = default;

WindowsSystemProbe::WindowsSystemProbe() : WindowsSystemProbe(readBaseCpuMHz(), ProcessorPerformanceCounter::open())
{}

WindowsSystemProbe::WindowsSystemProbe(std::uint64_t baseCpuMHz, std::unique_ptr<ProcessorPerformanceCounter> processorPerformance)
    : m_NumCores(logicalProcessorCount()),
      m_GroupFirstCoreIds(groupFirstCoreIds()),
      m_BaseCpuMHz(baseCpuMHz),
      m_ProcessorPerformance(std::move(processorPerformance))
{
    // Get hostname (UTF-8 via wide API)
    std::array<wchar_t, MAX_COMPUTERNAME_LENGTH + 1> hostBuffer{};
    // Note: Windows APIs require DWORD for buffer sizes; explicit usage is intentional.
    DWORD bufferSize = MAX_COMPUTERNAME_LENGTH + 1;
    if (GetComputerNameW(hostBuffer.data(), &bufferSize) != 0)
    {
        m_Hostname = WinString::wideToUtf8(hostBuffer.data());
    }
    else
    {
        m_Hostname = "unknown";
    }

    // Get CPU model from registry
    {
        constexpr auto* cpuKeyPath = LR"(HARDWARE\DESCRIPTION\System\CentralProcessor\0)";
        constexpr auto* cpuValueName = L"ProcessorNameString";

        DWORD cpuBufferSize = 0;
        const LSTATUS sizeStatus =
            RegGetValueW(HKEY_LOCAL_MACHINE, cpuKeyPath, cpuValueName, RRF_RT_REG_SZ, nullptr, nullptr, &cpuBufferSize);
        std::vector<wchar_t> cpuBuffer(cpuBufferSize / sizeof(wchar_t));
        // Derive the byte size back from the actually-allocated buffer rather than reusing
        // the raw registry-reported cpuBufferSize, which may be one wchar_t larger than
        // cpuBuffer.size() * sizeof(wchar_t) due to the truncating division above.
        auto cpuBufferBytes = static_cast<DWORD>(cpuBuffer.size() * sizeof(wchar_t));
        if (sizeStatus == ERROR_SUCCESS && !cpuBuffer.empty() &&
            RegGetValueW(HKEY_LOCAL_MACHINE, cpuKeyPath, cpuValueName, RRF_RT_REG_SZ, nullptr, cpuBuffer.data(), &cpuBufferBytes) ==
                ERROR_SUCCESS)
        {
            m_CpuModel = WinString::wideToUtf8(cpuBuffer.data());

            // Trim leading/trailing whitespace
            while (!m_CpuModel.empty() && m_CpuModel[0] == ' ')
            {
                m_CpuModel.erase(0, 1);
            }
            while (!m_CpuModel.empty() && m_CpuModel.back() == ' ')
            {
                m_CpuModel.pop_back();
            }
        }
    }
    if (m_CpuModel.empty())
    {
        m_CpuModel = "Unknown CPU";
    }

    // The CPU Details facts (#809), read again only when the set of active processors changes
    // (refreshCpuDetailsIfProcessorsChanged())
    m_CpuDetails.details = readCpuDetails(m_GroupFirstCoreIds, m_CpuDetails.ids);

    spdlog::debug("WindowsSystemProbe initialized with {} cores, host={}, cpu={}", m_NumCores, m_Hostname, m_CpuModel);
}

SystemCounters WindowsSystemProbe::read()
{
    SystemCounters counters{};

    readCpuCounters(counters);
    refreshCpuDetailsIfProcessorsChanged(counters.cpuPerCore);
    readMemoryCounters(counters);
    readUptime(counters);
    readStaticInfo(counters);
    readCpuFreq(counters);
    readNetworkCounters(counters);

    return counters;
}

void WindowsSystemProbe::readCpuCounters(SystemCounters& counters) const
{
    // First, get total CPU via GetSystemTimes (always works)
    FILETIME ftIdle{};
    FILETIME ftKernel{};
    FILETIME ftUser{};

    if (GetSystemTimes(&ftIdle, &ftKernel, &ftUser) == 0)
    {
        spdlog::error("GetSystemTimes failed: {}", GetLastError());
        return;
    }

    // GetSystemTimes returns:
    // - idle: time spent idle
    // - kernel: time spent in kernel mode (includes idle time)
    // - user: time spent in user mode
    //
    // To get actual kernel time: kernel - idle

    const uint64_t idle = filetimeToTicks(ftIdle);
    const uint64_t kernel = filetimeToTicks(ftKernel);
    const uint64_t user = filetimeToTicks(ftUser);
    const uint64_t system = kernel - idle; // Actual kernel time

    counters.cpuTotal.idle = idle;
    counters.cpuTotal.system = system;
    counters.cpuTotal.user = user;

    // Now get per-core CPU via NtQuerySystemInformation
    readPerCoreCpuCounters(counters);
}

void WindowsSystemProbe::readPerCoreCpuCounters(SystemCounters& counters) const
{
    // KernelTime includes idle, and DPC/interrupt time are inside kernel time; processorTimes()
    // splits them so CpuCounters::active() counts each tick once (#1032). DpcTime is the
    // closest Windows analogue of Linux softirq.
    const auto toCounters = [](const ProcessorPerformanceInfo& info)
    {
        return processorTimes(largeIntegerToTicks(info.KernelTime),
                              largeIntegerToTicks(info.IdleTime),
                              largeIntegerToTicks(info.UserTime),
                              largeIntegerToTicks(info.DpcTime),
                              largeIntegerToTicks(info.InterruptTime));
    };

    // SystemProcessorPerformanceInformation reports only the calling thread's processor group,
    // so a machine with more than 64 logical processors (several groups) showed one group's
    // cores (#1107). NtQuerySystemInformationEx takes the group as input: query each in turn and
    // append them in group order.
    const WORD groupCount = GetActiveProcessorGroupCount();
    const bool multiGroup = groupCount > 1;
    if (const auto ntQueryEx = Windows::ntQuerySystemInformationEx(); ntQueryEx != nullptr)
    {
        std::vector<CpuCounters> cores;
        cores.reserve(m_NumCores);
        bool allGroupsRead = groupCount > 0;

        for (WORD group = 0; group < groupCount; ++group)
        {
            if (group >= m_GroupFirstCoreIds.size())
            {
                // A group the boot-time table does not cover has no stable ids; treat the sample
                // as a failed group read rather than number its processors ad hoc.
                allGroupsRead = false;
                break;
            }
            USHORT groupNumber = group;
            std::vector<ProcessorPerformanceInfo> perfInfo(std::max<DWORD>(GetActiveProcessorCount(group), 1));
            ULONG returnLength = 0;
            const NTSTATUS status = queryProcessorPerformance(
                perfInfo,
                returnLength,
                [&](PVOID data, ULONG length, PULONG returned)
                { return ntQueryEx(SystemProcessorPerformanceInformation, &groupNumber, sizeof(groupNumber), data, length, returned); });
            if (status != 0) // STATUS_SUCCESS = 0
            {
                spdlog::debug("NtQuerySystemInformationEx failed for processor group {}: 0x{:08X}", group, status);
                allGroupsRead = false;
                break;
            }
            appendProcessorGroup(
                cores, std::span<const ProcessorPerformanceInfo>(perfInfo), returnLength, m_GroupFirstCoreIds[group], toCounters);
        }

        if (allGroupsRead)
        {
            counters.cpuPerCore = std::move(cores);
            if (multiGroup)
            {
                // Total from the same all-group counters as the per-core grid (#1107).
                counters.cpuTotal = multiGroupTotal(sumCpuCounters(counters.cpuPerCore), m_LastAllGroupTotal);
            }
            spdlog::trace("Read per-core CPU for {} cores in {} processor groups", counters.cpuPerCore.size(), groupCount);
            return;
        }
    }

    if (multiGroup)
    {
        // A group query failed. The one-group fallback below would put the calling thread's group
        // into the slots of group 0 (and whichever came before it), and SystemModel matches cores
        // by position, so failure and recovery samples would compare different CPUs. Report no
        // per-core data this sample, and never a one-group Total: the last all-group one, or a
        // zeroed one before the first complete read (see multiGroupTotal) (#1107).
        counters.cpuTotal = multiGroupTotal(std::nullopt, m_LastAllGroupTotal);
        spdlog::debug("Per-core CPU unavailable this sample: a processor group query failed");
        return;
    }

    // Fallback without NtQuerySystemInformationEx on a single-group machine.
    auto ntQuery = Windows::ntQuerySystemInformation();
    if (ntQuery == nullptr)
    {
        spdlog::warn("NtQuerySystemInformation not available, per-core CPU disabled");
        return;
    }

    std::vector<ProcessorPerformanceInfo> perfInfo(std::max<std::size_t>(m_NumCores, 1));
    ULONG returnLength = 0;
    const NTSTATUS status = queryProcessorPerformance(perfInfo,
                                                      returnLength,
                                                      [&](PVOID data, ULONG length, PULONG returned)
                                                      { return ntQuery(SystemProcessorPerformanceInformation, data, length, returned); });

    if (status != 0) // STATUS_SUCCESS = 0
    {
        // NTSTATUS is a signed integral type; formatting with {:X} prints the underlying bit pattern in hex.
        spdlog::error("NtQuerySystemInformation failed: 0x{:08X}", status);
        return;
    }

    const std::size_t coresReturned =
        appendProcessorGroup(counters.cpuPerCore, std::span<const ProcessorPerformanceInfo>(perfInfo), returnLength, 0, toCounters);
    spdlog::trace("Read per-core CPU for {} cores", coresReturned);
}

void WindowsSystemProbe::readMemoryCounters(SystemCounters& counters)
{
    MEMORYSTATUSEX memStatus{};
    memStatus.dwLength = sizeof(memStatus);

    if (GlobalMemoryStatusEx(&memStatus) == 0)
    {
        spdlog::error("GlobalMemoryStatusEx failed: {}", GetLastError());
        return;
    }

    counters.memory.totalBytes = memStatus.ullTotalPhys;
    counters.memory.freeBytes = memStatus.ullAvailPhys;
    counters.memory.availableBytes = memStatus.ullAvailPhys;
    counters.memory.hasAvailableBytes = true;

    // Cached: the system cache, i.e. the standby list plus the system working set (#1027). That is
    // the memory Windows holds as file cache and gives back on demand, the closest analogue of
    // Linux's page cache, and what Task Manager calls "Cached". It is already inside "available",
    // so Used (total - available) is unchanged. GetPerformanceInfo needs no privilege, unlike the
    // per-list breakdown (SystemMemoryListInformation).
    PERFORMANCE_INFORMATION perfInfo{};
    perfInfo.cb = sizeof(perfInfo);
    if (GetPerformanceInfo(&perfInfo, sizeof(perfInfo)) != 0)
    {
        counters.memory.cachedBytes = static_cast<uint64_t>(perfInfo.SystemCache) * static_cast<uint64_t>(perfInfo.PageSize);
    }

    // Swap: the page files' own sizes (#1026). The commit figures in MEMORYSTATUSEX describe RAM
    // plus page file, and the "free page file" derived from them underflowed whenever the
    // remaining commit was below the available RAM, which pinned swap at ~100 %.
    const SwapBytes swap = readSwap();
    counters.memory.swapTotalBytes = swap.totalBytes;
    counters.memory.swapFreeBytes = swap.freeBytes;
}

SwapBytes WindowsSystemProbe::readSwap()
{
    const auto ntQuerySystemInformation = Windows::ntQuerySystemInformation();
    if (ntQuerySystemInformation == nullptr)
    {
        return {};
    }

    SYSTEM_INFO sysInfo{};
    GetSystemInfo(&sysInfo);

    // One entry is 32 bytes plus its file name; 4 KiB holds dozens of page files. Grow once if the
    // OS says it needs more.
    std::vector<std::byte> buffer(4096);
    ULONG returnLength = 0;
    NTSTATUS status =
        ntQuerySystemInformation(SystemPageFileInformationClass, buffer.data(), static_cast<ULONG>(buffer.size()), &returnLength);
    if (status == STATUS_INFO_LENGTH_MISMATCH_VALUE && returnLength > buffer.size())
    {
        buffer.resize(returnLength);
        status = ntQuerySystemInformation(SystemPageFileInformationClass, buffer.data(), static_cast<ULONG>(buffer.size()), &returnLength);
    }
    if (status < 0)
    {
        spdlog::debug("NtQuerySystemInformation(SystemPageFileInformation) failed: 0x{:x}", static_cast<unsigned long>(status));
        return {};
    }

    const std::size_t used = std::min<std::size_t>(returnLength, buffer.size());
    const auto totals = sumPageFiles(std::span<const std::byte>(buffer.data(), used));
    // No page file at all is a valid answer: swap is then zero, as it should read.
    return totals.has_value() ? swapFromPageFiles(*totals, sysInfo.dwPageSize) : SwapBytes{};
}

void WindowsSystemProbe::readUptime(SystemCounters& counters)
{
    // GetTickCount64 returns milliseconds since system start
    const uint64_t uptimeMs = GetTickCount64();
    counters.uptimeSeconds = uptimeMs / 1000;

    // Calculate boot timestamp
    auto now = std::chrono::system_clock::now();
    auto nowEpoch = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
    const uint64_t nowEpochSeconds = toU64NonNegative(nowEpoch);
    counters.bootTimestamp = (nowEpochSeconds > counters.uptimeSeconds) ? (nowEpochSeconds - counters.uptimeSeconds) : 0ULL;
}

void WindowsSystemProbe::readStaticInfo(SystemCounters& counters) const
{
    counters.hostname = m_Hostname;
    counters.cpuModel = m_CpuModel;
    counters.cpuCoreCount = m_NumCores;
    counters.cpuDetails = m_CpuDetails.details;
}

void WindowsSystemProbe::refreshCpuDetailsIfProcessorsChanged(std::span<const CpuCounters> perCore)
{
    const auto sampledIds = perCore | std::views::transform(&CpuCounters::coreId);
    if (CpuTopology::cpuDetailsNeedRead(m_CpuDetails, sampledIds))
    {
        // Rare: only after the set of active processors changes (a hot-add), never every sample
        std::vector<std::size_t> describedIds;
        CpuDetails fresh = readCpuDetails(m_GroupFirstCoreIds, describedIds);
        (void) CpuTopology::commitIfConsistent(sampledIds, std::move(describedIds), std::move(fresh), m_CpuDetails);
    }
    // cpuCoreCount and the fallback per-core buffer follow the processors actually sampled, so
    // cpuPerCore.size() keeps matching cpuCoreCount after a hot-add. A failed read (none) keeps both.
    if (!perCore.empty())
    {
        m_NumCores = perCore.size();
    }
}

void WindowsSystemProbe::readCpuFreq(SystemCounters& counters)
{
    // The current clock, as Linux reports it under the same hasCpuFreq: the base clock scaled by
    // "% Processor Performance" (#1184). The base clock alone until PDH has a rate, or without PDH.
    const std::optional<double> performancePercent =
        (m_ProcessorPerformance != nullptr) ? m_ProcessorPerformance->read() : std::optional<double>{};
    counters.cpuFreqMHz = currentCpuFrequencyMHz(m_BaseCpuMHz, performancePercent);
    // Load average is not available on Windows (leave at 0)
}

SystemCapabilities WindowsSystemProbe::capabilities() const
{
    return SystemCapabilities{
        .hasPerCoreCpu = true, // Via NtQuerySystemInformation
        .hasMemoryAvailable = true,
        .hasSwap = true,
        .hasUptime = true,
        .hasIoWait = false,            // Windows doesn't expose iowait
        .hasSteal = false,             // Windows doesn't expose steal time
        .hasLoadAvg = false,           // Windows doesn't have load average
        .hasCpuFreq = true,            // Current clock: rated base x % Processor Performance (#1184, #1530)
        .hasNetworkCounters = true,    // Via GetIfTable2 (64-bit counters, Unicode names)
        .hasVirtualizationInfo = true, // Firmware/SLAT/hypervisor/VBS status (#809)
    };
}

long WindowsSystemProbe::ticksPerSecond() const
{
    // Windows FILETIME uses 100-nanosecond intervals
    return 10'000'000L;
}

// isCountedNetworkRow() spells the ifType values out to stay <windows.h>-free (#1257).
static_assert(IF_TYPE_WWAN_GSM == IF_TYPE_WWANPP);
static_assert(IF_TYPE_WWAN_CDMA == IF_TYPE_WWANPP2);
// As are isNotPresentNetworkRow()'s and isHardwareNetworkRow()'s (#1284).
static_assert(NDIS_PHYSICAL_MEDIUM_BLUETOOTH == NdisPhysicalMediumBluetooth);
static_assert(IF_OPER_STATUS_UP == IfOperStatusUp);
static_assert(IF_OPER_STATUS_NOT_PRESENT == IfOperStatusNotPresent);

namespace
{
// The Network class's per-interface keys: <class>\{interface GUID}\Connection holds PnPInstanceId,
// the device instance id of the adapter behind the interface (#1284).
constexpr const wchar_t* NETWORK_CLASS_KEY = L"SYSTEM\\CurrentControlSet\\Control\\Network\\{4D36E972-E325-11CE-BFC1-08002BE10318}\\";
constexpr const wchar_t* PNP_INSTANCE_ID_VALUE = L"PnPInstanceId";

[[nodiscard]] std::wstring guidText(const GUID& guid)
{
    return std::format(L"{{{:08X}-{:04X}-{:04X}-{:02X}{:02X}-{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}}}",
                       guid.Data1,
                       guid.Data2,
                       guid.Data3,
                       guid.Data4[0],
                       guid.Data4[1],
                       guid.Data4[2],
                       guid.Data4[3],
                       guid.Data4[4],
                       guid.Data4[5],
                       guid.Data4[6],
                       guid.Data4[7]);
}

// The adapter's PnP device instance id from the registry, or empty when the interface has none
// (Teredo, 6to4) or it cannot be read.
[[nodiscard]] std::wstring readAdapterDeviceInstanceId(const GUID& interfaceGuid)
{
    const std::wstring keyPath = std::wstring(NETWORK_CLASS_KEY) + guidText(interfaceGuid) + L"\\Connection";
    DWORD bytes = 0;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, keyPath.c_str(), PNP_INSTANCE_ID_VALUE, RRF_RT_REG_SZ, nullptr, nullptr, &bytes) !=
            ERROR_SUCCESS ||
        bytes < sizeof(wchar_t))
    {
        return {};
    }
    std::wstring id(bytes / sizeof(wchar_t), L'\0');
    if (RegGetValueW(HKEY_LOCAL_MACHINE, keyPath.c_str(), PNP_INSTANCE_ID_VALUE, RRF_RT_REG_SZ, nullptr, id.data(), &bytes) !=
        ERROR_SUCCESS)
    {
        return {};
    }
    id.resize(std::wcslen(id.c_str())); // Drop the terminator RegGetValueW wrote
    return id;
}

// Whether the device with this instance id is present: CM_LOCATE_DEVNODE_NORMAL finds only a device
// node that is in the system (started, disabled or failed); a removed device's is not found.
[[nodiscard]] DevicePresence devicePresence(const std::wstring& deviceInstanceId)
{
    if (deviceInstanceId.empty())
    {
        return DevicePresence::Unknown;
    }
    std::wstring id = deviceInstanceId; // CM_Locate_DevNodeW takes a non-const id
    DEVINST devNode = 0;
    switch (CM_Locate_DevNodeW(&devNode, id.data(), CM_LOCATE_DEVNODE_NORMAL))
    {
    case CR_SUCCESS:
        return DevicePresence::Present;
    case CR_NO_SUCH_DEVNODE:
        return DevicePresence::Absent;
    default:
        return DevicePresence::Unknown;
    }
}

// How long an empty device-id read waits before the registry is asked again (#1369 review).
constexpr std::chrono::seconds ADAPTER_ID_RETRY_INTERVAL{30};

// The adapter's device instance id, from @p cache or the registry. A found id is kept for good (an
// interface's adapter never changes); an empty read is not taken as final -- the value may not be
// written yet, or the read failed -- and is retried after ADAPTER_ID_RETRY_INTERVAL, so a phantom
// adapter first seen too early is still recognised later rather than listed until restart.
template<typename Cache>
[[nodiscard]] const std::wstring& adapterDeviceInstanceId(Cache& cache, std::uint64_t interfaceLuid, const GUID& interfaceGuid)
{
    const auto now = std::chrono::steady_clock::now();
    std::array<std::uint64_t, 2> guidHalves{};
    static_assert(sizeof(guidHalves) == sizeof(GUID));
    std::memcpy(guidHalves.data(), &interfaceGuid, sizeof(GUID));
    auto& entry = cache[interfaceLuid];
    // Windows can give a freed LUID to a later interface: a different GUID is a different adapter,
    // whose id is read afresh rather than inherited (#1369 review).
    if (entry.guidLow != guidHalves[0] || entry.guidHigh != guidHalves[1])
    {
        entry = {};
        entry.guidLow = guidHalves[0];
        entry.guidHigh = guidHalves[1];
    }
    if (entry.id.empty() && now >= entry.retryAt)
    {
        entry.id = readAdapterDeviceInstanceId(interfaceGuid);
        entry.retryAt = now + ADAPTER_ID_RETRY_INTERVAL;
    }
    return entry.id;
}
} // namespace

void WindowsSystemProbe::readNetworkCounters(SystemCounters& counters)
{
    // Use GetIfTable2 for 64-bit counters and proper Unicode interface names.
    // GetIfTable2 allocates the buffer internally; we must free it with FreeMibTable.
    // Available since Windows Vista/Server 2008.
    MIB_IF_TABLE2* rawTable = nullptr;
    const DWORD status = GetIfTable2(&rawTable);
    if (status != NO_ERROR || rawTable == nullptr)
    {
        spdlog::warn("GetIfTable2 failed: {}", status);
        return;
    }
    // Owned from here, so an exception thrown while the rows are read (the device-id cache and
    // registry lookups allocate) still frees it; the sampler catches and carries on, so a raw
    // pointer leaked a table on every failed sample (#1369 review).
    const auto freeTable = [](MIB_IF_TABLE2* t) noexcept
    {
        FreeMibTable(t);
    };
    const std::unique_ptr<MIB_IF_TABLE2, decltype(freeTable)> table(rawTable, freeTable);

    for (ULONG i = 0; i < table->NumEntries; ++i)
    {
        const MIB_IF_ROW2& row = table->Table[i];

        // Loopback, non-network types and NDIS filter-module rows are not interfaces of their own
        // (#1030); see isCountedNetworkRow().
        if (!isCountedNetworkRow(row.Type, row.InterfaceAndOperStatusFlags.FilterInterface != 0))
        {
            continue;
        }
        // Adapters removed from the system stay listed; they carry nothing (#1284). A down row's
        // device is looked up: down alone doesn't mean removed.
        const DevicePresence presence =
            needsDevicePresence(row.OperStatus)
                ? devicePresence(adapterDeviceInstanceId(m_AdapterDeviceInstanceIds, row.InterfaceLuid.Value, row.InterfaceGuid))
                : DevicePresence::Unknown;
        if (isNotPresentNetworkRow(row.OperStatus, presence))
        {
            continue;
        }
        // 64-bit byte counters - no more 32-bit overflow issues
        const uint64_t rxBytes = row.InOctets;
        const uint64_t txBytes = row.OutOctets;

        // Store per-interface data
        SystemCounters::InterfaceCounters ifaceCounters;

        // MIB_IF_ROW2 provides proper Unicode strings:
        // - Alias: friendly name (e.g., "Wi-Fi", "Ethernet")
        // - Description: full adapter description (e.g., "Intel(R) Wi-Fi 6 AX201 160MHz")
        // Fallback chain for name: Alias -> Description -> Interface index
        const std::string alias = WinString::wideToUtf8(row.Alias);
        const std::string description = WinString::wideToUtf8(row.Description);

        if (!alias.empty())
        {
            ifaceCounters.name = alias;
        }
        else if (!description.empty())
        {
            ifaceCounters.name = description;
        }
        else
        {
            ifaceCounters.name = std::format("Interface {}", row.InterfaceIndex);
        }

        // Display name: prefer Description, fall back to name
        ifaceCounters.displayName = description.empty() ? ifaceCounters.name : description;

        ifaceCounters.rxBytes = rxBytes;
        ifaceCounters.txBytes = txBytes;

        // IF_OPER_STATUS enum - IfOperStatusUp (1) means interface is operational
        ifaceCounters.isUp = (row.OperStatus == IfOperStatusUp);

        // A software interface -- VPN tunnel, Hyper-V/WSL vEthernet, WAN Miniport -- carries traffic
        // that also crosses a hardware adapter, so the Total leaves it out (#1257, see
        // sumCountedInterfaces()). A Bluetooth PAN link is hardware although its flag is clear (#1284).
        const bool hardware =
            isHardwareNetworkRow(row.InterfaceAndOperStatusFlags.HardwareInterface != 0, row.Type, row.PhysicalMediumType);
        ifaceCounters.isVirtual = !hardware;
        ifaceCounters.isVirtualKnown = true; // Every MIB_IF_ROW2 carries the flag (#1260)

        // 64-bit link speeds in bits/sec - convert to Mbps
        // Use transmit speed (receive speed may differ on asymmetric links)
        // Windows uses 0 or ULONG64_MAX to indicate unknown speed
        constexpr uint64_t WINDOWS_UNKNOWN_LINK_SPEED = std::numeric_limits<uint64_t>::max();
        if (row.TransmitLinkSpeed == 0 || row.TransmitLinkSpeed == WINDOWS_UNKNOWN_LINK_SPEED)
        {
            ifaceCounters.linkSpeedMbps = 0;
        }
        else
        {
            ifaceCounters.linkSpeedMbps = row.TransmitLinkSpeed / 1'000'000ULL;
        }

        counters.networkInterfaces.push_back(std::move(ifaceCounters));
    }

    // Hardware interfaces only, unless there are none -- as on Linux and in SystemModel (#1257).
    const NetworkTotals totals = sumCountedInterfaces(counters.networkInterfaces);
    counters.netRxBytes = totals.rxBytes;
    counters.netTxBytes = totals.txBytes;
}

} // namespace Platform
