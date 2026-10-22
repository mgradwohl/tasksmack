// TaskSmackHelper.exe: TaskSmack's privileged broker on Windows (#2517; #2181). TaskSmack's UI never runs
// elevated for this. When the user chooses an action TaskSmack can't do itself, TaskSmack starts this
// through ShellExecuteExW("runas"), so Windows asks for consent (or an administrator's password), and
// it runs elevated. It serves only the TaskSmack.exe beside it that started it, and only from an
// install TaskSmack's user can't change (BrokerHelper::checkParentIsTaskSmack). It answers a closed set
// of BrokerProtocol requests on a named pipe only that TaskSmack may open, and exits when TaskSmack
// closes the pipe or exits, or after a few idle minutes; an over-the-shoulder elevation, as another
// account, after its one action (BrokerHelper::sessionPolicy, #2550).
//
// It links only Platform: no SDL, ImGui, GL or config file. Its command line is untrusted, so
// BrokerHelper parses it exactly and decides whom to let in from TaskSmack's process handle and token,
// never from a string (BrokerHelper.h). A Windows (not console) program, so starting it opens no window.

#include "Platform/BrokerProtocol.h"
#include "Platform/BrokerRequestMapping.h"
#include "Platform/Windows/BrokerHelper.h"
#include "Platform/Windows/BrokerPipeServer.h"
#include "Platform/Windows/WindowsDebugPrivilege.h"
#include "Platform/Windows/WindowsProcessActions.h"
#include "Platform/Windows/WindowsProcessConnections.h"
#include "Platform/Windows/WindowsProcessModules.h"
#include "Platform/Windows/WindowsProcessOpenFiles.h"

#include <spdlog/spdlog.h>

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

#include <cstddef>
#include <span>
#include <stop_token>
#include <string_view>
#include <vector>

namespace
{

/// The command line's arguments after the program's name, or nothing when it can't be split.
[[nodiscard]] std::vector<std::wstring_view> arguments(int& count, LPWSTR*& argv)
{
    argv = CommandLineToArgvW(GetCommandLineW(), &count);
    if (argv == nullptr || count < 1)
    {
        return {};
    }
    const std::span<LPWSTR> all(argv, static_cast<std::size_t>(count));
    return {all.begin() + 1, all.end()};
}

} // namespace

// NOLINTNEXTLINE(misc-use-internal-linkage, readability-identifier-naming) - the Windows entry point
int WINAPI wWinMain(HINSTANCE /*instance*/, HINSTANCE /*previous*/, LPWSTR /*commandLine*/, int /*show*/)
{
    namespace Helper = Platform::Windows::BrokerHelper;
    // Only System32 for any DLL loaded from here on, never the helper's own folder, which a portable
    // TaskSmack may have unpacked somewhere its user can write. The imports are covered by the linker's
    // /DEPENDENTLOADFLAG (CMakeLists.txt).
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);
    // And the process mitigations (#2557), before anything else runs. One this Windows doesn't know is
    // only logged: the helper still works without it.
    try
    {
        const std::vector<std::string_view> refused =
            Helper::applyMitigations([](PROCESS_MITIGATION_POLICY policy, void* value, std::size_t size)
                                     { return SetProcessMitigationPolicy(policy, value, size) != FALSE; });
        for (const std::string_view name : refused)
        {
            spdlog::warn("TaskSmackHelper: Windows refused the {} mitigation", name);
        }
    }
    catch (...) // NOLINT(bugprone-empty-catch) - out of memory listing them: the helper runs without the log
    {}

    int count = 0;
    LPWSTR* argv = nullptr;
    const std::vector<std::wstring_view> args = arguments(count, argv);
    const auto parsed = Helper::parseArguments(args);
    LocalFree(static_cast<HLOCAL>(argv));
    if (!parsed.has_value())
    {
        return static_cast<int>(Helper::Exit::Usage);
    }

    try
    {
        // What TaskSmack must be (the TaskSmack.exe beside this) and who started this, that the parent
        // named is checked against (#2549).
        const auto self = Helper::readSelf();
        if (!self.has_value())
        {
            return static_cast<int>(Helper::Exit::NoParent);
        }
        // As an elevated TaskSmack does (#2128): without it, services' processes refuse an administrator.
        static_cast<void>(Platform::Windows::enableDebugPrivilegeIfElevated());
        Platform::WindowsProcessActions actions;
        // Process Details' reads of an elevated or another user's process (#2309). Windows has no
        // environment (#179) or PSS/USS reader, and reads firmware and crashes unelevated, so those
        // requests get the mapping's ErrorReply.
        Platform::Windows::WindowsProcessModulesReader modules;
        Platform::Windows::WindowsProcessOpenFilesReader openFiles;
        Platform::Windows::WindowsProcessConnectionsReader connections;
        const Platform::BrokerServer::Readers readers{
            .environment = nullptr,
            .modules = &modules,
            .openFiles = &openFiles,
            .connections = &connections,
        };
        const Platform::Windows::BrokerPipeServer::RequestHandler handler = [&actions, &readers](const Platform::Broker::Request& request)
        {
            return Platform::BrokerServer::handleRequest(request, actions, readers);
        };
        const std::stop_source never;
        return static_cast<int>(Helper::run(
            *parsed, *self, handler, Helper::IDLE_TIMEOUT, Platform::Windows::BrokerPipeServer::ACCEPT_TIMEOUT, never.get_token()));
    }
    catch (...) // an allocation failure or the like: TaskSmack sees the pipe close
    {
        return static_cast<int>(Helper::Exit::Failed);
    }
}
