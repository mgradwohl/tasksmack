#pragma once

// Pure arithmetic extracted from WindowsSystemProbe so it can be unit-tested with fabricated
// values, without depending on what this machine's page files or processors happen to report.

#include "Platform/SystemTypes.h"

#include <algorithm>
#include <cmath>
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

/// MIB_IF_ROW2 values spelled out, like the ifTypes above, to keep this header free of Windows includes.
inline constexpr std::uint32_t NDIS_PHYSICAL_MEDIUM_BLUETOOTH = 10; // NdisPhysicalMediumBluetooth (#1284)
inline constexpr std::uint32_t IF_OPER_STATUS_UP = 1;               // IfOperStatusUp (#1284)
inline constexpr std::uint32_t IF_OPER_STATUS_NOT_PRESENT = 6;      // IfOperStatusNotPresent (#1284)

/// Whether the PnP device behind a network interface is in the system, as the configuration manager
/// reports it (#1284).
enum class DevicePresence : std::uint8_t
{
    /// Not asked, or not known: the interface has no PnP device (Teredo, 6to4), or the query failed.
    Unknown,
    /// The device node exists -- whether it is started, disabled or failed.
    Present,
    /// No device node: the device was removed (a phantom in Device Manager).
    Absent,
};

/// Whether the probe should ask whether a row's device is present: only for a row that is not up and
/// not already reported IfOperStatusNotPresent (#1284). An up interface's device is plainly present,
/// and every other row would cost a configuration-manager query each sample for nothing.
///
/// @param operStatus  MIB_IF_ROW2::OperStatus.
[[nodiscard]] constexpr bool needsDevicePresence(std::uint32_t operStatus) noexcept
{
    return operStatus != IF_OPER_STATUS_UP && operStatus != IF_OPER_STATUS_NOT_PRESENT;
}

/// Whether a GetIfTable2 row is an adapter that is no longer in the system (#1284).
///
/// Windows keeps listing removed adapters -- a USB Ethernet dongle or dock unplugged long ago, an
/// adapter whose driver was uninstalled. They carry no traffic and only cluttered the interface list,
/// so the probe leaves them out. Most report OperStatus IfOperStatusNotPresent, which decides on its
/// own. Some report IfOperStatusDown instead (a dock's USB Ethernet that Device Manager shows as a
/// phantom); for those the device's own presence decides, since a down adapter that is present --
/// unplugged cable, disabled in Windows, Wi-Fi with no network -- must stay listed.
///
/// @param operStatus  MIB_IF_ROW2::OperStatus.
/// @param presence    Whether the adapter's PnP device is present (Unknown when not asked).
[[nodiscard]] constexpr bool isNotPresentNetworkRow(std::uint32_t operStatus, DevicePresence presence) noexcept
{
    return operStatus == IF_OPER_STATUS_NOT_PRESENT || presence == DevicePresence::Absent;
}

/// Whether a counted GetIfTable2 row is a hardware link of its own rather than software whose traffic
/// also crosses a hardware adapter (#1257, #1284).
///
/// MIB_IF_ROW2::InterfaceAndOperStatusFlags.HardwareInterface decides, except for a Bluetooth
/// Personal Area Network adapter: Windows reports it as an Ethernet interface without the flag, yet a
/// phone tethered over Bluetooth is a link of its own, and with any other hardware adapter listed
/// (an idle Wi-Fi) its traffic was left out of the Total. It is recognised by its Bluetooth physical
/// medium, which no software adapter reports.
///
/// @param hardwareInterface   MIB_IF_ROW2::InterfaceAndOperStatusFlags.HardwareInterface.
/// @param ifType              MIB_IF_ROW2::Type.
/// @param physicalMediumType  MIB_IF_ROW2::PhysicalMediumType.
[[nodiscard]] constexpr bool isHardwareNetworkRow(bool hardwareInterface, std::uint32_t ifType, std::uint32_t physicalMediumType) noexcept
{
    return hardwareInterface || (ifType == IF_TYPE_ETHERNET && physicalMediumType == NDIS_PHYSICAL_MEDIUM_BLUETOOTH);
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
/// MIB_IF_ROW2::InterfaceAndOperStatusFlags.HardwareInterface is clear, unless it is a Bluetooth PAN
/// link (isHardwareNetworkRow(), #1284). A Wi-Fi adapter's extra ports ("Wi-Fi 2" to "Wi-Fi 5":
/// Wi-Fi Direct, hotspot, multi-link) report HardwareInterface and are counted like any hardware
/// link: their traffic is the machine's own radio traffic, and leaving it out would undercount. With no hardware interface
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

/// Highest "% Processor Performance" taken as a reading. Turbo runs a CPU above its base clock, so
/// readings over 100 are normal (a 2 GHz part boosting to 5.5 GHz reads 275); anything past this is
/// a garbage counter value, not a clock.
inline constexpr double MAX_PROCESSOR_PERFORMANCE_PERCENT = 1000.0;

/// The nominal (rated base) clock in MHz that "% Processor Performance" is a percentage of (#1530):
/// the highest PROCESSOR_POWER_INFORMATION::MaxMhz across the logical processors, as
/// CallNtPowerInformation(ProcessorInformation) reports them -- the figure Win32_Processor.MaxClockSpeed
/// and Task Manager's "Base speed" show. On a hybrid part each efficiency class has its own MaxMhz
/// (a Core Ultra 7 255H reports 2000 for its P-cores, 1500 for its E-cores and 700 for its LP E-cores);
/// the highest is the P-cores' rated base. The registry's ~MHz is not that figure on such parts (3686
/// on the 255H), so it stands in only when there is no MaxMhz to take.
/// @param processorMaxMHz Each logical processor's MaxMhz; empty when CallNtPowerInformation failed.
/// @param registryMHz     The ~MHz value under HARDWARE\DESCRIPTION\System\CentralProcessor\0; 0 = unknown.
/// @return The highest non-zero MaxMhz; registryMHz when there is none.
[[nodiscard]] inline std::uint64_t nominalCpuBaseMHz(std::span<const std::uint32_t> processorMaxMHz, std::uint64_t registryMHz) noexcept
{
    const std::uint32_t highest = processorMaxMHz.empty() ? 0U : std::ranges::max(processorMaxMHz);
    return (highest != 0U) ? highest : registryMHz;
}

/// The CPU's current clock in MHz, derived as Task Manager derives its "Speed" (#1184): the nominal
/// base clock (nominalCpuBaseMHz) scaled by PDH's "\Processor Information(_Total)\% Processor
/// Performance", the processors' average speed as a percentage of base. The base on its own is not
/// what Linux's cpufreq reports (the current clock) under the same hasCpuFreq.
/// @param baseMHz                     The nominal base clock from nominalCpuBaseMHz; 0 = unknown.
/// @param processorPerformancePercent The PDH reading, or nullopt when there is none yet (PDH needs two
///                                    samples for a rate) or the counter is unavailable.
/// @return The current clock; the base clock when there is no usable reading; 0 when the base is unknown.
[[nodiscard]] inline std::uint64_t currentCpuFrequencyMHz(std::uint64_t baseMHz, std::optional<double> processorPerformancePercent) noexcept
{
    if (baseMHz == 0 || !processorPerformancePercent.has_value())
    {
        return baseMHz;
    }
    const double percent = *processorPerformancePercent;
    if (!std::isfinite(percent) || percent <= 0.0 || percent > MAX_PROCESSOR_PERFORMANCE_PERCENT)
    {
        return baseMHz;
    }
    return static_cast<std::uint64_t>(std::llround((static_cast<double>(baseMHz) * percent) / 100.0));
}

} // namespace Platform
