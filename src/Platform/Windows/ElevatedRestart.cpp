#include "ElevatedRestart.h"

#include "Platform/IElevatedRestart.h"
#include "Platform/Windows/BrokerHelper.h"
#include "Platform/Windows/WinString.h"
#include "Platform/Windows/WindowsHandles.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
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

// clang-format off
#include <aclapi.h> // GetNamedSecurityInfoW
#include <sddl.h>   // ConvertStringSidToSidW
#include <shlobj.h> // SHGetKnownFolderPath
// clang-format on

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

/// Whether @p left and @p right name the same path component, ignoring case as NTFS does.
[[nodiscard]] bool sameComponent(const std::wstring& left, const std::wstring& right) noexcept
{
    return CompareStringOrdinal(left.c_str(), static_cast<int>(left.size()), right.c_str(), static_cast<int>(right.size()), TRUE) ==
           CSTR_EQUAL;
}

std::optional<DWORD> realLinkCount(const std::wstring& path)
{
    const UniqueHandle file(CreateFileW(path.c_str(),
                                        FILE_READ_ATTRIBUTES,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                        nullptr,
                                        OPEN_EXISTING,
                                        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
                                        nullptr));
    BY_HANDLE_FILE_INFORMATION info{};
    if (!file || GetFileInformationByHandle(file.get(), &info) == FALSE)
    {
        return std::nullopt;
    }
    return info.nNumberOfLinks;
}

std::optional<bool> realOwnerIsPrivileged(const std::wstring& path)
{
    PSID owner = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION, &owner, nullptr, nullptr, nullptr, &descriptor) !=
            ERROR_SUCCESS ||
        owner == nullptr)
    {
        if (descriptor != nullptr)
        {
            LocalFree(descriptor);
        }
        return std::nullopt;
    }
    bool privileged = IsWellKnownSid(owner, WinBuiltinAdministratorsSid) != FALSE || IsWellKnownSid(owner, WinLocalSystemSid) != FALSE;
    // NT SERVICE\TrustedInstaller, which owns what Windows itself installs.
    PSID trustedInstaller = nullptr;
    if (ConvertStringSidToSidW(L"S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464", &trustedInstaller) != FALSE)
    {
        privileged = privileged || EqualSid(owner, trustedInstaller) != FALSE;
        LocalFree(trustedInstaller);
    }
    LocalFree(descriptor);
    return privileged;
}

/// The handed-over config directory, held open for the rest of the process (realPin()).
[[nodiscard]] UniqueHandle& pinnedDirectory()
{
    static UniqueHandle pinned;
    return pinned;
}

bool realPin(const std::wstring& path)
{
    // No FILE_SHARE_DELETE: while this is open the folder can't be renamed, deleted or replaced by a junction.
    UniqueHandle dir(CreateFileW(path.c_str(),
                                 FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 nullptr,
                                 OPEN_EXISTING,
                                 FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                                 nullptr));
    BY_HANDLE_FILE_INFORMATION info{};
    if (!dir || GetFileInformationByHandle(dir.get(), &info) == FALSE || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
    {
        return false; // swapped for a link between the checks and this open, or gone
    }
    pinnedDirectory() = std::move(dir);
    return true;
}

} // namespace

std::expected<std::wstring, std::string> parameters(DWORD pid, std::uint64_t startTicks, const std::filesystem::path& configDir)
{
    if (configDir.empty())
    {
        return std::unexpected(std::string("TaskSmack's settings folder isn't known"));
    }
    const std::wstring handoff = widenAscii(HANDOFF_ARGUMENT);
    const std::wstring configArgument = widenAscii(CONFIG_DIR_ARGUMENT);
    const std::wstring pidText = std::to_wstring(pid);
    const std::wstring ticksText = std::to_wstring(startTicks);
    std::wstring line = std::format(L"{} {} {} {} {}", handoff, pidText, ticksText, configArgument, quotedArgument(configDir.native()));

    const auto split = splitAsTheCopyWill(line);
    if (!split.has_value() || split->size() != 5 || (*split)[0] != handoff || (*split)[1] != pidText || (*split)[2] != ticksText ||
        (*split)[3] != configArgument || (*split)[4] != configDir.native())
    {
        return std::unexpected(std::string("TaskSmack wouldn't read its own command line as meant"));
    }
    return line;
}

ElevatedRestartResult
restart(const std::filesystem::path& configDir, HWND owner, const Functions& functions, DWORD pid, std::optional<std::uint64_t> startTicks)
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
    if (!startTicks.has_value())
    {
        startTicks = BrokerHelper::startTicksOf(GetCurrentProcess()); // read as the elevated copy will read it
    }
    if (!startTicks.has_value())
    {
        return failed("couldn't read TaskSmack's start time");
    }
    const auto line = parameters(pid, *startTicks, configDir);
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

HandoffWait waitForHandoff(DWORD pid, std::uint64_t startTicks, DWORD timeoutMs, const WaitFunctions& functions)
{
    const HANDLE opened = functions.openProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    const DWORD openError = GetLastError();
    const UniqueHandle process(opened);
    if (!process)
    {
        // Only a PID that names no process means it has gone. Access denied is the usual answer when another
        // account approved the prompt: its process may not open the user's.
        return openError == ERROR_INVALID_PARAMETER ? HandoffWait::NotRunning : HandoffWait::Unopenable;
    }
    if (BrokerHelper::startTicksOf(process.get()) != std::optional<std::uint64_t>{startTicks})
    {
        return HandoffWait::NotThatProcess; // the PID was reused, or isn't the one that restarted this copy
    }
    switch (functions.waitForSingleObject(process.get(), timeoutMs))
    {
    case WAIT_OBJECT_0:
        return HandoffWait::Exited;
    case WAIT_TIMEOUT:
        return HandoffWait::TimedOut;
    default:
        return HandoffWait::Unopenable; // a handle that can't be waited on: the lock decides
    }
}

std::expected<void, std::string>
checkConfigDir(const std::filesystem::path& dir, const std::filesystem::path& profilesRoot, const ConfigDirFunctions& functions)
{
    const auto refused = [&dir](std::string_view why)
    {
        return std::unexpected(std::format("{}: {}", WinString::wideToUtf8(dir.native()), why));
    };
    // A local, absolute, already-normal path: no "." or "..", no \\?\ or UNC prefix, a drive letter.
    const std::wstring& text = dir.native();
    if (!dir.is_absolute() || dir.lexically_normal() != dir || !dir.has_root_name() || text.starts_with(L"\\\\") ||
        dir.root_name().native().size() != 2)
    {
        return refused("not a plain local folder path");
    }
    // Under the user profiles folder, one folder or more below it.
    std::vector<std::wstring> components;
    for (const std::filesystem::path& part : dir.relative_path())
    {
        if (!part.empty())
        {
            components.push_back(part.native());
        }
    }
    std::vector<std::wstring> rootComponents;
    if (!profilesRoot.is_absolute() || !sameComponent(profilesRoot.root_name().native(), dir.root_name().native()))
    {
        return refused("not under the user profiles folder");
    }
    for (const std::filesystem::path& part : profilesRoot.relative_path())
    {
        if (!part.empty())
        {
            rootComponents.push_back(part.native());
        }
    }
    if (components.size() <= rootComponents.size() + 1 ||
        !std::ranges::equal(rootComponents, std::span(components).first(rootComponents.size()), sameComponent))
    {
        return refused("not inside a user's profile");
    }
    // Every folder from the drive down: real folders, never a junction, link or other reparse point.
    std::wstring prefix = dir.root_name().native() + L"\\";
    for (const std::wstring& component : components)
    {
        prefix += component;
        const DWORD attributes = functions.getFileAttributes(prefix.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
        {
            return refused("a folder on its path isn't there");
        }
        if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
        {
            return refused(std::format("{} is a junction or link", WinString::wideToUtf8(prefix)));
        }
        prefix += L"\\";
    }
    const auto ownerIsPrivileged = functions.ownerIsPrivileged != nullptr ? functions.ownerIsPrivileged : &realOwnerIsPrivileged;
    const std::optional<bool> privileged = ownerIsPrivileged(text);
    if (!privileged.has_value())
    {
        return refused("its owner can't be read");
    }
    if (*privileged)
    {
        return refused("it is owned by an administrator or system account, not a user");
    }
    // The files an elevated TaskSmack opens there: neither links nor hard links to a file elsewhere.
    const auto linkCount = functions.linkCount != nullptr ? functions.linkCount : &realLinkCount;
    for (const std::wstring_view name : {std::wstring_view(L"config.toml"), std::wstring_view(L"tasksmack.lock")})
    {
        const std::wstring file = (dir / name).native();
        const DWORD attributes = functions.getFileAttributes(file.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES)
        {
            continue; // not there yet: created as a plain file
        }
        if ((attributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0)
        {
            return refused(std::format("its {} is a link or a folder", WinString::wideToUtf8(name)));
        }
        if (linkCount(file) != std::optional<DWORD>{1})
        {
            return refused(std::format("its {} is hard-linked to another file", WinString::wideToUtf8(name)));
        }
    }
    const auto pin = functions.pin != nullptr ? functions.pin : &realPin;
    if (!pin(text))
    {
        return refused("it can't be held open, or changed while it was checked");
    }
    return {};
}

std::expected<void, std::string> checkConfigDir(const std::filesystem::path& dir)
{
    wchar_t* profiles = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_UserProfiles, 0, nullptr, &profiles)))
    {
        CoTaskMemFree(profiles);
        return std::unexpected(std::string("the user profiles folder isn't known"));
    }
    const std::filesystem::path root(profiles);
    CoTaskMemFree(profiles);
    return checkConfigDir(dir, root);
}

} // namespace Platform::Windows::ElevatedRestart
