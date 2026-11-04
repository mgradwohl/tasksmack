#include "WindowsFileLocation.h"

#include "ComScope.h"
#include "Platform/IProcessActions.h"
#include "WinString.h"

#include <spdlog/spdlog.h>

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h> // NOLINT(misc-include-cleaner) - umbrella header; symbols reside in sub-headers
#include <shlobj_core.h> // NOLINT(misc-include-cleaner) - ILCreateFromPathW, ILFree, SHOpenFolderAndSelectItems
// clang-format on

#include <format>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

namespace Platform::Windows
{

// NOLINTBEGIN(misc-include-cleaner) - Windows APIs; Win32 types come from windows.h sub-headers
namespace
{

// LCOV_EXCL_START - the real calls open an Explorer window, which no test may do (#3426)
HRESULT realSelectInFolder(const std::wstring& path)
{
    const ComScope com;
    if (!com.usable())
    {
        return CO_E_NOTINITIALIZED;
    }
    PIDLIST_ABSOLUTE item = ILCreateFromPathW(path.c_str());
    if (item == nullptr)
    {
        const DWORD error = GetLastError();
        return (error != ERROR_SUCCESS) ? HRESULT_FROM_WIN32(error) : E_FAIL;
    }
    // No children named: the folder is item's parent, opened with item selected.
    const HRESULT hr = SHOpenFolderAndSelectItems(item, 0, nullptr, 0);
    ILFree(item);
    return hr;
}

DWORD realStartProcess(const std::wstring& application, std::wstring& commandLine)
{
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info{};
    if (CreateProcessW(application.c_str(), commandLine.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &info) == 0)
    {
        return GetLastError();
    }
    CloseHandle(info.hThread);
    CloseHandle(info.hProcess);
    return ERROR_SUCCESS;
}

std::wstring realExplorerPath()
{
    std::wstring folder(MAX_PATH, L'\0');
    const UINT length = GetWindowsDirectoryW(folder.data(), static_cast<UINT>(folder.size()));
    if (length == 0 || length >= folder.size())
    {
        return {};
    }
    folder.resize(length);
    return folder + L"\\explorer.exe";
}
// LCOV_EXCL_STOP

[[nodiscard]] std::string hresultText(HRESULT hr)
{
    return std::format("0x{:08X}", static_cast<unsigned long>(hr));
}

} // namespace

FileLocationFunctions realFileLocationFunctions() noexcept
{
    return {.selectInFolder = &realSelectInFolder, .startProcess = &realStartProcess, .explorerPath = &realExplorerPath};
}

std::wstring explorerSelectCommandLine(std::wstring_view path)
{
    std::wstring commandLine = L"explorer.exe /select,\"";
    commandLine += path;
    commandLine += L'"';
    return commandLine;
}

ProcessActionResult revealInExplorer(const FileLocationFunctions& fns, const std::wstring& path)
{
    if (path.empty())
    {
        return ProcessActionResult::error("Its executable's path is unknown");
    }
    if (path.contains(L'"'))
    {
        // No Windows path holds one, and it would end the quoted argument early.
        return ProcessActionResult::error("Its executable's path isn't a valid file path");
    }
    const std::string utf8Path = WinString::wideToUtf8(path);
    HRESULT shellResult = E_NOTIMPL;
    if (fns.selectInFolder != nullptr)
    {
        shellResult = fns.selectInFolder(path);
        if (SUCCEEDED(shellResult))
        {
            spdlog::info("Asked Explorer to show {}", utf8Path);
            return ProcessActionResult::ok();
        }
        spdlog::warn("SHOpenFolderAndSelectItems failed on {} ({}); trying explorer.exe /select", utf8Path, hresultText(shellResult));
    }

    const std::wstring explorer = (fns.explorerPath != nullptr) ? fns.explorerPath() : std::wstring{};
    if (explorer.empty() || fns.startProcess == nullptr)
    {
        return ProcessActionResult::error(std::format("Explorer couldn't be asked to show it (error {})", hresultText(shellResult)));
    }
    std::wstring commandLine = explorerSelectCommandLine(path);
    const DWORD error = fns.startProcess(explorer, commandLine);
    if (error != ERROR_SUCCESS)
    {
        std::string message = std::format("Explorer couldn't be started: {}", std::system_category().message(static_cast<int>(error)));
        spdlog::warn("Failed to start explorer.exe /select on {}: {}", utf8Path, message);
        return ProcessActionResult::error(std::move(message));
    }
    spdlog::info("Started explorer.exe /select on {}", utf8Path);
    return ProcessActionResult::ok();
}
// NOLINTEND(misc-include-cleaner)

} // namespace Platform::Windows
