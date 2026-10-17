#pragma once

// Body text of the startup privilege notice (ElevationNoticeLayer). Kept out of the layer, which
// renders it with ImGui, so the wording is unit-testable directly (see CONTRIBUTING.md's "extract
// the pure decision logic into a small header" pattern).

#include "Platform/ProcessTypes.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace App::ElevationNoticeText
{

/// Linux: the notice for the permissions missing (#2303), as Platform::MissingPrivilege bits: one
/// paragraph for each, saying what it blocks, then the remedy naming the capabilities that allow it.
/// With read access alone missing, it is the notice's original text, word for word. 0 (the
/// details unknown) reads as READ_OTHERS, the notice's original subject (#1287).
/// The remedy is worded for both causes: sudo alone can't restore capabilities removed from a root
/// process's bounding set (a container or hardened service), so the capabilities themselves are named.
/// Keep in step with docs/guide/faq.md ("Process I/O, FDs or network show N/A").
///
/// Line breaks separate paragraphs only ("\n\n"): the notice wraps its text to the dialog's width, and
/// a break inside a paragraph cut a line short beside the wrapped ones (#1200).
[[nodiscard]] inline std::string linuxText(std::uint8_t missing)
{
    namespace Missing = Platform::MissingPrivilege;
    if (missing == 0)
    {
        missing = Missing::READ_OTHERS;
    }
    const bool limitsActions = (missing & (Missing::SIGNAL_OTHERS | Missing::RAISE_PRIORITY)) != 0;
    std::string text = limitsActions ? "TaskSmack can't read complete data for, or act on, other users' processes."
                                     : "TaskSmack can't read complete data for other users' processes.";
    // The capabilities the remedy names, in the order the paragraphs need them, each once.
    std::vector<std::string_view> capabilities;
    const auto need = [&capabilities](std::string_view capability)
    {
        if (std::ranges::find(capabilities, capability) == capabilities.end())
        {
            capabilities.push_back(capability);
        }
    };
    if ((missing & Missing::PROCESSES_HIDDEN) != 0)
    {
        text += "\n\n/proc is mounted with hidepid, so other users' processes aren't listed at all. "
                "Membership of the mount's gid= group shows them too.";
        need("CAP_SYS_PTRACE");
    }
    if ((missing & Missing::READ_OTHERS) != 0)
    {
        text += "\n\nFile descriptor counts, I/O statistics and/or network usage may be unavailable for "
                "processes owned by other users.";
        need("CAP_DAC_READ_SEARCH");
        need("CAP_SYS_PTRACE");
    }
    if ((missing & Missing::SIGNAL_OTHERS) != 0)
    {
        text += "\n\nEnding, suspending or resuming other users' processes is refused.";
        need("CAP_KILL");
    }
    if ((missing & Missing::RAISE_PRIORITY) != 0)
    {
        text += "\n\nRaising a process's priority, the Realtime I/O class, and changing other users' "
                "priority or CPU affinity are refused.";
        need("CAP_SYS_NICE");
    }
    std::string named;
    for (std::size_t i = 0; i < capabilities.size(); ++i)
    {
        if (i > 0)
        {
            named += (i + 1 == capabilities.size()) ? " and " : ", ";
        }
        named += capabilities[i];
    }
    std::string_view those = "them";
    if (capabilities.size() == 1)
    {
        those = "it";
    }
    else if (capabilities.size() == 2)
    {
        those = "those two";
    }
    text += "\n\nFor complete data, run it as root (sudo TaskSmack) or grant it " + named +
            ". In a container or service that drops capabilities, allow " + std::string(those) + " there; root alone isn't enough.";
    return text;
}

/// Windows, not Administrator: TCP EStats collection can't be enabled.
inline constexpr std::string_view WINDOWS = "TaskSmack is running without Administrator privileges.\n\n"
                                            "Per-process network statistics are unavailable.\n\n"
                                            "For complete data, run TaskSmack as Administrator.";

/// Any other platform.
inline constexpr std::string_view OTHER = "TaskSmack is running without elevated privileges.\n\n"
                                          "Some per-process data may be unavailable.";

/// The notice body for the platform this build targets; @p missing (Platform::MissingPrivilege bits)
/// shapes the Linux one.
[[nodiscard]] inline std::string forCurrentPlatform([[maybe_unused]] std::uint8_t missing = 0)
{
#ifdef __linux__
    return linuxText(missing);
#elif defined(_WIN32)
    return std::string(WINDOWS);
#else
    return std::string(OTHER);
#endif
}

} // namespace App::ElevationNoticeText
