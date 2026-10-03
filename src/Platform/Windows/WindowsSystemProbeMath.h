#pragma once

// Pure arithmetic extracted from WindowsSystemProbe so it can be unit-tested with fabricated
// values, without depending on what this machine's page files or processors happen to report.

#include "Platform/SystemTypes.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>

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

} // namespace Platform
