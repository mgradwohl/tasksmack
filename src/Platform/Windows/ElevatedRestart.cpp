#include "ElevatedRestart.h"

#include "Platform/IElevatedRestart.h"
#include "Platform/IHandedOverFolder.h"
#include "Platform/Windows/BrokerHelper.h"
#include "Platform/Windows/HandleRelativeFile.h"
#include "Platform/Windows/OrdinalCase.h"
#include "Platform/Windows/PinnedFolder.h"
#include "Platform/Windows/WinString.h"
#include "Platform/Windows/WindowsHandles.h"
#include "Platform/Windows/WindowsSecurityOwners.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <expected>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// clang-format off
#include <shlobj.h>  // SHGetKnownFolderPath
#include <userenv.h> // GetUserProfileDirectoryW: the token's own profile (#4106)
// clang-format on

namespace Platform::Windows::ElevatedRestart
{

namespace
{

/// Known folder @p id's path; nullopt when it can't be read.
[[nodiscard]] std::optional<std::filesystem::path> knownFolder(REFKNOWNFOLDERID id)
{
    wchar_t* path = nullptr;
    const HRESULT result = SHGetKnownFolderPath(id, 0, nullptr, &path);
    // Freed whether or not the call succeeded, as SHGetKnownFolderPath's contract asks.
    const std::unique_ptr<wchar_t, decltype(&::CoTaskMemFree)> owned(path, &::CoTaskMemFree);
    if (FAILED(result) || owned == nullptr || owned.get()[0] == L'\0')
    {
        return std::nullopt;
    }
    return std::filesystem::path(owned.get());
}

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

/// Whether @p left and @p right name the same path component, ignoring case as NTFS does: the whole text, a trailing
/// separator included. A named function, not the overloaded equalsOrdinalIgnoreCase, so std::ranges::equal can take it.
[[nodiscard]] bool sameComponent(const std::wstring& left, const std::wstring& right) noexcept
{
    return equalsOrdinalIgnoreCase(std::wstring_view(left), std::wstring_view(right));
}

} // namespace

std::optional<DWORD> linkCountOf(const std::wstring& path)
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

HANDLE openToPin(const std::wstring& path)
{
    // No FILE_SHARE_DELETE: while this is open the folder can't be renamed or deleted. READ_CONTROL: its owner is read
    // through this handle. FILE_FLAG_OPEN_REPARSE_POINT: a link here is opened itself, then refused by its attributes.
    return CreateFileW(path.c_str(),
                       FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | READ_CONTROL,
                       FILE_SHARE_READ | FILE_SHARE_WRITE,
                       nullptr,
                       OPEN_EXISTING,
                       FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                       nullptr);
}

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
        return failed(std::format("couldn't tell where TaskSmack is: {}", Platform::WinString::win32ErrorText(GetLastError())));
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
        return failed(Platform::WinString::win32ErrorText(error));
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

HandoffWait waitForHandoff(DWORD pid,
                           std::uint64_t startTicks,
                           DWORD timeoutMs,
                           const WaitFunctions& functions,
                           const HandoffProgress& onProgress,
                           DWORD progressEveryMs)
{
    auto* const opened = functions.openProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
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
    // In slices, so the wait can say it is still waiting (#3781); never INFINITE, and never longer than timeoutMs.
    const DWORD slice = progressEveryMs == 0 ? timeoutMs : progressEveryMs;
    DWORD waited = 0;
    while (true)
    {
        const DWORD thisSlice = std::min(slice, timeoutMs - waited);
        switch (functions.waitForSingleObject(process.get(), thisSlice))
        {
        case WAIT_OBJECT_0:
            return HandoffWait::Exited;
        case WAIT_TIMEOUT:
            break;
        default:
            return HandoffWait::Unopenable; // a handle that can't be waited on: the lock decides
        }
        waited += thisSlice;
        if (waited >= timeoutMs)
        {
            return HandoffWait::TimedOut;
        }
        if (onProgress)
        {
            onProgress(std::chrono::milliseconds{waited});
        }
    }
}

std::expected<UniqueHandle, std::string>
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
    // Inside a user's profile folder: two folders or more below the user profiles folder (C:\Users\pat\TaskSmack,
    // never C:\Users\pat itself).
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
        prefix += L'\\';
    }
    // Held open, and from here on checked through that handle (#3775): the folder it holds is the one checked
    // above only if it is still a real folder, no reparse point, at the same path, and the user's.
    UniqueHandle pinned(functions.openToPin(text));
    BY_HANDLE_FILE_INFORMATION info{};
    if (!pinned || functions.getFileInformation(pinned.get(), &info) == FALSE ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 || (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
    {
        return refused("it can't be held open, or changed while it was checked");
    }
    const std::optional<std::wstring> finalPath = finalPathOf(pinned.get(), functions.getFinalPath);
    if (!finalPath.has_value())
    {
        return refused("the folder it holds can't be named");
    }
    if (!sameComponent(*finalPath, text))
    {
        return refused(std::format("the folder it holds is {}, not the folder checked", WinString::wideToUtf8(*finalPath)));
    }
    const std::optional<bool> privileged = functions.ownerIsPrivileged(pinned.get());
    if (!privileged.has_value())
    {
        return refused("its owner can't be read");
    }
    if (*privileged)
    {
        return refused("it is owned by an administrator or system account, not a user");
    }
    // The files an elevated TaskSmack opens there: neither links nor hard links to a file elsewhere. Checked only
    // once the folder is held, so it can't be renamed away and these paths name files in the folder held.
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
        if (functions.linkCount(file) != std::optional<DWORD>{1})
        {
            return refused(std::format("its {} is hard-linked to another file", WinString::wideToUtf8(name)));
        }
    }
    return pinned;
}

std::expected<UniqueHandle, std::string> checkConfigDir(const std::filesystem::path& dir)
{
    const auto root = knownFolder(FOLDERID_UserProfiles);
    if (!root.has_value())
    {
        return std::unexpected(std::string("the user profiles folder isn't known"));
    }
    return checkConfigDir(dir, *root);
}

bool pathIsInside(const std::filesystem::path& child, const std::filesystem::path& ancestor)
{
    // Both in the form a handed-over folder's path is kept (absolute, normal, no trailing separator), so a profile
    // folder named with a trailing '\' ("C:\Users\pat\") still contains its own AppData.
    const std::filesystem::path c = handedOverFolderPath(child);
    const std::filesystem::path a = handedOverFolderPath(ancestor);
    if (!sameComponent(c.root_name().native(), a.root_name().native()))
    {
        return false;
    }
    auto childPart = c.begin();
    for (auto ancestorPart = a.begin(); ancestorPart != a.end(); ++ancestorPart, ++childPart)
    {
        if (childPart == c.end() || !sameComponent(childPart->native(), ancestorPart->native()))
        {
            return false;
        }
    }
    return childPart != c.end(); // strictly below: at least one component more
}

namespace
{

/// @p handle's owner, acceptable to @p ownerAcceptable, or the error why not.
[[nodiscard]] std::expected<void, std::string>
requireAcceptableOwner(HANDLE handle, OwnerAcceptableFn ownerAcceptable, std::string_view what)
{
    const std::optional<bool> acceptable = ownerAcceptable(handle);
    if (!acceptable.has_value())
    {
        return std::unexpected(std::format("{}'s owner can't be read", what));
    }
    if (!*acceptable)
    {
        return std::unexpected(std::format("{} is owned by another account, not this one or an administrator", what));
    }
    return {};
}

} // namespace

std::optional<bool> ownerIsOwnAccountOrAdministrators(HANDLE handle)
{
    const auto owner = ownerSidOf(handle);
    if (!owner.has_value())
    {
        return std::nullopt;
    }
    // The token's user read as every current-token caller reads it (readTokenUserSid(), #3864), through the same
    // pseudo-handle as before; a user that can't be read leaves Administrators alone acceptable, as it always has.
    return isAdministrators(owner->sid) || isTokenUser(owner->sid).value_or(false);
}

std::optional<std::filesystem::path> currentUserProfileDirectory()
{
    DWORD size = 0;
    GetUserProfileDirectoryW(GetCurrentProcessToken(), nullptr, &size); // asks for the length first
    if (size == 0)
    {
        return std::nullopt;
    }
    std::wstring buffer(size, L'\0');
    if (GetUserProfileDirectoryW(GetCurrentProcessToken(), buffer.data(), &size) == FALSE)
    {
        return std::nullopt;
    }
    buffer.resize(std::wcslen(buffer.c_str()));
    return std::filesystem::path(buffer);
}

std::expected<UniqueHandle, std::string> checkOwnConfigDir(const std::filesystem::path& dir,
                                                           const std::filesystem::path& profileDir,
                                                           const std::filesystem::path& profilesRoot,
                                                           ConfigDirFunctions functions,
                                                           OwnerAcceptableFn ownerAcceptable)
{
    if (profileDir.empty())
    {
        return std::unexpected(std::string("this account's own profile folder isn't known"));
    }
    // Anchored to the token's own profile, not just somewhere under the user profiles folder: a redirected AppData
    // (HKCU can move it) that points outside this profile is refused.
    if (!pathIsInside(dir, profileDir))
    {
        return std::unexpected(std::format("{} isn't inside this account's own profile {}",
                                           WinString::wideToUtf8(dir.native()),
                                           WinString::wideToUtf8(profileDir.native())));
    }
    // checkConfigDir()'s own owner check is set aside; the owner is required to pass ownerAcceptable below instead
    // (this account or Administrators), so a folder an elevated TaskSmack made (owned by Administrators) is kept while
    // SYSTEM's, TrustedInstaller's or another user's is refused.
    functions.ownerIsPrivileged = [](HANDLE /*handle*/) -> std::optional<bool>
    {
        return false;
    };
    if (functions.getFileAttributes(dir.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        if (!pathIsInside(dir.parent_path(), profileDir))
        {
            return std::unexpected(std::format("{}'s parent isn't inside this account's own profile", WinString::wideToUtf8(dir.native())));
        }
        // Not there yet: made in its parent, checked, held and owner-checked the same way, relative to that handle.
        auto parent = checkConfigDir(dir.parent_path(), profilesRoot, functions);
        if (!parent.has_value())
        {
            return std::unexpected(std::format("it isn't there, and its parent can't be held to make it in: {}", parent.error()));
        }
        if (auto owner = requireAcceptableOwner(parent->get(), ownerAcceptable, "its parent"); !owner.has_value())
        {
            return std::unexpected(std::move(owner.error()));
        }
        const PinnedFolder held(dir.parent_path(), std::move(*parent));
        if (const auto made = held.createFolder(dir.filename()); !made.has_value())
        {
            return std::unexpected(std::format("{}: it can't be made: {}", WinString::wideToUtf8(dir.native()), made.error()));
        }
    }
    auto pinned = checkConfigDir(dir, profilesRoot, functions);
    if (!pinned.has_value())
    {
        return pinned;
    }
    if (auto owner = requireAcceptableOwner(pinned->get(), ownerAcceptable, WinString::wideToUtf8(dir.native())); !owner.has_value())
    {
        return std::unexpected(std::move(owner.error()));
    }
    return pinned;
}

OwnConfigDir holdOwnConfigDir()
{
    const auto roaming = knownFolder(FOLDERID_RoamingAppData);
    if (!roaming.has_value())
    {
        return {.path = {}, .held = std::unexpected(std::string("the roaming AppData folder isn't known"))};
    }
    const std::filesystem::path dir = handedOverFolderPath(*roaming / L"TaskSmack");
    const auto root = knownFolder(FOLDERID_UserProfiles);
    if (!root.has_value())
    {
        return {.path = dir, .held = std::unexpected(std::string("the user profiles folder isn't known"))};
    }
    const auto profile = currentUserProfileDirectory();
    if (!profile.has_value())
    {
        return {.path = dir, .held = std::unexpected(std::string("this account's own profile folder isn't known"))};
    }
    auto pinned = checkOwnConfigDir(dir, *profile, *root);
    if (!pinned.has_value())
    {
        return {.path = dir, .held = std::unexpected(std::move(pinned.error()))};
    }
    return {.path = dir, .held = std::make_shared<PinnedFolder>(dir, std::move(*pinned))};
}

} // namespace Platform::Windows::ElevatedRestart
