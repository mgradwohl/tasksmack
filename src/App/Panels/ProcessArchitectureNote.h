#pragma once

// What Process Details' Architecture row says about the architecture a process runs as (#2394), on
// hover: whether it is emulated, and what that costs. Keyed on ProcessSnapshot::architecture, the
// label Platform::ProcessArchitecture::architectureLabel() words. Pure, so it is unit-tested.

#include <string_view>

namespace App::ProcessArchitectureNote
{

/// The Architecture row's tooltip for @p architecture; empty for an empty (unread) one.
[[nodiscard]] constexpr std::string_view architectureNote(std::string_view architecture) noexcept
{
    if (architecture.empty())
    {
        return {};
    }
    if (architecture.ends_with("(emulated)"))
    {
        return "Emulated: its instructions are translated to this machine's own as it runs, which costs "
               "performance. A native build of the program, where there is one, runs faster.";
    }
    if (architecture.ends_with("(WOW64)"))
    {
        return "A 32-bit program, run by Windows' WOW64 layer on 64-bit Windows: at native speed, but with "
               "a 32-bit program's address space.";
    }
    if (architecture.ends_with("(32-bit)"))
    {
        return "A 32-bit program, run natively by this 64-bit kernel: at native speed, but with a 32-bit "
               "program's address space.";
    }
    if (architecture == "ARM64EC")
    {
        return "ARM64EC: native ARM64 code that can load x64 code beside it. That x64 code is emulated, which "
               "costs performance; the ARM64 code is not.";
    }
    return "Native: it runs as this machine's own architecture, with no emulation.";
}

} // namespace App::ProcessArchitectureNote
