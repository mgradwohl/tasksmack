#pragma once

// Where TaskSmack.exe looks for a DLL it, or a library it uses (SDL, the GL loaders), loads by name (#3897): the
// decision WinMain makes before anything else, from its token and its command line. System32 only when it runs
// elevated in a way that has a less-privileged side, which could have planted a DLL in the folder it runs from;
// otherwise that folder as well, where a software OpenGL is found on a PC with no GPU driver. Its imports come from
// System32 only either way (/DEPENDENTLOADFLAG, cmake/CompilerOptions.cmake).
//
// Pure functions of their inputs, so test_DllSearchPolicy.cpp covers every case; system32OnlyForThisProcess() reads
// the process's own. Nothing here loads a DLL: it runs before SetDefaultDllDirectories.

#ifdef _WIN32
// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// clang-format on

#include <string_view>

namespace Platform::Windows::DllSearchPolicy
{

/// Whether @p commandLine (GetCommandLineW()'s, the program first) holds Platform::HANDOFF_ARGUMENT as an argument of
/// its own: a copy Restart as administrator started (#2728), whatever account approved it. A plain scan, split at
/// spaces and tabs with quotes stripped, not the CRT's parse: it errs towards finding it, which only restricts more.
[[nodiscard]] bool hasHandoffArgument(std::wstring_view commandLine) noexcept;

/// Whether a DLL loaded by name should come from System32 only, for a token of elevation type @p elevation and a
/// command line @p commandLine:
///   - TokenElevationTypeFull: elevated through UAC, consent or another administrator's credentials, so the
///     unelevated side, which may write to the folder TaskSmack runs from, is less privileged. System32 only.
///   - --handoff: Restart as administrator started it. Over the shoulder to the built-in Administrator (RID 500,
///     a LAPS account), whose token is TokenElevationTypeDefault, the user who clicked Restart is still the less
///     privileged side. System32 only.
///   - Otherwise (TokenElevationTypeLimited, or TokenElevationTypeDefault without --handoff: a standard user, an
///     administrator with no split token, or a hosted CI runner, whose tests run elevated with a Default token):
///     the folder TaskSmack runs from as well. Only its own account, or one already as privileged, could have put
///     a DLL there, as far as TaskSmack can tell (SECURITY.md names the cases left).
[[nodiscard]] bool system32Only(TOKEN_ELEVATION_TYPE elevation, std::wstring_view commandLine) noexcept;

/// system32Only() for this process: its token's TokenElevationType, as currentElevationType() (WindowsElevation.h)
/// reads it (TokenElevationTypeDefault if it can't be read), and GetCommandLineW().
[[nodiscard]] bool system32OnlyForThisProcess() noexcept;

} // namespace Platform::Windows::DllSearchPolicy
#endif
