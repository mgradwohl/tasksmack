#pragma once

// TaskSmack's PipeBroker::Launcher (#2549; #2517 step c): starts TaskSmackHelper.exe elevated with
// ShellExecuteExW("runas"), so Windows asks for consent, or an administrator's password, first.
//   - The helper is the TaskSmackHelper.exe beside TaskSmack.exe, by the full path GetModuleFileNameW
//     gives TaskSmack's own image: never looked for on a search path or in the current directory.
//   - Its parameters are BrokerHelper::commandLine() for the pipe, TaskSmack's PID and TaskSmack's own
//     creation time. They are checked to read back exactly, as the helper will split and parse them,
//     before anything is started.
//   - A dismissed prompt (ERROR_CANCELLED) is PROMPT_DISMISSED, which callers take as a refusal, as a
//     dismissed pkexec prompt is on Linux; a missing helper is HELPER_MISSING.
//   - ShellExecuteExW blocks while the prompt is up, with no time limit: PipeBroker calls this on a
//     thread of its own, outside its lock.
//   - "runas" makes the helper TaskSmack's child: AppInfo creates it with TaskSmack as its parent
//     process, which the helper checks (BrokerHelper::checkParentIsTaskSmack()).
//
// The Win32 calls are a table of function pointers, so the tests drive every answer with fakes and never
// raise a UAC prompt (test_HelperLauncher.cpp).

#ifdef _WIN32
// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
// clang-format on

#include "Platform/Windows/BrokerHelper.h"
#include "Platform/Windows/PipeBroker.h"
#include "Platform/Windows/WindowsHandles.h"

#include <expected>
#include <stop_token>
#include <string>
#include <string_view>

namespace Platform::Windows::HelperLauncher
{

/// The helper's file name, beside TaskSmack.exe.
inline constexpr std::wstring_view HELPER_FILE_NAME = L"TaskSmackHelper.exe";
/// The error when the user dismisses the UAC prompt.
inline constexpr std::string_view PROMPT_DISMISSED = "the UAC prompt was dismissed";
/// The error when there is no TaskSmackHelper.exe beside TaskSmack.exe.
inline constexpr std::string_view HELPER_MISSING = "TaskSmackHelper.exe isn't in TaskSmack's folder";

/// ShellExecuteExW on a thread whose COM is initialised, as the shell asks of its callers: the default
/// Functions::shellExecuteEx. Leaves GetLastError() as ShellExecuteExW set it.
BOOL WINAPI shellExecuteWithCom(SHELLEXECUTEINFOW* info);

/// The calls launch() makes: the system's, or a test's fakes.
struct Functions
{
    using ShellExecuteExFn = BOOL(WINAPI*)(SHELLEXECUTEINFOW*);
    using GetModuleFileNameFn = DWORD(WINAPI*)(HMODULE, LPWSTR, DWORD);

    ShellExecuteExFn shellExecuteEx = &shellExecuteWithCom;
    GetModuleFileNameFn getModuleFileName = &::GetModuleFileNameW;
};

/// @p arguments as the helper's parameters: BrokerHelper::commandLine(), once it is checked that
/// CommandLineToArgvW splits it, and BrokerHelper::parseArguments() reads it, back into exactly
/// @p arguments. The error says it doesn't.
[[nodiscard]] std::expected<std::wstring, std::string> parameters(const BrokerHelper::Arguments& arguments);

/// What a failed ShellExecuteExW's @p error means for TaskSmack: PROMPT_DISMISSED, HELPER_MISSING, or
/// the system's words.
[[nodiscard]] std::string launchError(DWORD error);

/// Starts the helper beside this executable elevated, to serve @p pipeName for this process, and
/// returns its process. Blocks while the UAC prompt is up; @p stopToken is looked at only before the
/// prompt, which nothing can take down. The error says why there is no helper.
[[nodiscard]] std::expected<UniqueHandle, std::string>
launch(const std::wstring& pipeName, const std::stop_token& stopToken, const Functions& functions = {});

/// launch() with @p functions as a PipeBroker::Launcher: what TaskSmack's PipeBroker is built with.
[[nodiscard]] PipeBroker::Launcher makeLauncher(Functions functions = {});

} // namespace Platform::Windows::HelperLauncher

#endif // _WIN32
