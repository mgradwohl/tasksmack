#pragma once

// The ioprio value encoding behind LinuxProcessActions' I/O priority (#803), free of syscalls so it is
// unit-testable. There is no glibc wrapper for ioprio_set(2)/ioprio_get(2), and <linux/ioprio.h> is
// not on every supported system, so the layout is spelled out here with names that cannot collide
// with that header's IOPRIO_* macros.

#include "Domain/PriorityConfig.h"
#include "Platform/IProcessActions.h"

#include <optional>

namespace Platform::IoPrio
{

/// IOPRIO_WHO_PROCESS: the `who` argument naming one process or thread by ID.
inline constexpr int WHO_PROCESS = 1;

/// The class sits in bits 13-15 (IOPRIO_CLASS_SHIFT).
inline constexpr int CLASS_SHIFT = 13;

/// The level sits in bits 0-2 (IOPRIO_PRIO_LEVEL_MASK). Since Linux 6.5, bits 3-12 hold hints, which
/// a decode ignores and an encode leaves 0.
inline constexpr int LEVEL_MASK = 0x7;

/// The highest class number the kernel defines (IOPRIO_CLASS_IDLE).
inline constexpr int MAX_CLASS = static_cast<int>(IoPriorityClass::Idle);

/// Whether @p ioClass has levels (Realtime and BestEffort); None and Idle do not.
[[nodiscard]] constexpr bool classHasLevels(IoPriorityClass ioClass) noexcept
{
    return ioClass == IoPriorityClass::Realtime || ioClass == IoPriorityClass::BestEffort;
}

/// @p priority as the ioprio value ioprio_set(2) takes (IOPRIO_PRIO_VALUE(class, level)). The level is
/// held to 0-7 for Realtime and BestEffort, and is 0 for None and Idle, which the kernel otherwise
/// refuses (None) or ignores (Idle).
[[nodiscard]] constexpr int encode(const IoPriority& priority) noexcept
{
    const int level = classHasLevels(priority.ioClass) ? Domain::Priority::clampIoLevel(priority.level) : 0;
    return (static_cast<int>(priority.ioClass) << CLASS_SHIFT) | level;
}

/// The I/O priority an ioprio_get(2) result names, or nullopt for a negative value (an error) or a
/// class the kernel does not define. The level is read only for Realtime and BestEffort.
[[nodiscard]] constexpr std::optional<IoPriority> decode(int value) noexcept
{
    if (value < 0)
    {
        return std::nullopt;
    }
    const int classBits = value >> CLASS_SHIFT;
    if (classBits > MAX_CLASS)
    {
        return std::nullopt;
    }
    const auto ioClass = static_cast<IoPriorityClass>(classBits);
    return IoPriority{
        .ioClass = ioClass,
        .level = classHasLevels(ioClass) ? (value & LEVEL_MASK) : 0,
    };
}

} // namespace Platform::IoPrio
