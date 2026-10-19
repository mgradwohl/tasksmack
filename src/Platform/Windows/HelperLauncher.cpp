#include "HelperLauncher.h"

#include "Platform/Windows/BrokerHelper.h"
#include "Platform/Windows/PipeBroker.h"
#include "Platform/Windows/WindowsElevation.h"
#include "Platform/Windows/WindowsHandles.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <objbase.h>

namespace Platform::Windows::HelperLauncher
{

namespace
{

/// The longest path GetModuleFileNameW returns, in characters (an extended-length path).
constexpr DWORD LONGEST_PATH = 32'768;

/// This executable's full path, from @p getModuleFileName: MAX_PATH first, then the longest path.
[[nodiscard]] std::optional<std::wstring> ownImage(Functions::GetModuleFileNameFn getModuleFileName)
{
    for (const DWORD size : {DWORD{MAX_PATH}, LONGEST_PATH})
    {
        std::wstring path(size, L'\0');
        const DWORD written = getModuleFileName(nullptr, path.data(), size);
        if (written == 0)
        {
            return std::nullopt;
        }
        if (written < size)
        {
            path.resize(written);
            return path;
        }
    }
    return std::nullopt;
}

/// This process's creation time, in a FILETIME's ticks: what the helper checks TaskSmack's against.
[[nodiscard]] std::optional<std::uint64_t> ownStartTicks()
{
    FILETIME creation{};
    FILETIME exitTime{};
    FILETIME kernelTime{};
    FILETIME userTime{};
    if (GetProcessTimes(GetCurrentProcess(), &creation, &exitTime, &kernelTime, &userTime) == FALSE)
    {
        return std::nullopt;
    }
    return (static_cast<std::uint64_t>(creation.dwHighDateTime) << 32U) | creation.dwLowDateTime;
}

/// @p parameters split as the helper's CommandLineToArgvW will split its command line, after its path.
[[nodiscard]] std::optional<std::vector<std::wstring>> splitAsTheHelperWill(const std::wstring& parameters)
{
    const std::wstring commandLine = std::format(L"\"{}\" {}", HELPER_FILE_NAME, parameters);
    int count = 0;
    LPWSTR* argv = CommandLineToArgvW(commandLine.c_str(), &count);
    if (argv == nullptr)
    {
        return std::nullopt;
    }
    const std::span<LPWSTR> all(argv, static_cast<std::size_t>(count));
    std::vector<std::wstring> split;
    if (!all.empty())
    {
        split.assign(all.begin() + 1, all.end());
    }
    LocalFree(static_cast<HLOCAL>(argv));
    return split;
}

} // namespace

BOOL WINAPI shellExecuteWithCom(SHELLEXECUTEINFOW* info)
{
    // ShellExecuteExW may hand the work to shell extensions, which need COM on the calling thread.
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const BOOL started = ShellExecuteExW(info);
    const DWORD error = GetLastError();
    if (SUCCEEDED(com))
    {
        CoUninitialize();
    }
    SetLastError(error);
    return started;
}

std::expected<std::wstring, std::string> parameters(const BrokerHelper::Arguments& arguments)
{
    std::wstring line = BrokerHelper::commandLine(arguments);
    // What the helper will read: nothing that quoting or spaces could split differently.
    const auto split = splitAsTheHelperWill(line);
    if (!split.has_value())
    {
        return std::unexpected(std::string("the helper's command line couldn't be split"));
    }
    const std::vector<std::wstring_view> views(split->begin(), split->end());
    const auto parsed = BrokerHelper::parseArguments(views);
    if (!parsed.has_value() || parsed->pipeName != arguments.pipeName || parsed->parentPid != arguments.parentPid ||
        parsed->parentStartTicks != arguments.parentStartTicks)
    {
        return std::unexpected(std::string("the helper wouldn't read its command line as meant"));
    }
    return line;
}

std::string launchError(DWORD error)
{
    switch (error)
    {
    case ERROR_CANCELLED:
        return std::string(PROMPT_DISMISSED);
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
        return std::string(HELPER_MISSING);
    default:
        break;
    }
    return std::format("couldn't start TaskSmackHelper.exe: {}", std::system_category().message(static_cast<int>(error)));
}

std::expected<UniqueHandle, std::string>
launch(const std::wstring& pipeName, const std::stop_token& stopToken, const Functions& functions, HWND owner)
{
    if (stopToken.stop_requested())
    {
        return std::unexpected(std::string("cancelled"));
    }
    const auto image = ownImage(functions.getModuleFileName);
    if (!image.has_value())
    {
        return std::unexpected(
            std::format("couldn't tell where TaskSmack is: {}", std::system_category().message(static_cast<int>(GetLastError()))));
    }
    const std::wstring helper = BrokerHelper::besidePath(*image, HELPER_FILE_NAME);
    if (helper.empty())
    {
        return std::unexpected(std::string("couldn't tell where TaskSmack is"));
    }
    const auto ticks = ownStartTicks();
    if (!ticks.has_value())
    {
        return std::unexpected(std::string("couldn't read TaskSmack's start time"));
    }
    const auto line = parameters({.pipeName = pipeName, .parentPid = GetCurrentProcessId(), .parentStartTicks = *ticks});
    if (!line.has_value())
    {
        return std::unexpected(line.error());
    }

    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    // The process back, ShellExecuteExW's work done before it returns (this thread may end next), and
    // no error dialog of the shell's: the error is TaskSmack's to show.
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    info.hwnd = owner; // the prompt in front of TaskSmack (#2550)
    info.lpVerb = L"runas";
    info.lpFile = helper.c_str();
    info.lpParameters = line->c_str();
    info.nShow = SW_HIDE;
    if (functions.shellExecuteEx(&info) == FALSE)
    {
        return std::unexpected(launchError(GetLastError()));
    }
    UniqueHandle process(info.hProcess);
    if (!process)
    {
        return std::unexpected(std::string("the helper started without a process to check"));
    }
    return process;
}

PipeBroker::Launcher makeLauncher(Functions functions, HWND owner)
{
    return [functions, owner](const std::wstring& pipeName, const std::stop_token& stopToken)
    {
        return launch(pipeName, stopToken, functions, owner);
    };
}

std::expected<void, std::string> availability(bool elevated, const std::wstring& ownImage, BrokerHelper::Self::InstallCheck installCheck)
{
    if (elevated)
    {
        return std::unexpected(std::string("TaskSmack already runs as an administrator"));
    }
    const std::wstring helper = BrokerHelper::besidePath(ownImage, HELPER_FILE_NAME);
    std::error_code error;
    if (helper.empty() || !std::filesystem::is_regular_file(helper, error))
    {
        return std::unexpected(std::string(HELPER_MISSING));
    }
    // What the helper will check before it serves (#2572): ask first, rather than raise a UAC prompt
    // for a helper that then refuses TaskSmack.
    const std::array<std::wstring, 2> install{ownImage, helper};
    if (installCheck == nullptr)
    {
        return std::unexpected(std::string("no install check"));
    }
    if (const auto unchangeable = installCheck(GetCurrentProcess(), install); !unchangeable.has_value())
    {
        return std::unexpected(std::format("TaskSmack's folder isn't one only an administrator can change, so Windows can't run its "
                                           "administrator helper for it: {}",
                                           unchangeable.error()));
    }
    return {};
}

std::expected<void, std::string> availability()
{
    const auto image = ownImage(&::GetModuleFileNameW);
    if (!image.has_value())
    {
        return std::unexpected(std::string("couldn't tell where TaskSmack is"));
    }
    return availability(isProcessElevated(), *image, &BrokerHelper::checkUnchangeableByParent);
}

} // namespace Platform::Windows::HelperLauncher
