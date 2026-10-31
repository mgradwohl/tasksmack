#pragma once

// A process's CPU scheduling policy (#3086), as htop's Policy column shows it: Linux's SCHED_* classes,
// with the real-time priority for FIFO and round robin. Pure, so tested on every platform.

#include <algorithm>
#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>

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

/// A scheduling policy's label held by value, so building it allocates nothing (#3244): the Processes table
/// words the policy from the snapshot's schedPolicy and rtPriority where it draws the cell, instead of every
/// snapshot carrying the text for each copy of the published snapshots to copy. The longest label,
/// "FIFO 4294967295", fits.
class SchedPolicyText
{
  public:
    [[nodiscard]] std::string_view view() const noexcept
    {
        return {m_Chars.data(), m_Size};
    }
    [[nodiscard]] bool empty() const noexcept
    {
        return m_Size == 0;
    }

  private:
    friend SchedPolicyText schedPolicyText(SchedPolicy policy, std::uint32_t rtPriority);

    std::array<char, 16> m_Chars{};
    std::size_t m_Size = 0;
};

/// How the Policy column and Process Details word it: "Normal", "Batch", "Idle", "Deadline", and the
/// real-time ones with their priority, "FIFO 50" and "RR 10". Empty for Unknown.
[[nodiscard]] inline SchedPolicyText schedPolicyText(SchedPolicy policy, std::uint32_t rtPriority)
{
    std::string_view fixed;
    std::string_view realTime;
    switch (policy)
    {
    case SchedPolicy::Normal:
        fixed = "Normal";
        break;
    case SchedPolicy::Batch:
        fixed = "Batch";
        break;
    case SchedPolicy::Idle:
        fixed = "Idle";
        break;
    case SchedPolicy::Deadline:
        fixed = "Deadline";
        break;
    case SchedPolicy::RoundRobin:
        realTime = "RR";
        break;
    case SchedPolicy::Fifo:
        realTime = "FIFO";
        break;
    case SchedPolicy::Unknown:
        break;
    }
    SchedPolicyText text;
    if (realTime.empty())
    {
        std::ranges::copy(fixed, text.m_Chars.begin());
        text.m_Size = fixed.size();
    }
    else
    {
        const auto written =
            std::format_to_n(text.m_Chars.begin(), static_cast<std::ptrdiff_t>(text.m_Chars.size()), "{} {}", realTime, rtPriority);
        text.m_Size = static_cast<std::size_t>(written.size);
    }
    return text;
}

/// schedPolicyText() as a string, for a caller that keeps it.
[[nodiscard]] inline std::string schedPolicyLabel(SchedPolicy policy, std::uint32_t rtPriority)
{
    return std::string(schedPolicyText(policy, rtPriority).view());
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
