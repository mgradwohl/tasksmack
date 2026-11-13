#include "ElevatedRestart.h"

#include "Platform/IElevatedRestart.h"
#include "Platform/Windows/BrokerHelper.h"
#include "Platform/Windows/WindowsHandles.h"

#include <cstddef>
#include <expected>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace Platform::Windows::ElevatedRestart
{

namespace
{

/// @p text as one CommandLineToArgvW argument: in quotes, with the backslashes before the closing quote
/// doubled, so a directory that ends in one ("C:\") isn't read as an escaped quote. A path holds no quotes.
[[nodiscard]] std::wstring quotedArgument(std::wstring_view text)
{
    std::wstring result;
    result.reserve(text.size() + 4);
    result.push_back(L'"');
    result.append(text);
    std::size_t trailing = 0;
    while (trailing < text.size() && text[text.size() - 1 - trailing] == L'\\')
    {
        ++trailing;
    }
    result.append(trailing, L'\\');
    result.push_back(L'"');
    return result;
}

/// @p parameters split as the new copy's CommandLineToArgvW will split its command line, after its path.
[[nodiscard]] std::optional<std::vector<std::wstring>> splitAsTheCopyWill(const std::wstring& parameters)
{
    const std::wstring commandLine = std::format(L"\"TaskSmack.exe\" {}", parameters);
    int count = 0;
    struct Free
    {
        void operator()(LPWSTR* memory) const noexcept
        {
            LocalFree(static_cast<HLOCAL>(memory));
        }
    };
    const std::unique_ptr<LPWSTR, Free> argv(CommandLineToArgvW(commandLine.c_str(), &count));
    if (!argv || count < 1)
    {
        return std::nullopt;
    }
    const std::span<LPWSTR> all(argv.get(), static_cast<std::size_t>(count));
    return std::vector<std::wstring>(all.begin() + 1, all.end());
}

/// @p text, ASCII, widened.
[[nodiscard]] std::wstring widenAscii(std::string_view text)
{
    return {text.begin(), text.end()};
}

} // namespace

std::expected<std::wstring, std::string> parameters(DWORD pid, const std::filesystem::path& configDir)
{
    if (configDir.empty())
    {
        return std::unexpected(std::string("TaskSmack's settings folder isn't known"));
    }
    const std::wstring handoff = widenAscii(HANDOFF_ARGUMENT);
    const std::wstring configArgument = widenAscii(CONFIG_DIR_ARGUMENT);
    const std::wstring pidText = std::to_wstring(pid);
    std::wstring line = std::format(L"{} {} {} {}", handoff, pidText, configArgument, quotedArgument(configDir.native()));

    const auto split = splitAsTheCopyWill(line);
    if (!split.has_value() || split->size() != 4 || (*split)[0] != handoff || (*split)[1] != pidText || (*split)[2] != configArgument ||
        (*split)[3] != configDir.native())
    {
        return std::unexpected(std::string("TaskSmack wouldn't read its own command line as meant"));
    }
    return line;
}

ElevatedRestartResult restart(const std::filesystem::path& configDir, HWND owner, const Functions& functions, DWORD pid)
{
    const auto failed = [](std::string detail)
    {
        return ElevatedRestartResult{.outcome = ElevatedRestartResult::Outcome::Failed, .detail = std::move(detail)};
    };
    const std::optional<std::wstring> image = BrokerHelper::ownImage(functions.getModuleFileName);
    if (!image.has_value())
    {
        return failed(
            std::format("couldn't tell where TaskSmack is: {}", std::system_category().message(static_cast<int>(GetLastError()))));
    }
    const auto line = parameters(pid, configDir);
    if (!line.has_value())
    {
        return failed(line.error());
    }

    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    // The process back (only then does this copy close), ShellExecuteExW's work done before it returns (this
    // worker ends next), and no error dialog of the shell's: the notice says what happened.
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    info.hwnd = owner; // the prompt in front of TaskSmack
    info.lpVerb = L"runas";
    info.lpFile = image->c_str();
    info.lpParameters = line->c_str();
    info.nShow = SW_SHOWNORMAL;
    if (functions.shellExecuteEx(&info) == FALSE)
    {
        const DWORD error = GetLastError();
        if (error == ERROR_CANCELLED)
        {
            return {.outcome = ElevatedRestartResult::Outcome::Declined, .detail = {}};
        }
        return failed(std::system_category().message(static_cast<int>(error)));
    }
    const UniqueHandle process(info.hProcess);
    if (!process)
    {
        return failed("Windows started no process to hand over to");
    }
    return {.outcome = ElevatedRestartResult::Outcome::Started, .detail = {}};
}

AdministratorPrompt promptFor(TOKEN_ELEVATION_TYPE elevationType) noexcept
{
    return elevationType == TokenElevationTypeLimited ? AdministratorPrompt::Consent : AdministratorPrompt::Credentials;
}

HandoffWait waitForHandoff(DWORD pid, DWORD timeoutMs, const WaitFunctions& functions)
{
    const UniqueHandle process(functions.openProcess(SYNCHRONIZE, FALSE, pid));
    if (!process)
    {
        return HandoffWait::NotRunning;
    }
    switch (functions.waitForSingleObject(process.get(), timeoutMs))
    {
    case WAIT_OBJECT_0:
        return HandoffWait::Exited;
    case WAIT_TIMEOUT:
        return HandoffWait::TimedOut;
    default:
        return HandoffWait::NotRunning; // a handle that can't be waited on: nothing to wait for
    }
}

} // namespace Platform::Windows::ElevatedRestart
