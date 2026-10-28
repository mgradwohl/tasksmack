#pragma once

// A process's CPU scheduling policy (#3086), as htop's Policy column shows it: Linux's SCHED_* classes,
// with the real-time priority for FIFO and round robin. Pure, so tested on every platform.

#include <compare>
#include <cstdint>
#include <format>
#include <string>

namespace Platform
{

/// The scheduling policy, in the order the Policy column sorts (most urgent last when ascending).
enum class SchedPolicy : std::uint8_t
{
    Unknown,    ///< Not read (another platform, or a short /proc/[pid]/stat)
    Idle,       ///< SCHED_IDLE: runs only when nothing else wants the CPU
    Batch,      ///< SCHED_BATCH: CPU-bound, never treated as interactive
    Normal,     ///< SCHED_OTHER: the default time-sharing class
    Deadline,   ///< SCHED_DEADLINE: runtime/deadline/period reservations
    RoundRobin, ///< SCHED_RR: real time, sharing the CPU with its own priority in turns
    Fifo,       ///< SCHED_FIFO: real time, running until it blocks or yields
};

/// The policy Linux's /proc/[pid]/stat field 41 (or sched_getscheduler()) names; SCHED_RESET_ON_FORK, a
/// flag the kernel may OR in, is ignored. Unknown for any other value.
[[nodiscard]] constexpr SchedPolicy schedPolicyFromLinux(std::uint32_t policy) noexcept
{
    constexpr std::uint32_t RESET_ON_FORK = 0x40000000U;
    switch (policy & ~RESET_ON_FORK)
    {
    case 0:
        return SchedPolicy::Normal;
    case 1:
        return SchedPolicy::Fifo;
    case 2:
        return SchedPolicy::RoundRobin;
    case 3:
        return SchedPolicy::Batch;
    case 5:
        return SchedPolicy::Idle;
    case 6:
        return SchedPolicy::Deadline;
    default:
        return SchedPolicy::Unknown;
    }
}

/// How the Policy column and Process Details word it: "Normal", "Batch", "Idle", "Deadline", and the
/// real-time ones with their priority, "FIFO 50" and "RR 10". Empty for Unknown.
[[nodiscard]] inline std::string schedPolicyLabel(SchedPolicy policy, std::uint32_t rtPriority)
{
    switch (policy)
    {
    case SchedPolicy::Normal:
        return "Normal";
    case SchedPolicy::Batch:
        return "Batch";
    case SchedPolicy::Idle:
        return "Idle";
    case SchedPolicy::Deadline:
        return "Deadline";
    case SchedPolicy::RoundRobin:
        return std::format("RR {}", rtPriority);
    case SchedPolicy::Fifo:
        return std::format("FIFO {}", rtPriority);
    case SchedPolicy::Unknown:
        break;
    }
    return {};
}

/// The Policy column's order: by policy as the enum lists them, then by real-time priority.
[[nodiscard]] constexpr std::strong_ordering
compareSchedPolicy(SchedPolicy a, std::uint32_t aPriority, SchedPolicy b, std::uint32_t bPriority) noexcept
{
    if (const auto byPolicy = a <=> b; byPolicy != 0)
    {
        return byPolicy;
    }
    return aPriority <=> bPriority;
}

} // namespace Platform
