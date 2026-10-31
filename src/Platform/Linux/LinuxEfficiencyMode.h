#pragma once

// Efficiency mode on Linux (#3202), as Task Manager's on Windows (#3071): a background process kept out of
// the way. On means nice 19 for every thread, the lowest priority, and where TaskSmack may write it and the
// process is alone in its cgroup, the cgroup's cpu.weight lowered to 1, the minimum: under cgroup v2's CPU
// controller nice only ranks a cgroup's own processes, so the weight is what keeps it behind other cgroups'
// work. A process sharing its cgroup (a terminal tab's shell and what it started) keeps the cgroup's weight,
// which would slow its neighbours too, and tasksmack-helper can't write cgroup files (its sandbox opens
// nothing for writing), so the weight is a bonus and nice 19 is what Efficiency mode is: the probe reports
// a process at nice 19 as in it (EFFICIENCY_MODE_STATUS).
//
// Off puts back what On changed: the nice and weight remembered for that process, or nice 0 and the default
// weight (100) when nothing was remembered (TaskSmack restarted, or the helper turned it on); a nice or
// weight someone changed since On is left alone, as Windows leaves a priority class changed since
// (priorityClassToRestore()).
//
// Pure: the parsing and the restore rules, tested in test_LinuxProcessActions.cpp.

#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace Platform::LinuxEfficiency
{

inline constexpr int EFFICIENCY_NICE = 19;     ///< The lowest priority nice offers
inline constexpr int MIN_CPU_WEIGHT = 1;       ///< cgroup v2 cpu.weight's minimum
inline constexpr int DEFAULT_CPU_WEIGHT = 100; ///< cpu.weight's default
inline constexpr int MAX_CPU_WEIGHT = 10000;   ///< cpu.weight's maximum
inline constexpr std::string_view CPU_WEIGHT_FILE = "cpu.weight";
inline constexpr std::string_view CGROUP_PROCS_FILE = "cgroup.procs";

/// Whether a process at @p nice is in Efficiency mode, as the probe reports it.
[[nodiscard]] constexpr bool isEfficiencyNice(std::int32_t nice) noexcept
{
    return nice == EFFICIENCY_NICE;
}

/// The cgroup v2 path in /proc/<pid>/cgroup's text ("0::/user.slice/app.scope"), without its leading
/// slash ("user.slice/app.scope"; "" for the root cgroup), or nullopt when there is no v2 line.
[[nodiscard]] inline std::optional<std::string> unifiedCgroupPath(std::string_view procCgroup)
{
    while (!procCgroup.empty())
    {
        const std::size_t end = procCgroup.find('\n');
        const std::string_view line = procCgroup.substr(0, end);
        procCgroup = (end == std::string_view::npos) ? std::string_view{} : procCgroup.substr(end + 1);
        if (line.starts_with("0::/"))
        {
            return std::string(line.substr(4));
        }
    }
    return std::nullopt;
}

/// cpu.weight's value (1 to 10000), or nullopt for anything else.
[[nodiscard]] inline std::optional<int> parseCpuWeight(std::string_view text)
{
    while (!text.empty() && (text.back() == '\n' || text.back() == ' '))
    {
        text.remove_suffix(1);
    }
    int weight = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), weight);
    if (error != std::errc{} || end != text.data() + text.size() || weight < MIN_CPU_WEIGHT || weight > MAX_CPU_WEIGHT)
    {
        return std::nullopt;
    }
    return weight;
}

/// Whether cgroup.procs' text lists @p pid and no other process.
[[nodiscard]] inline bool onlyProcessIn(std::string_view cgroupProcs, std::int32_t pid)
{
    bool found = false;
    while (!cgroupProcs.empty())
    {
        const std::size_t end = cgroupProcs.find('\n');
        const std::string_view line = cgroupProcs.substr(0, end);
        cgroupProcs = (end == std::string_view::npos) ? std::string_view{} : cgroupProcs.substr(end + 1);
        if (line.empty())
        {
            continue;
        }
        std::int32_t listed = 0;
        const auto [last, error] = std::from_chars(line.data(), line.data() + line.size(), listed);
        if (error != std::errc{} || last != line.data() + line.size() || listed != pid)
        {
            return false;
        }
        found = true;
    }
    return found;
}

/// The nice Off puts back for a process now at @p current: the one remembered from On, or 0; nullopt
/// (left alone) when someone has changed it since On.
[[nodiscard]] constexpr std::optional<std::int32_t> niceToRestore(std::int32_t current, std::optional<std::int32_t> remembered) noexcept
{
    if (!isEfficiencyNice(current))
    {
        return std::nullopt;
    }
    return remembered.value_or(0);
}

/// The cpu.weight Off puts back for a cgroup now at @p current: the one remembered from On, or the default;
/// nullopt (left alone) when it isn't the minimum On set.
[[nodiscard]] constexpr std::optional<int> weightToRestore(std::optional<int> current, std::optional<int> remembered) noexcept
{
    if (current != MIN_CPU_WEIGHT)
    {
        return std::nullopt;
    }
    return remembered.value_or(DEFAULT_CPU_WEIGHT);
}

} // namespace Platform::LinuxEfficiency
