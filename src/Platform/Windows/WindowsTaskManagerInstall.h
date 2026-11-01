#pragma once

// Replace Task Manager on Windows (#3072): whether this copy is installed, and whether an elevated
// TaskSmack may write taskmgr.exe's IFEO Debugger itself (#3280).
//
// Windows starts the Debugger for whoever opens Task Manager, elevated after an administrator's
// Ctrl+Shift+Esc. So it may name only a TaskSmack.exe no one but an administrator can change. The
// helper checks that for the copy beside it (BrokerHelper::checkUnchangeableByParent()); an elevated
// TaskSmack, which writes the key without the helper, runs the same check (BrokerHelper::checkUnchangeableBy())
// against the user it runs for unelevated, not against its own administrator's token. Program Files is
// read from the shell's known folders, never from %ProgramFiles%, which the user can set.
//
// App only: the helper doesn't link shell32, and doesn't need any of this.

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

#include "Platform/Windows/WindowsHandles.h"

#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <shlobj.h>

namespace Platform::Windows
{

/// SHGetKnownFolderPath, or a test's: the path is allocated with CoTaskMemAlloc, and freed with CoTaskMemFree.
using KnownFolderPathFn = HRESULT(WINAPI*)(REFKNOWNFOLDERID, DWORD, HANDLE, PWSTR*);

/// The folders an installed TaskSmack is under, in UTF-8: the known folders FOLDERID_ProgramFiles,
/// FOLDERID_ProgramFilesX64 and FOLDERID_ProgramFilesX86, through @p knownFolderPath, each that it gives.
/// Never the environment: a user's own %ProgramFiles% reaches the programs the shell starts, elevated too.
[[nodiscard]] std::vector<std::string> installFolders(KnownFolderPathFn knownFolderPath = &::SHGetKnownFolderPath);

/// A token, for identification, with the rights the user of @p ownToken has unelevated: its linked
/// token when @p ownToken is an elevated administrator's under UAC (TokenElevationTypeFull). Otherwise
/// (UAC off, the built-in Administrator, or already unelevated) @p ownToken restricted as UAC would:
/// Administrators and "local account and member of Administrators" deny-only, and no privileges but
/// SeChangeNotifyPrivilege. @p ownToken needs TOKEN_QUERY | TOKEN_DUPLICATE. The error says what failed.
[[nodiscard]] std::expected<UniqueHandle, std::string> unelevatedTokenOf(HANDLE ownToken);

/// Whether no one but an administrator can change @p exe: it names itself (no link or junction on the
/// way, so the path written is the file checked) and BrokerHelper::checkUnchangeableBy() passes for it
/// with unelevatedTokenOf() this process's token, the exe, its folder and every folder above it. The
/// helper's check, against the user who would be changing it.
[[nodiscard]] std::expected<void, std::string> checkUnchangeableByUnelevatedUser(const std::wstring& exe);

/// checkUnchangeableByUnelevatedUser(), or a test's.
using DirectWriteCheck = std::expected<void, std::string> (*)(const std::wstring& exe);

/// Whether an elevated TaskSmack at @p selfPath (UTF-8) may write taskmgr.exe's Debugger itself: it is
/// installed (TaskManagerReplacement::isInstalledCopy() under @p installFolders) and @p check passes for
/// it. The error says which failed.
[[nodiscard]] std::expected<void, std::string> checkDirectWrite(std::string_view selfPath,
                                                                std::span<const std::string> installFolders,
                                                                DirectWriteCheck check = &checkUnchangeableByUnelevatedUser);

} // namespace Platform::Windows

#endif // _WIN32
