#pragma once

#include <cstdint>

namespace Platform::HelperSandbox
{

/// What tasksmack-helper's seccomp filter does with a system call it doesn't list (#2752).
enum class Mode : std::uint8_t
{
    /// Allows it and has the kernel log it (SECCOMP_RET_LOG, audit type 1326): the default while the
    /// allow-list is proven on real systems, so a call it missed can't break the helper in the field.
    Log,
    /// Kills the helper (SECCOMP_RET_KILL_PROCESS); TaskSmack sees its session end. The helper runs so
    /// with `--seccomp=enforce`, which the tests pass; the default once Log has been through a release.
    Enforce,
};

/// The helper's argument that selects Mode::Enforce. pkexec passes argv but not the environment, and the
/// flag can only tighten the filter, so it's safe for anyone to pass.
inline constexpr const char* ENFORCE_ARGUMENT = "--seccomp=enforce";

} // namespace Platform::HelperSandbox
