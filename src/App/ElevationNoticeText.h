#pragma once

// Body text of the startup privilege notice (ElevationNoticeLayer). Kept out of the layer, which
// renders it with ImGui, so the wording is unit-testable directly (see CONTRIBUTING.md's "extract
// the pure decision logic into a small header" pattern).

#include <string_view>

namespace App::ElevationNoticeText
{

/// Linux, effective capabilities (CapEff, for root too) lacking CAP_SYS_PTRACE or both of
/// CAP_DAC_READ_SEARCH and CAP_DAC_OVERRIDE -- or, with CapEff unreadable, not root: another user's
/// /proc/[pid]/fd and /proc/[pid]/io can't be read, so that process's FD count, I/O and network usage
/// (attributed through its fd links) read N/A (#1287). Root with those capabilities dropped (a
/// container) gets the notice; CAP_DAC_READ_SEARCH alone restores only FD counts and still gets it
/// (Platform::ProcPrivileges::hasReducedPrivileges).
/// Keep in step with docs/guide/faq.md ("Process I/O, FDs or network show N/A").
inline constexpr std::string_view LINUX = "TaskSmack is running without elevated privileges.\n\n"
                                          "File descriptor counts, I/O statistics and network\n"
                                          "usage are unavailable for processes owned by\n"
                                          "other users.\n\n"
                                          "For complete data, run:\n"
                                          "    sudo TaskSmack";

/// Windows, not Administrator: TCP EStats collection can't be enabled.
inline constexpr std::string_view WINDOWS = "TaskSmack is running without Administrator privileges.\n\n"
                                            "Per-process network statistics are unavailable.\n\n"
                                            "For complete data, run TaskSmack as Administrator.";

/// Any other platform.
inline constexpr std::string_view OTHER = "TaskSmack is running without elevated privileges.\n\n"
                                          "Some per-process data may be unavailable.";

/// The notice body for the platform this build targets.
[[nodiscard]] constexpr std::string_view forCurrentPlatform() noexcept
{
#ifdef __linux__
    return LINUX;
#elif defined(_WIN32)
    return WINDOWS;
#else
    return OTHER;
#endif
}

} // namespace App::ElevationNoticeText
