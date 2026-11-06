#pragma once

/// @file CiRealSystemWindows.h
/// @brief What the Windows CI real-system tests (#3489) share: a standard user's token made from the runner's
/// administrator one, idle child processes that end on scope exit, finding a process by its image, an install folder
/// in Program Files only administrators can change, and an environment variable set for a child (#3490). Only the
/// CiRealSystem* suites use it, behind CiRealSystem.h's guard.

#ifdef _WIN32

#include "Core/Utf8Path.h"
#include "Platform/IProcessActions.h"
#include "Platform/Windows/WindowsHandles.h"

#include <gtest/gtest.h>

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <aclapi.h>
#include <tlhelp32.h>
// clang-format on

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace TestSupport::CiRealSystem
{

using Platform::Windows::UniqueHandle;

/// The bound on anything the tests wait for that should happen at once; a cold runner may scan a new image first.
inline constexpr std::chrono::milliseconds STARTUP_WAIT{60'000};
/// How often a wait looks again.
inline constexpr std::chrono::milliseconds POLL_INTERVAL{50};

/// A fresh suffix for the folders and names the tests make, so two runs never meet.
[[nodiscard]] inline std::wstring uniqueSuffix()
{
    std::random_device random;
    return std::format(L"{:08x}{:08x}", random(), GetCurrentProcessId());
}

/// @p process's creation time in a FILETIME's ticks: what a ProcessTarget names it by. 0 when it can't be read.
[[nodiscard]] inline std::uint64_t startTicksOf(HANDLE process)
{
    FILETIME creation{};
    FILETIME exitTime{};
    FILETIME kernelTime{};
    FILETIME userTime{};
    if (GetProcessTimes(process, &creation, &exitTime, &kernelTime, &userTime) == FALSE)
    {
        return 0;
    }
    return (static_cast<std::uint64_t>(creation.dwHighDateTime) << 32U) | creation.dwLowDateTime;
}

/// The SID of this process's user, as bytes; empty when it can't be read.
[[nodiscard]] inline std::vector<std::byte> ownUserSid()
{
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) == FALSE)
    {
        return {};
    }
    const UniqueHandle owned(token);
    DWORD size = 0;
    GetTokenInformation(owned.get(), TokenUser, nullptr, 0, &size);
    std::vector<std::byte> buffer(size);
    if (size == 0 || GetTokenInformation(owned.get(), TokenUser, buffer.data(), size, &size) == FALSE)
    {
        return {};
    }
    const auto* user =
        reinterpret_cast<const TOKEN_USER*>(buffer.data()); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast) - the token's TOKEN_USER
    const DWORD length = GetLengthSid(user->User.Sid);
    const auto* first = static_cast<const std::byte*>(user->User.Sid);
    return {first, first + length};
}

/// @p type's well-known SID, as bytes.
[[nodiscard]] inline std::vector<std::byte> wellKnownSid(WELL_KNOWN_SID_TYPE type)
{
    std::vector<std::byte> sid(SECURITY_MAX_SID_SIZE);
    auto size = static_cast<DWORD>(sid.size());
    if (CreateWellKnownSid(type, nullptr, sid.data(), &size) == FALSE)
    {
        return {};
    }
    sid.resize(size);
    return sid;
}

/// One entry of an Acl: a SID (as bytes), the rights it is given, and how what is under the object inherits it.
struct Grant
{
    std::vector<std::byte> sid;
    DWORD rights = 0;
    DWORD inheritance = NO_INHERITANCE;
};

/// An ACL made by SetEntriesInAclW from Grants, freed on scope exit. get() is null when it couldn't be made.
class Acl
{
  public:
    explicit Acl(std::vector<Grant> grants) : m_Grants(std::move(grants))
    {
        std::vector<EXPLICIT_ACCESS_W> entries;
        for (Grant& grant : m_Grants)
        {
            EXPLICIT_ACCESS_W entry{};
            entry.grfAccessPermissions = grant.rights;
            entry.grfAccessMode = SET_ACCESS;
            entry.grfInheritance = grant.inheritance;
            entry.Trustee.TrusteeForm = TRUSTEE_IS_SID;
            entry.Trustee.TrusteeType = TRUSTEE_IS_UNKNOWN;
            // TRUSTEE_IS_SID: ptstrName holds the SID, as the API documents.
            entry.Trustee.ptstrName = reinterpret_cast<LPWSTR>(grant.sid.data()); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
            entries.push_back(entry);
        }
        if (SetEntriesInAclW(static_cast<ULONG>(entries.size()), entries.data(), nullptr, &m_Acl) != ERROR_SUCCESS)
        {
            m_Acl = nullptr;
        }
    }
    ~Acl()
    {
        if (m_Acl != nullptr)
        {
            LocalFree(static_cast<HLOCAL>(m_Acl));
        }
    }
    Acl(const Acl&) = delete;
    Acl& operator=(const Acl&) = delete;
    Acl(Acl&&) = delete;
    Acl& operator=(Acl&&) = delete;

    [[nodiscard]] PACL get() const noexcept
    {
        return m_Acl;
    }

  private:
    std::vector<Grant> m_Grants;
    PACL m_Acl = nullptr;
};

/// A primary token for a standard user, made from this process's (the runner's administrator): Administrators
/// deny-only and every privilege but SeChangeNotify gone (CreateRestrictedToken), Medium integrity, and a default DACL that lets
/// in this user, SYSTEM and Administrators, so the processes started with it, and what they make, can be opened
/// by each other as well as by the test. CreateProcessAsUserW takes it without SeAssignPrimaryToken: it is a
/// restricted version of the caller's own token. @p alsoLetIn, when not empty, is one more SID the default DACL
/// gives full access: another account's, whose processes must open this one's. Null, with a failure added, when
/// it can't be made.
[[nodiscard]] inline UniqueHandle standardUserPrimaryToken(const std::vector<std::byte>& alsoLetIn = {})
{
    HANDLE own = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_ASSIGN_PRIMARY | TOKEN_ADJUST_DEFAULT, &own) == FALSE)
    {
        ADD_FAILURE() << "couldn't open this process's token: " << GetLastError();
        return {};
    }
    const UniqueHandle ownToken(own);
    std::vector<std::byte> administrators = wellKnownSid(WinBuiltinAdministratorsSid);
    SID_AND_ATTRIBUTES denyOnly{.Sid = administrators.data(), .Attributes = 0};
    HANDLE restricted = nullptr;
    if (CreateRestrictedToken(ownToken.get(), DISABLE_MAX_PRIVILEGE, 1, &denyOnly, 0, nullptr, 0, nullptr, &restricted) == FALSE)
    {
        ADD_FAILURE() << "couldn't restrict this process's token: " << GetLastError();
        return {};
    }
    UniqueHandle token(restricted);
    std::vector<Grant> grants{
        {.sid = ownUserSid(), .rights = GENERIC_ALL},
        {.sid = wellKnownSid(WinLocalSystemSid), .rights = GENERIC_ALL},
        {.sid = administrators, .rights = GENERIC_ALL},
    };
    if (!alsoLetIn.empty())
    {
        grants.push_back({.sid = alsoLetIn, .rights = GENERIC_ALL});
    }
    const Acl acl(std::move(grants));
    TOKEN_DEFAULT_DACL defaultDacl{.DefaultDacl = acl.get()};
    if (acl.get() == nullptr || SetTokenInformation(token.get(), TokenDefaultDacl, &defaultDacl, sizeof(defaultDacl)) == FALSE)
    {
        ADD_FAILURE() << "couldn't set the restricted token's default DACL: " << GetLastError();
        return {};
    }
    // Medium integrity, as a standard user's processes run: CreateRestrictedToken keeps the elevated runner's High,
    // and a High process refuses a Medium one (another account's helper) write access such as PROCESS_TERMINATE
    // whatever its DACL says.
    std::vector<std::byte> medium = wellKnownSid(WinMediumLabelSid);
    TOKEN_MANDATORY_LABEL label{.Label = {.Sid = medium.data(), .Attributes = SE_GROUP_INTEGRITY}};
    const auto labelSize = static_cast<DWORD>(sizeof(label) + medium.size());
    if (medium.empty() || SetTokenInformation(token.get(), TokenIntegrityLevel, &label, labelSize) == FALSE)
    {
        ADD_FAILURE() << "couldn't lower the restricted token to Medium integrity: " << GetLastError();
        return {};
    }
    return token;
}

/// Adds an ACE giving @p sid @p rights to kernel object @p object's DACL (a process, or a token), keeping what is
/// there. False, with a failure added, when it can't.
inline bool letIn(HANDLE object, std::vector<std::byte> sid, DWORD rights)
{
    PACL existing = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (GetSecurityInfo(object, SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &existing, nullptr, &descriptor) !=
        ERROR_SUCCESS)
    {
        ADD_FAILURE() << "couldn't read an object's DACL";
        return false;
    }
    EXPLICIT_ACCESS_W entry{};
    entry.grfAccessPermissions = rights;
    entry.grfAccessMode = GRANT_ACCESS;
    entry.grfInheritance = NO_INHERITANCE;
    entry.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    entry.Trustee.TrusteeType = TRUSTEE_IS_UNKNOWN;
    // TRUSTEE_IS_SID: ptstrName holds the SID, as the API documents.
    entry.Trustee.ptstrName = reinterpret_cast<LPWSTR>(sid.data()); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
    PACL updated = nullptr;
    const DWORD merged = SetEntriesInAclW(1, &entry, existing, &updated);
    const DWORD set = (merged == ERROR_SUCCESS)
                        ? SetSecurityInfo(object, SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, updated, nullptr)
                        : merged;
    LocalFree(static_cast<HLOCAL>(updated));
    LocalFree(descriptor);
    EXPECT_EQ(set, static_cast<DWORD>(ERROR_SUCCESS)) << "couldn't widen an object's DACL";
    return set == ERROR_SUCCESS;
}

/// A process the test started: ended (TerminateProcess) and waited for on scope exit if it is still running.
class Child
{
  public:
    Child() = default;
    Child(HANDLE process, HANDLE thread, DWORD pid) : m_Process(process), m_Thread(thread), m_Pid(pid)
    {}
    ~Child()
    {
        end();
    }
    Child(const Child&) = delete;
    Child& operator=(const Child&) = delete;
    Child(Child&&) noexcept = default;
    Child& operator=(Child&&) noexcept = default;

    [[nodiscard]] HANDLE process() const noexcept
    {
        return m_Process.get();
    }
    [[nodiscard]] HANDLE thread() const noexcept
    {
        return m_Thread.get();
    }
    [[nodiscard]] DWORD pid() const noexcept
    {
        return m_Pid;
    }
    [[nodiscard]] Platform::ProcessTarget target() const
    {
        return {.pid = static_cast<std::int32_t>(m_Pid), .startTimeTicks = startTicksOf(m_Process.get())};
    }
    /// Its exit status once it has exited within @p timeout; nullopt while it still runs.
    [[nodiscard]] std::optional<DWORD> exitCode(std::chrono::milliseconds timeout) const
    {
        if (!m_Process || WaitForSingleObject(m_Process.get(), static_cast<DWORD>(timeout.count())) != WAIT_OBJECT_0)
        {
            return std::nullopt;
        }
        DWORD code = 0;
        GetExitCodeProcess(m_Process.get(), &code);
        return code;
    }
    /// Lets a child started suspended run.
    void resume() const noexcept
    {
        if (m_Thread)
        {
            ResumeThread(m_Thread.get());
        }
    }
    /// Ends it, if it still runs, and waits for it.
    void end() noexcept
    {
        if (m_Process && WaitForSingleObject(m_Process.get(), 0) == WAIT_TIMEOUT)
        {
            TerminateProcess(m_Process.get(), 1);
            WaitForSingleObject(m_Process.get(), static_cast<DWORD>(STARTUP_WAIT.count()));
        }
    }

  private:
    UniqueHandle m_Process;
    UniqueHandle m_Thread;
    DWORD m_Pid = 0;
};

/// How start() starts a child.
struct StartOptions
{
    HANDLE token = nullptr;  ///< CreateProcessAsUserW with this primary token, or this process's own
    bool suspended = false;  ///< CREATE_SUSPENDED: it never runs a line of its own
    std::wstring parameters; ///< after the quoted exe
};

/// @p exe started as @p options say, with no window. An empty Child, with a failure added, when it can't be.
[[nodiscard]] inline Child start(const std::filesystem::path& exe, const StartOptions& options = {})
{
    std::wstring commandLine = L"\"" + exe.wstring() + L"\"";
    if (!options.parameters.empty())
    {
        commandLine += L" " + options.parameters;
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info{};
    const DWORD flags = CREATE_NO_WINDOW | (options.suspended ? CREATE_SUSPENDED : 0U);
    const BOOL created =
        (options.token != nullptr)
            ? CreateProcessAsUserW(
                  options.token, exe.c_str(), commandLine.data(), nullptr, nullptr, FALSE, flags, nullptr, nullptr, &startup, &info)
            : CreateProcessW(exe.c_str(), commandLine.data(), nullptr, nullptr, FALSE, flags, nullptr, nullptr, &startup, &info);
    if (created == FALSE)
    {
        ADD_FAILURE() << "couldn't start " << Core::pathToUtf8(exe) << ": " << GetLastError();
        return {};
    }
    return {info.hProcess, info.hThread, info.dwProcessId};
}

/// cmd.exe from System32, which every Windows has.
[[nodiscard]] inline std::filesystem::path systemCmd()
{
    std::array<wchar_t, MAX_PATH> system{};
    const UINT length = GetSystemDirectoryW(system.data(), static_cast<UINT>(system.size()));
    return std::filesystem::path(std::wstring(system.data(), length)) / L"cmd.exe";
}

/// @p process's image, as a Win32 path; empty when it can't be read.
[[nodiscard]] inline std::wstring imageOf(HANDLE process)
{
    std::array<wchar_t, 32'768> path{};
    auto size = static_cast<DWORD>(path.size());
    if (QueryFullProcessImageNameW(process, 0, path.data(), &size) == FALSE)
    {
        return {};
    }
    return {path.data(), size};
}

/// Whether @p first and @p second are the same path, ignoring case as NTFS does.
[[nodiscard]] inline bool samePath(std::wstring_view first, std::wstring_view second)
{
    return CompareStringOrdinal(first.data(), static_cast<int>(first.size()), second.data(), static_cast<int>(second.size()), TRUE) ==
           CSTR_EQUAL;
}

/// The PIDs of the running processes whose executable file is named @p exeName (Toolhelp's szExeFile).
[[nodiscard]] inline std::vector<DWORD> pidsNamed(std::wstring_view exeName)
{
    std::vector<DWORD> pids;
    const UniqueHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snapshot || snapshot.get() == INVALID_HANDLE_VALUE)
    {
        return pids;
    }
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL more = Process32FirstW(snapshot.get(), &entry); more != FALSE; more = Process32NextW(snapshot.get(), &entry))
    {
        if (samePath(&entry.szExeFile[0], exeName))
        {
            pids.push_back(entry.th32ProcessID);
        }
    }
    return pids;
}

/// The process running @p exe, found within @p timeout and opened with @p access: one the test didn't start
/// itself (a shell launch hands back no process), told apart by its unique path. Null when there is none.
[[nodiscard]] inline UniqueHandle findProcessRunning(const std::filesystem::path& exe, DWORD access, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (bool first = true; first || std::chrono::steady_clock::now() < deadline; first = false)
    {
        for (const DWORD pid : pidsNamed(exe.filename().wstring()))
        {
            UniqueHandle process(OpenProcess(access | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
            if (process && samePath(imageOf(process.get()), exe.wstring()))
            {
                return process;
            }
        }
        std::this_thread::sleep_for(POLL_INTERVAL);
    }
    return {};
}

/// Ends @p process if it still runs, on scope exit: what a test found rather than started.
class EndOnExit
{
  public:
    explicit EndOnExit(HANDLE process) noexcept : m_Process(process)
    {}
    ~EndOnExit()
    {
        if (m_Process != nullptr && WaitForSingleObject(m_Process, 0) == WAIT_TIMEOUT)
        {
            TerminateProcess(m_Process, 1);
            WaitForSingleObject(m_Process, static_cast<DWORD>(STARTUP_WAIT.count()));
        }
    }
    EndOnExit(const EndOnExit&) = delete;
    EndOnExit& operator=(const EndOnExit&) = delete;
    EndOnExit(EndOnExit&&) = delete;
    EndOnExit& operator=(EndOnExit&&) = delete;

  private:
    HANDLE m_Process;
};

/// A folder of the test's own, removed with what is in it on scope exit (retried a moment: Windows may hold an
/// image that just exited a little longer).
class ScratchFolder
{
  public:
    explicit ScratchFolder(std::filesystem::path path) : m_Path(std::move(path))
    {
        std::error_code error;
        std::filesystem::create_directories(m_Path, error);
        EXPECT_FALSE(error) << "couldn't make " << Core::pathToUtf8(m_Path) << ": " << error.message();
    }
    ~ScratchFolder()
    {
        try
        {
            constexpr int ATTEMPTS = 100;
            std::error_code error;
            for (int attempt = 0; attempt < ATTEMPTS && std::filesystem::exists(m_Path, error); ++attempt)
            {
                std::filesystem::remove_all(m_Path, error);
                if (error)
                {
                    std::this_thread::sleep_for(POLL_INTERVAL);
                }
            }
        }
        catch (...) // NOLINT(bugprone-empty-catch) - out of memory: the folder stays on the throwaway runner
        {}
    }
    ScratchFolder(const ScratchFolder&) = delete;
    ScratchFolder& operator=(const ScratchFolder&) = delete;
    ScratchFolder(ScratchFolder&&) = delete;
    ScratchFolder& operator=(ScratchFolder&&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept
    {
        return m_Path;
    }

  private:
    std::filesystem::path m_Path;
};

/// @p from copied to @p to; a failure added when it can't be.
inline void copyFile(const std::filesystem::path& from, const std::filesystem::path& to)
{
    std::error_code error;
    std::filesystem::copy_file(from, to, std::filesystem::copy_options::overwrite_existing, error);
    EXPECT_FALSE(error) << "couldn't copy " << Core::pathToUtf8(from) << " to " << Core::pathToUtf8(to) << ": " << error.message();
}

/// SetNamedSecurityInfoW(@p path): owner Administrators and a protected DACL of SYSTEM and Administrators full
/// control and Users read and execute, inherited by what is under it: what an installer gives Program Files\TaskSmack.
inline void protectAsAnInstall(const std::filesystem::path& path)
{
    std::vector<std::byte> administrators = wellKnownSid(WinBuiltinAdministratorsSid);
    constexpr DWORD INHERITED = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
    const Acl acl({
        {.sid = wellKnownSid(WinLocalSystemSid), .rights = GENERIC_ALL, .inheritance = INHERITED},
        {.sid = administrators, .rights = GENERIC_ALL, .inheritance = INHERITED},
        {.sid = wellKnownSid(WinBuiltinUsersSid), .rights = GENERIC_READ | GENERIC_EXECUTE, .inheritance = INHERITED},
    });
    ASSERT_NE(acl.get(), nullptr) << "couldn't make the install's ACL: " << GetLastError();
    std::wstring name = path.wstring();
    const DWORD set = SetNamedSecurityInfoW(name.data(),
                                            SE_FILE_OBJECT,
                                            OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                                            administrators.data(),
                                            nullptr,
                                            acl.get(),
                                            nullptr);
    ASSERT_EQ(set, static_cast<DWORD>(ERROR_SUCCESS)) << "couldn't protect " << Core::pathToUtf8(path);
}

/// %ProgramFiles%.
[[nodiscard]] inline std::filesystem::path programFiles()
{
    std::array<wchar_t, MAX_PATH> path{};
    const DWORD length = GetEnvironmentVariableW(L"ProgramFiles", path.data(), static_cast<DWORD>(path.size()));
    return (length == 0 || length >= path.size()) ? std::filesystem::path(L"C:\\Program Files")
                                                  : std::filesystem::path(std::wstring(path.data(), length));
}

/// This test binary's own path, which a test starts again as its child (with another token, say).
[[nodiscard]] inline std::filesystem::path ownExecutable()
{
    std::array<wchar_t, 32'768> path{};
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    return {std::wstring(path.data(), length)};
}

/// Sets environment variable @p name for this process (so a child started now inherits it) until scope exit.
class ScopedVariable
{
  public:
    ScopedVariable(std::wstring name, const std::wstring& value) : m_Name(std::move(name))
    {
        EXPECT_NE(SetEnvironmentVariableW(m_Name.c_str(), value.c_str()), FALSE) << GetLastError();
    }
    ~ScopedVariable()
    {
        SetEnvironmentVariableW(m_Name.c_str(), nullptr);
    }
    ScopedVariable(const ScopedVariable&) = delete;
    ScopedVariable& operator=(const ScopedVariable&) = delete;
    ScopedVariable(ScopedVariable&&) = delete;
    ScopedVariable& operator=(ScopedVariable&&) = delete;

  private:
    std::wstring m_Name;
};

} // namespace TestSupport::CiRealSystem

#endif // _WIN32
