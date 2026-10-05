#pragma once

// Pure arithmetic extracted from WindowsSystemProbe so it can be unit-tested with fabricated
// values, without depending on what this machine's page files or processors happen to report.

#include "Platform/SystemTypes.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <vector>

namespace Platform
{

/// Page-file sizes summed over every page file, in pages.
struct PageFileTotals
{
    std::uint64_t totalPages = 0;
    std::uint64_t inUsePages = 0;
};

/// Byte offsets within one SYSTEM_PAGEFILE_INFORMATION entry, as NtQuerySystemInformation
/// (class SystemPageFileInformation) writes it: NextEntryOffset, TotalSize, TotalInUse,
/// PeakUsage (all ULONG), then the file name. Only the first three fields are read.
inline constexpr std::size_t PAGEFILE_ENTRY_NEXT_OFFSET = 0;
inline constexpr std::size_t PAGEFILE_ENTRY_TOTAL_SIZE = 4;
inline constexpr std::size_t PAGEFILE_ENTRY_TOTAL_IN_USE = 8;
inline constexpr std::size_t PAGEFILE_ENTRY_MIN_BYTES = 16;

/// Sum the page files described by a SystemPageFileInformation buffer.
///
/// Swap used to be derived from the commit figures (ullTotalPageFile/ullAvailPageFile), which
/// describe RAM *plus* page file. The "free page file" figure taken from them underflowed
/// whenever the remaining commit was smaller than the available RAM, which on a normal system
/// is most of the time, so swap read ~100 % (#1026). The page files' own sizes are exact.
///
/// The entries form a chain linked by NextEntryOffset (0 ends it). A link that does not move
/// forward, or an entry that would run past the buffer, ends the walk: the buffer comes from the
/// OS, but a malformed one must not make this loop or read out of bounds.
///
/// @param buffer  The bytes NtQuerySystemInformation returned (returnLength of them).
/// @return The totals, or nullopt if the buffer does not hold even one complete entry.
[[nodiscard]] inline std::optional<PageFileTotals> sumPageFiles(std::span<const std::byte> buffer) noexcept
{
    const auto readU32 = [&buffer](std::size_t offset) noexcept
    {
        std::uint32_t value = 0;
        std::memcpy(&value, buffer.data() + offset, sizeof(value));
        return value;
    };

    if (buffer.size() < PAGEFILE_ENTRY_MIN_BYTES)
    {
        return std::nullopt;
    }

    PageFileTotals totals;
    std::size_t offset = 0;
    while (offset + PAGEFILE_ENTRY_MIN_BYTES <= buffer.size())
    {
        totals.totalPages += readU32(offset + PAGEFILE_ENTRY_TOTAL_SIZE);
        totals.inUsePages += readU32(offset + PAGEFILE_ENTRY_TOTAL_IN_USE);

        const std::uint32_t next = readU32(offset + PAGEFILE_ENTRY_NEXT_OFFSET);
        if (next == 0)
        {
            break;
        }
        // NextEntryOffset is relative to this entry and always at least one entry long.
        if (next < PAGEFILE_ENTRY_MIN_BYTES)
        {
            break;
        }
        offset += next;
    }
    return totals;
}

/// Swap figures in bytes from page-file totals, with "in use" capped at the size.
struct SwapBytes
{
    std::uint64_t totalBytes = 0;
    std::uint64_t freeBytes = 0;
};

[[nodiscard]] constexpr SwapBytes swapFromPageFiles(const PageFileTotals& totals, std::uint64_t pageSizeBytes) noexcept
{
    const std::uint64_t inUse = (totals.inUsePages < totals.totalPages) ? totals.inUsePages : totals.totalPages;
    return {
        .totalBytes = totals.totalPages * pageSizeBytes,
        .freeBytes = (totals.totalPages - inUse) * pageSizeBytes,
    };
}

/// One processor's times from SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION, as CpuCounters.
///
/// Windows reports KernelTime *including* idle, and DPC and interrupt time *inside* kernel time.
/// CpuCounters::active() adds system + irq + softirq, so irq and softirq must come out of system
/// or they are counted twice: every core read busier than it was, and the per-core grid did not
/// average to the Total figure, whose GetSystemTimes source has no such split (#1032).
///
/// All inputs are 100 ns ticks. Each subtraction is clamped: the counters are sampled together
/// but not atomically, so a component can momentarily exceed the total it belongs to.
[[nodiscard]] constexpr CpuCounters
processorTimes(std::uint64_t kernel, std::uint64_t idle, std::uint64_t user, std::uint64_t dpc, std::uint64_t interrupt) noexcept
{
    const std::uint64_t kernelBusy = (kernel > idle) ? (kernel - idle) : 0;
    const std::uint64_t irq = (interrupt < kernelBusy) ? interrupt : kernelBusy;
    const std::uint64_t softirq = (dpc < kernelBusy - irq) ? dpc : (kernelBusy - irq);

    CpuCounters core{};
    core.user = user;
    core.idle = idle;
    core.irq = irq;
    core.softirq = softirq;
    core.system = kernelBusy - irq - softirq;
    return core;
}

/// Append one processor group's SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION entries to `cores`.
///
/// Without NtQuerySystemInformationEx, SystemProcessorPerformanceInformation reports only the
/// calling thread's processor group, so a machine with more than 64 logical processors showed
/// one group's cores and core count (#1107). The probe now queries each group in turn and
/// appends its entries here, keeping group order so core indices match the OS's numbering.
///
/// @param cores        Per-core counters gathered so far; this group's are appended.
/// @param buffer       The entries the query was given room for.
/// @param returnBytes  The ReturnLength the query reported. Fewer bytes than the buffer (a group
///                     with fewer processors, or one that shrank) appends only that many entries;
///                     more is clamped to the buffer so a bad length cannot read past it.
/// @param firstCoreId  The coreId of this group's processor 0 (see processorGroupFirstCoreIds()).
///                     Each entry's coreId is firstCoreId plus its index in the group, so it does
///                     not depend on how many entries earlier groups returned this sample.
/// @param convert      Turns one entry into CpuCounters (processorTimes() in the probe).
/// @return How many entries were appended.
template<typename Entry, typename Convert>
std::size_t appendProcessorGroup(
    std::vector<CpuCounters>& cores, std::span<const Entry> buffer, std::size_t returnBytes, std::size_t firstCoreId, Convert convert)
{
    const std::size_t returned = std::min(returnBytes / sizeof(Entry), buffer.size());
    cores.reserve(cores.size() + returned);
    for (std::size_t i = 0; i < returned; ++i)
    {
        CpuCounters core = convert(buffer[i]);
        // A stable identity: SystemModel matches per-core history by it (#1229), so it must not
        // shift when an earlier group's count changes, and it is unique across groups (#1107).
        core.coreId = firstCoreId + i;
        cores.push_back(core);
    }
    return returned;
}

/// The coreId of each processor group's first processor (#1107): the sum of the earlier groups'
/// *maximum* processor counts (GetMaximumProcessorCount, fixed for the boot session). The active
/// count can change at runtime -- a hot-added processor -- and numbering by it would renumber every
/// later group's processors, so SystemModel would compare one CPU's counters with another's. The
/// ids have no gaps unless a group has room for processors not yet added.
[[nodiscard]] inline std::vector<std::size_t> processorGroupFirstCoreIds(std::span<const std::uint32_t> maximumPerGroup)
{
    std::vector<std::size_t> firstIds;
    firstIds.reserve(maximumPerGroup.size());
    std::size_t next = 0;
    for (const std::uint32_t maximum : maximumPerGroup)
    {
        firstIds.push_back(next);
        next += maximum;
    }
    return firstIds;
}

/// The machine-wide CPU counters as the sum of per-core ones (#1107). On a machine with several
/// processor groups the probe takes Total from every group's cores, so Total and the per-core grid
/// come from the same counters: GetSystemTimes is not documented to cover every group.
[[nodiscard]] inline CpuCounters sumCpuCounters(std::span<const CpuCounters> cores) noexcept
{
    CpuCounters total;
    for (const CpuCounters& core : cores)
    {
        total.user += core.user;
        total.nice += core.nice;
        total.system += core.system;
        total.idle += core.idle;
        total.iowait += core.iowait;
        total.irq += core.irq;
        total.softirq += core.softirq;
        total.steal += core.steal;
        total.guest += core.guest;
        total.guestNice += core.guestNice;
    }
    return total;
}

/// The Total a multi-group machine reports this sample (#1107): the all-group sum when every group
/// was read, otherwise the last one. Before the first complete read there is no all-group Total, and
/// reporting a one-group counter instead would become SystemModel's baseline and be compared with
/// the all-group sum on recovery, so the other groups' lifetime counters would land in one interval
/// as a spike. A zeroed Total is reported until then: the model sees no change, and the first
/// complete read measures from zero (the average since boot) rather than spiking.
[[nodiscard]] inline CpuCounters multiGroupTotal(const std::optional<CpuCounters>& allGroupNow,
                                                 std::optional<CpuCounters>& lastAllGroup) noexcept
{
    if (allGroupNow.has_value())
    {
        lastAllGroup = allGroupNow;
        return *allGroupNow;
    }
    return lastAllGroup.value_or(CpuCounters{});
}

/// IANA ifType values GetIfTable2 reports (ipifcons.h), spelled out so this header stays free of
/// Windows includes.
inline constexpr std::uint32_t IF_TYPE_ETHERNET = 6;
inline constexpr std::uint32_t IF_TYPE_PPP_LINK = 23;
inline constexpr std::uint32_t IF_TYPE_LOOPBACK = 24;
inline constexpr std::uint32_t IF_TYPE_VIRTUAL = 53;
inline constexpr std::uint32_t IF_TYPE_WIFI = 71;
inline constexpr std::uint32_t IF_TYPE_TUNNEL_LINK = 131;
inline constexpr std::uint32_t IF_TYPE_WWAN_GSM = 243;  // Mobile broadband, GSM-based (#1257)
inline constexpr std::uint32_t IF_TYPE_WWAN_CDMA = 244; // Mobile broadband, CDMA-based (#1257)

/// Whether a GetIfTable2 row counts as a network interface of its own.
///
/// Ethernet, Wi-Fi, mobile broadband (WWAN), tunnels, PPP and virtual adapters (VPN, Hyper-V, Docker)
/// count; loopback and other types (Bluetooth, etc.) do not. A WWAN modem is the only uplink on some
/// laptops, so leaving its types out left them with no network Total at all (#1257). Nor do NDIS filter-module rows: GetIfTable2 lists one
/// per filter bound to an adapter (WFP MAC layer, QoS Packet Scheduler, Native WiFi filter, Hyper-V switch extensions), each repeating its
/// adapter's byte counters. Counting them made the network Total several times the real traffic -- on a Wi-Fi laptop with WSL, Wi-Fi was
/// counted 5 times and the WSL vEthernet adapter 4 times, 49.0 GB of lifetime bytes against 11.8 GB actual (#1030).
///
/// @param ifType             MIB_IF_ROW2::Type.
/// @param isFilterInterface  MIB_IF_ROW2::InterfaceAndOperStatusFlags.FilterInterface.
[[nodiscard]] constexpr bool isCountedNetworkRow(std::uint32_t ifType, bool isFilterInterface) noexcept
{
    if (isFilterInterface)
    {
        return false;
    }
    return ifType == IF_TYPE_ETHERNET || ifType == IF_TYPE_WIFI || ifType == IF_TYPE_TUNNEL_LINK || ifType == IF_TYPE_PPP_LINK ||
           ifType == IF_TYPE_VIRTUAL || ifType == IF_TYPE_WWAN_GSM || ifType == IF_TYPE_WWAN_CDMA;
}

/// Cumulative bytes over the interfaces the network Total counts (#1257).
struct NetworkTotals
{
    std::uint64_t rxBytes = 0;
    std::uint64_t txBytes = 0;
};

/// The network Total from the counted interfaces: hardware ones only, unless none is listed (#1257).
///
/// Traffic over a VPN tunnel, a Hyper-V/WSL vEthernet adapter or a WAN Miniport also crosses a
/// hardware adapter, so counting both doubled it. The probe marks a row virtual when
/// MIB_IF_ROW2::InterfaceAndOperStatusFlags.HardwareInterface is clear. With no hardware interface
/// at all every interface counts, so the Total isn't 0. Same rule as the Linux probe and
/// SystemModel's Total rate (#1106); keep them in step.
[[nodiscard]] inline NetworkTotals sumCountedInterfaces(std::span<const SystemCounters::InterfaceCounters> interfaces) noexcept
{
    const bool anyHardware = std::ranges::any_of(interfaces, [](const auto& iface) { return !iface.isVirtual; });
    NetworkTotals totals;
    for (const auto& iface : interfaces)
    {
        if (!anyHardware || !iface.isVirtual)
        {
            totals.rxBytes += iface.rxBytes;
            totals.txBytes += iface.txBytes;
        }
    }
    return totals;
}

} // namespace Platform
