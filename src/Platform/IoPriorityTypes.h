#pragma once

// A process's I/O priority (#803, #3362): its class and level. Shared by the process actions that read and set it
// (IProcessActions) and the sampled counters that carry it for the Processes table's I/O Priority column
// (ProcessCounters::ioPriority, #3551), as SchedPolicy.h is for the CPU scheduling policy.

#include <cstdint>
#include <optional>

namespace Platform
{

/// A process's I/O priority class. None to Idle are Linux's I/O scheduling classes (ioprio_set(2)), with
/// the kernel's own numbering (IOPRIO_CLASS_*); WindowsHint is Windows' per-process I/O priority, which
/// has no classes, only one hint (#3362).
enum class IoPriorityClass : std::uint8_t
{
    None = 0,       ///< Never set: the kernel derives a best-effort level from the nice value.
    Realtime = 1,   ///< Served before every other class; setting it needs CAP_SYS_NICE (or CAP_SYS_ADMIN).
    BestEffort = 2, ///< The normal class, with levels 0 (highest) to 7 (lowest).
    Idle = 3,       ///< Served only when no other process wants the disk.
    /// Windows' I/O priority hint (ProcessIoPriority, #3362): the level is the IoPriorityHint, as Windows
    /// numbers it (higher is more urgent, unlike Linux's levels). Only Windows reads or sets it.
    WindowsHint = 4,
};

/// Windows' I/O priority hints (IO_PRIORITY_HINT, #3362), as Windows numbers them: the level of an
/// IoPriority whose class is WindowsHint. Critical is the memory manager's own, never set by TaskSmack.
enum class IoPriorityHint : std::uint8_t
{
    VeryLow = 0,  ///< Background work: served after everything else
    Low = 1,      ///< Below normal
    Normal = 2,   ///< Every process's default
    High = 3,     ///< Setting it needs SeIncreaseBasePriorityPrivilege (an administrator)
    Critical = 4, ///< The system's own; read, never set
};

/// The hint for a WindowsHint level, or nullopt for a value Windows doesn't define.
[[nodiscard]] constexpr std::optional<IoPriorityHint> ioPriorityHintFromLevel(std::int32_t level) noexcept
{
    if (level < static_cast<std::int32_t>(IoPriorityHint::VeryLow) || level > static_cast<std::int32_t>(IoPriorityHint::Critical))
    {
        return std::nullopt;
    }
    return static_cast<IoPriorityHint>(level);
}

/// A process's I/O priority: its class, and its level within Realtime or BestEffort (0 is highest,
/// 7 lowest), or for WindowsHint the IoPriorityHint. The level means nothing for None and Idle, and is
/// 0 there.
struct IoPriority
{
    IoPriorityClass ioClass = IoPriorityClass::None;
    std::int32_t level = 0;

    [[nodiscard]] constexpr bool operator==(const IoPriority&) const noexcept = default;
};

} // namespace Platform
