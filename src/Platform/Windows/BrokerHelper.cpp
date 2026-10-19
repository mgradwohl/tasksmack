#include "BrokerHelper.h"

#include "Platform/BrokerProtocol.h"
#include "Platform/IProcessActions.h"
#include "Platform/IProcessConnections.h"
#include "Platform/IProcessEnvironment.h"
#include "Platform/IProcessModules.h"
#include "Platform/IProcessOpenFiles.h"
#include "Platform/IProcessProbe.h"
#include "Platform/Windows/BrokerPipe.h"
#include "Platform/Windows/BrokerPipeServer.h"
#include "Platform/Windows/WinString.h"
#include "Platform/Windows/WindowsHandles.h"
#include "Platform/Windows/WindowsNtQuery.h"
#include "Platform/Windows/WindowsProcessIdentity.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <aclapi.h>
#include <winternl.h>

namespace Platform::Windows::BrokerHelper
{

namespace
{

constexpr std::wstring_view BROKER_FLAG = L"--broker";

/// @p text as an unsigned decimal number no larger than @p max and not 0: digits only, no sign, no
/// spaces, no leading zero, so each number has exactly one spelling.
[[nodiscard]] std::expected<std::uint64_t, std::string> parsePositive(std::wstring_view text, std::uint64_t max, std::string_view what)
{
    constexpr std::size_t MAX_DIGITS = 20; // std::uint64_t's largest has 20
    if (text.empty() || text.size() > MAX_DIGITS || text.front() == L'0')
    {
        return std::unexpected(std::format("{} isn't a positive decimal number", what));
    }
    std::uint64_t value = 0;
    constexpr std::uint64_t BASE = 10;
    for (const wchar_t c : text)
    {
        if (c < L'0' || c > L'9')
        {
            return std::unexpected(std::format("{} isn't a positive decimal number", what));
        }
        const auto digit = static_cast<std::uint64_t>(c - L'0');
        if (value > (max - digit) / BASE)
        {
            return std::unexpected(std::format("{} is out of range", what));
        }
        value = (value * BASE) + digit;
    }
    return value;
}

[[nodiscard]] std::string lastErrorText(DWORD error)
{
    return std::system_category().message(static_cast<int>(error));
}

/// The longest path the wide Win32 calls return, in characters (an extended-length path).
constexpr DWORD LONGEST_PATH = 32'768;

/// A path a Win32 call writes into a buffer of the size it is given, @p read(buffer, size) returning
/// the characters written (0 on failure, or @p size or more when the buffer was too small): MAX_PATH
/// first, then the longest path. nullopt when it fails or doesn't fit.
template<typename Read> [[nodiscard]] std::optional<std::wstring> readPath(Read read)
{
    for (const DWORD size : {DWORD{MAX_PATH}, LONGEST_PATH})
    {
        std::wstring path(size, L'\0');
        const DWORD written = read(path.data(), size);
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

/// @p process's image, as a Win32 path.
[[nodiscard]] std::optional<std::wstring> imageOf(HANDLE process)
{
    return readPath(
        [process](wchar_t* buffer, DWORD size) -> DWORD
        {
            DWORD written = size;
            return QueryFullProcessImageNameW(process, 0, buffer, &written) != FALSE ? written : 0;
        });
}

/// The final path of the file at @p path: what it names once "..", short names, links and case are
/// resolved. nullopt when it can't be opened.
[[nodiscard]] std::optional<std::wstring> finalPathOf(const std::wstring& path)
{
    // No access asked beyond its attributes, every sharing mode: a running executable opens like
    // this, and nothing is read from it.
    const UniqueHandle file(CreateFileW(path.c_str(),
                                        FILE_READ_ATTRIBUTES,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                        nullptr,
                                        OPEN_EXISTING,
                                        FILE_ATTRIBUTE_NORMAL,
                                        nullptr));
    if (!file)
    {
        return std::nullopt;
    }
    return readPath([&file](wchar_t* buffer, DWORD size)
                    { return GetFinalPathNameByHandleW(file.get(), buffer, size, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS); });
}

/// @p path without the prefix GetFinalPathNameByHandleW gives it: "C:\dir" for "\\?\C:\dir", and
/// "\\server\share" for "\\?\UNC\server\share", so std::filesystem finds its root and each folder above.
[[nodiscard]] std::wstring withoutExtendedPrefix(const std::wstring& path)
{
    constexpr std::wstring_view UNC = L"\\\\?\\UNC\\";
    constexpr std::wstring_view EXTENDED = L"\\\\?\\";
    if (path.starts_with(UNC))
    {
        return L"\\\\" + path.substr(UNC.size());
    }
    if (path.starts_with(EXTENDED))
    {
        return path.substr(EXTENDED.size());
    }
    return path;
}

/// The rights @p token's user has over the file or folder at @p path, from AccessCheck against its
/// owner, group and DACL. The error says why they couldn't be told.
[[nodiscard]] std::expected<DWORD, std::string> grantedRights(HANDLE token, const std::wstring& path)
{
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const DWORD read = GetNamedSecurityInfoW(path.c_str(),
                                             SE_FILE_OBJECT,
                                             OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                                             nullptr,
                                             nullptr,
                                             nullptr,
                                             nullptr,
                                             &descriptor);
    if (read != ERROR_SUCCESS)
    {
        return std::unexpected(std::format("couldn't read who may change {}: {}", WinString::wideToUtf8(path), lastErrorText(read)));
    }
    struct Free
    {
        void operator()(void* memory) const noexcept
        {
            LocalFree(memory);
        }
    };
    const std::unique_ptr<void, Free> owned(descriptor);
    GENERIC_MAPPING mapping{.GenericRead = FILE_GENERIC_READ,
                            .GenericWrite = FILE_GENERIC_WRITE,
                            .GenericExecute = FILE_GENERIC_EXECUTE,
                            .GenericAll = FILE_ALL_ACCESS};
    // Room for the privileges AccessCheck may name; MAXIMUM_ALLOWED asks for none.
    struct Privileges
    {
        PRIVILEGE_SET set;
        std::array<LUID_AND_ATTRIBUTES, 8> more;
    } privileges{};
    DWORD privilegesLength = sizeof(privileges);
    DWORD granted = 0;
    BOOL status = FALSE;
    if (AccessCheck(descriptor, token, MAXIMUM_ALLOWED, &mapping, &privileges.set, &privilegesLength, &granted, &status) == FALSE)
    {
        return std::unexpected(
            std::format("couldn't tell who may change {}: {}", WinString::wideToUtf8(path), lastErrorText(GetLastError())));
    }
    return status != FALSE ? granted : DWORD{0};
}

/// Whether @p token's user has none of @p rights over @p path; the error names it otherwise.
[[nodiscard]] std::expected<void, std::string> checkNone(HANDLE token, const std::filesystem::path& path, DWORD rights)
{
    const auto granted = grantedRights(token, path.wstring());
    if (!granted.has_value())
    {
        return std::unexpected(granted.error());
    }
    if ((*granted & rights) != 0)
    {
        return std::unexpected(
            std::format("TaskSmack's user can change {} (rights 0x{:08X})", WinString::wideToUtf8(path.wstring()), *granted & rights));
    }
    return {};
}

/// NtQueryInformationProcess(ProcessBasicInformation)'s PROCESS_BASIC_INFORMATION, its fields named:
/// winternl.h calls the parent's PID Reserved3.
struct BasicInformation
{
    LONG exitStatus = 0;
    PVOID pebBaseAddress = nullptr;
    ULONG_PTR affinityMask = 0;
    LONG basePriority = 0;
    ULONG_PTR uniqueProcessId = 0;
    ULONG_PTR inheritedFromUniqueProcessId = 0;
};
static_assert(sizeof(BasicInformation) == sizeof(PROCESS_BASIC_INFORMATION));

using NtQueryInformationProcessFn =
    LONG(NTAPI*)(HANDLE process, ULONG informationClass, PVOID information, ULONG length, PULONG returnLength);
constexpr ULONG PROCESS_BASIC_INFORMATION_CLASS = 0; // ProcessBasicInformation

/// The PID of the process that created this one, or nullopt if it can't be read.
[[nodiscard]] std::optional<DWORD> creatorPid()
{
    const auto query = ntdllProc<NtQueryInformationProcessFn>("NtQueryInformationProcess");
    if (query == nullptr)
    {
        return std::nullopt;
    }
    BasicInformation information{};
    ULONG written = 0;
    if (query(GetCurrentProcess(), PROCESS_BASIC_INFORMATION_CLASS, &information, sizeof(information), &written) < 0 ||
        written != sizeof(information) || information.inheritedFromUniqueProcessId > std::numeric_limits<DWORD>::max())
    {
        return std::nullopt;
    }
    return static_cast<DWORD>(information.inheritedFromUniqueProcessId);
}

/// @p process's creation time, in a FILETIME's ticks, or nullopt if it can't be read.
[[nodiscard]] std::optional<std::uint64_t> startTicksOf(HANDLE process)
{
    FILETIME creation{};
    FILETIME exitTime{};
    FILETIME kernelTime{};
    FILETIME userTime{};
    if (GetProcessTimes(process, &creation, &exitTime, &kernelTime, &userTime) == FALSE)
    {
        return std::nullopt;
    }
    return (static_cast<std::uint64_t>(creation.dwHighDateTime) << 32U) | creation.dwLowDateTime;
}

/// Why openProcessVerified() refused the parent, in the helper's words.
[[nodiscard]] std::string parentRefusal(const IdentityFailure& failure)
{
    switch (failure.kind)
    {
    case IdentityFailureKind::InvalidPid:
    case IdentityFailureKind::IdentityUnknown:
        return "TaskSmack's process wasn't named";
    case IdentityFailureKind::ProcessGone:
    case IdentityFailureKind::Exited:
        return "TaskSmack has exited";
    case IdentityFailureKind::Mismatch:
        return "the process with TaskSmack's PID isn't the TaskSmack that started the helper";
    case IdentityFailureKind::AccessDenied:
    case IdentityFailureKind::OpenFailed:
    case IdentityFailureKind::TimesFailed:
        break;
    }
    return std::format("couldn't open TaskSmack's process: {}", lastErrorText(failure.win32Error));
}

} // namespace

std::expected<Arguments, std::string> parseArguments(std::span<const std::wstring_view> arguments)
{
    constexpr std::size_t COUNT = 4;
    if (arguments.size() != COUNT || arguments[0] != BROKER_FLAG)
    {
        return std::unexpected(std::string("usage: --broker <pipe name> <parent PID> <parent start time>"));
    }
    if (!BrokerPipe::isPipeName(arguments[1]))
    {
        return std::unexpected(std::string("the pipe name isn't one TaskSmack makes"));
    }
    const auto pid = parsePositive(arguments[2], std::numeric_limits<DWORD>::max(), "the parent's PID");
    if (!pid.has_value())
    {
        return std::unexpected(pid.error());
    }
    const auto ticks = parsePositive(arguments[3], std::numeric_limits<std::uint64_t>::max(), "the parent's start time");
    if (!ticks.has_value())
    {
        return std::unexpected(ticks.error());
    }
    return Arguments{.pipeName = std::wstring(arguments[1]), .parentPid = static_cast<DWORD>(*pid), .parentStartTicks = *ticks};
}

std::wstring commandLine(const Arguments& arguments)
{
    return std::format(L"{} {} {} {}", BROKER_FLAG, arguments.pipeName, arguments.parentPid, arguments.parentStartTicks);
}

std::expected<UniqueHandle, std::string> openParent(DWORD pid, std::uint64_t startTicks)
{
    if (pid == 0 || pid > static_cast<DWORD>(std::numeric_limits<std::int32_t>::max()))
    {
        return std::unexpected(std::string("TaskSmack's process wasn't named"));
    }
    // The one identity check every action uses (#2149): the creation time is read from the handle
    // opened, so the process checked is the process held.
    auto process = openProcessVerified({}, ProcessTarget{.pid = static_cast<std::int32_t>(pid), .startTimeTicks = startTicks}, SYNCHRONIZE);
    if (!process.has_value())
    {
        return std::unexpected(parentRefusal(process.error()));
    }
    return UniqueHandle(process->release());
}

std::wstring besidePath(std::wstring_view exePath, std::wstring_view fileName)
{
    const std::filesystem::path exe(exePath);
    if (!exe.is_absolute() || !exe.has_filename() || !exe.has_relative_path())
    {
        return {};
    }
    return (exe.parent_path() / fileName).wstring();
}

bool sameFile(const std::wstring& first, const std::wstring& second)
{
    const auto firstPath = finalPathOf(first);
    const auto secondPath = finalPathOf(second);
    if (!firstPath.has_value() || !secondPath.has_value())
    {
        return false;
    }
    // CompareStringOrdinal folds case with the system's upper-case table, as NTFS compares names.
    return CompareStringOrdinal(
               firstPath->c_str(), static_cast<int>(firstPath->size()), secondPath->c_str(), static_cast<int>(secondPath->size()), TRUE) ==
           CSTR_EQUAL;
}

std::expected<void, std::string> checkUnchangeableBy(HANDLE token, std::span<const std::wstring> files)
{
    if (files.empty())
    {
        return std::unexpected(std::string("no install to check"));
    }
    for (const std::wstring& file : files)
    {
        // The file itself, links resolved: the folders checked are where it really is.
        const auto final = finalPathOf(file);
        if (!final.has_value())
        {
            return std::unexpected(std::format("couldn't open {}", WinString::wideToUtf8(file)));
        }
        const std::filesystem::path path(withoutExtendedPrefix(*final));
        if (auto checked = checkNone(token, path, FILE_CHANGE_RIGHTS); !checked.has_value())
        {
            return checked;
        }
        std::filesystem::path folder = path.parent_path();
        if (auto checked = checkNone(token, folder, FOLDER_CHANGE_RIGHTS); !checked.has_value())
        {
            return checked;
        }
        while (folder.has_relative_path())
        {
            folder = folder.parent_path();
            if (auto checked = checkNone(token, folder, ANCESTOR_CHANGE_RIGHTS); !checked.has_value())
            {
                return checked;
            }
        }
    }
    return {};
}

std::expected<void, std::string> checkUnchangeableByParent(HANDLE parent, std::span<const std::wstring> files)
{
    HANDLE primary = nullptr;
    if (OpenProcessToken(parent, TOKEN_QUERY | TOKEN_DUPLICATE, &primary) == FALSE)
    {
        return std::unexpected(std::format("couldn't open TaskSmack's token: {}", lastErrorText(GetLastError())));
    }
    const UniqueHandle primaryToken(primary);
    HANDLE identification = nullptr;
    if (DuplicateToken(primaryToken.get(), SecurityIdentification, &identification) == FALSE)
    {
        return std::unexpected(std::format("couldn't copy TaskSmack's token: {}", lastErrorText(GetLastError())));
    }
    const UniqueHandle identificationToken(identification);
    return checkUnchangeableBy(identificationToken.get(), files);
}

std::expected<Self, std::string> readSelf()
{
    const auto image = readPath([](wchar_t* buffer, DWORD size) { return GetModuleFileNameW(nullptr, buffer, size); });
    if (!image.has_value())
    {
        return std::unexpected(std::format("couldn't read the helper's own path: {}", lastErrorText(GetLastError())));
    }
    std::wstring taskSmack = besidePath(*image, TASKSMACK_FILE_NAME);
    if (taskSmack.empty())
    {
        return std::unexpected(std::string("the helper's own path has no folder"));
    }
    const auto creator = creatorPid();
    if (!creator.has_value())
    {
        return std::unexpected(std::string("couldn't tell which process started the helper"));
    }
    const auto ticks = startTicksOf(GetCurrentProcess());
    if (!ticks.has_value())
    {
        return std::unexpected(std::format("couldn't read the helper's start time: {}", lastErrorText(GetLastError())));
    }
    return Self{.taskSmackPath = std::move(taskSmack),
                .creatorPid = *creator,
                .startTicks = *ticks,
                .helperPath = *image,
                .installCheck = &checkUnchangeableByParent};
}

std::expected<void, std::string> checkParentIsTaskSmack(HANDLE parent, const Arguments& arguments, const Self& self)
{
    // The process that created the helper, which "runas" makes the caller of ShellExecuteExW
    // (BrokerHelper.h), must be the one named: no program can name a TaskSmack and start the helper itself.
    if (self.creatorPid != arguments.parentPid)
    {
        return std::unexpected(
            std::format("process {} started the helper, not the TaskSmack named ({})", self.creatorPid, arguments.parentPid));
    }
    // A PID is only a number: the helper's creator may have exited since, and its PID gone to a later
    // process. The one named, held and checked against its start time by openParent(), started before
    // the helper did if it is the creator.
    if (arguments.parentStartTicks > self.startTicks)
    {
        return std::unexpected(std::string("the process named started after the helper, so didn't start it"));
    }
    const auto image = imageOf(parent);
    if (!image.has_value())
    {
        return std::unexpected(std::format("couldn't read the parent's executable: {}", lastErrorText(GetLastError())));
    }
    if (self.taskSmackPath.empty() || !sameFile(*image, self.taskSmackPath))
    {
        return std::unexpected(std::string("the parent isn't the TaskSmack.exe beside the helper"));
    }
    // And that install is an administrator's: a TaskSmack.exe beside a copy of the helper in a folder
    // of the user's own isn't TaskSmack, whatever it is called (#2572 review, finding 1).
    if (self.installCheck == nullptr)
    {
        return std::unexpected(std::string("no check of the install"));
    }
    const std::array<std::wstring, 2> installed{self.helperPath, self.taskSmackPath};
    return self.installCheck(parent, installed);
}

SessionPolicy sessionPolicy(const BrokerPipe::Sid& user, const BrokerPipe::Sid& helperUser, std::chrono::milliseconds idleTimeout)
{
    if (user == helperUser)
    {
        return {.grant = BrokerPipeServer::Grant::Session, .idleTimeout = idleTimeout, .readsOwnProcessesOnly = false};
    }
    const std::chrono::milliseconds shortened = std::min<std::chrono::milliseconds>(idleTimeout, OVER_THE_SHOULDER_IDLE_TIMEOUT);
    return {.grant = BrokerPipeServer::Grant::OneAction, .idleTimeout = shortened, .readsOwnProcessesOnly = true};
}

bool runsAs(const ProcessTarget& target, const BrokerPipe::Sid& user)
{
    // The handle the token is read from is the one whose start time was checked, so the user read is
    // the target's. A process's user doesn't change while it runs.
    const auto process = openProcessVerified({}, target, 0);
    if (!process.has_value())
    {
        return false;
    }
    const auto owner = BrokerPipe::userSidOfProcess(process->get());
    return owner.has_value() && *owner == user;
}

BrokerPipeServer::RequestHandler readsOnlyOwnProcesses(BrokerPipeServer::RequestHandler handler,
                                                       std::function<bool(const ProcessTarget&)> ownedByUser)
{
    return [handler = std::move(handler), ownedByUser = std::move(ownedByUser)](const Broker::Request& request) -> Broker::Response
    {
        // The Hello and actions pass. Each per-process read below needs its target to be the user's,
        // and any other read is refused: fails closed (BrokerPipeServer::isRead() asserts every request
        // kind is sorted into a read or not).
        if (!BrokerPipeServer::isRead(request))
        {
            return handler(request);
        }
        const std::string refusal(OTHER_USERS_READ_REFUSED);
        const auto owned = [&ownedByUser](const ProcessTarget& target)
        {
            return ownedByUser && ownedByUser(target);
        };
        if (const auto* read = std::get_if<Broker::EnvironmentRequest>(&request))
        {
            if (owned(read->target))
            {
                return handler(request);
            }
            return Broker::EnvironmentReply{.result = {.status = EnvironmentReadStatus::Failed, .variables = {}}};
        }
        if (const auto* read = std::get_if<Broker::ModulesRequest>(&request))
        {
            if (owned(read->target))
            {
                return handler(request);
            }
            return Broker::ModulesReply{.result = {.status = ModulesReadStatus::Failed, .modules = {}, .detail = refusal}};
        }
        if (const auto* read = std::get_if<Broker::OpenFilesRequest>(&request))
        {
            if (owned(read->target))
            {
                return handler(request);
            }
            OpenFilesReadResult refused;
            refused.status = OpenFilesReadStatus::Failed;
            refused.detail = refusal;
            return Broker::OpenFilesReply{.result = std::move(refused)};
        }
        if (const auto* read = std::get_if<Broker::ConnectionsRequest>(&request))
        {
            if (owned(read->target))
            {
                return handler(request);
            }
            return Broker::ConnectionsReply{.result = {.status = ConnectionsReadStatus::Failed, .connections = {}, .detail = refusal}};
        }
        if (const auto* read = std::get_if<Broker::MemoryRollupRequest>(&request))
        {
            if (owned(read->target))
            {
                return handler(request);
            }
            return Broker::MemoryRollupReply{.result = std::unexpected(MemoryRollupFailure::Unavailable)};
        }
        // System-wide reads (firmware, every user's crashes), and any read kind not named above.
        return Broker::ErrorReply{.message = refusal};
    };
}

std::string exitMeaning(DWORD code)
{
    switch (code)
    {
    case static_cast<DWORD>(Exit::Done):
        return "the helper ended its session";
    case static_cast<DWORD>(Exit::Failed):
        return "the helper failed";
    case static_cast<DWORD>(Exit::Usage):
        return "the helper didn't understand its command line";
    case static_cast<DWORD>(Exit::NoParent):
        // Most often a TaskSmack its user can change (#2572): a portable or per-user copy.
        return "the helper didn't accept TaskSmack as the process that started it: it serves only a TaskSmack installed where "
               "only an administrator can change it, such as Program Files";
    case static_cast<DWORD>(Exit::NoClient):
        return "the helper couldn't make its pipe";
    default:
        break;
    }
    return std::format("the helper exited with status {}", code);
}

ProcessWatch::ProcessWatch(HANDLE process, const std::stop_token& outer)
    : m_Wake(CreateEventW(nullptr, TRUE, FALSE, nullptr)), m_Outer(outer, [this] { m_Stop.request_stop(); })
{
    if (!m_Wake)
    {
        throw std::runtime_error(std::format("couldn't watch TaskSmack's process: {}", lastErrorText(GetLastError())));
    }
    m_Thread = std::jthread(
        [this, process]
        {
            const std::array<HANDLE, 2> handles{process, m_Wake.get()};
            // Either ends the wait: the process (or a failed wait) stops the session, the wake ends this thread.
            if (WaitForMultipleObjects(static_cast<DWORD>(handles.size()), handles.data(), FALSE, INFINITE) != WAIT_OBJECT_0 + 1)
            {
                m_Stop.request_stop();
            }
        });
}

ProcessWatch::~ProcessWatch()
{
    SetEvent(m_Wake.get()); // m_Thread, joined next, returns at once
}

Exit run(const Arguments& arguments,
         const Self& self,
         const BrokerPipeServer::RequestHandler& handler,
         std::chrono::milliseconds idleTimeout,
         std::chrono::milliseconds acceptTimeout,
         const std::stop_token& stopToken)
{
    try
    {
        // TaskSmack's process, held from here to the end, so no other process can take its PID.
        const auto parent = openParent(arguments.parentPid, arguments.parentStartTicks);
        if (!parent.has_value())
        {
            return Exit::NoParent;
        }
        // That it is TaskSmack, and the one that started this helper (#2549).
        if (!checkParentIsTaskSmack(parent->get(), arguments, self).has_value())
        {
            return Exit::NoParent;
        }
        // Whom the pipe lets in: TaskSmack's user, from its token, which needn't be the helper's own.
        const auto user = BrokerPipe::userSidOfProcess(parent->get());
        if (!user.has_value())
        {
            return Exit::NoParent;
        }
        // And the helper's own account, to tell an over-the-shoulder elevation (#2550).
        const auto helperUser = BrokerPipe::userSidOfProcess(GetCurrentProcess());
        if (!helperUser.has_value())
        {
            return Exit::Failed;
        }
        return serveParent(parent->get(), *user, *helperUser, arguments.pipeName, handler, idleTimeout, acceptTimeout, stopToken);
    }
    catch (...) // NOLINT(bugprone-empty-catch) - an allocation failure or the like: TaskSmack sees the pipe close
    {}
    return Exit::Failed;
}

Exit serveParent(HANDLE parent,
                 const BrokerPipe::Sid& user,
                 const BrokerPipe::Sid& helperUser,
                 const std::wstring& pipeName,
                 const BrokerPipeServer::RequestHandler& handler,
                 std::chrono::milliseconds idleTimeout,
                 std::chrono::milliseconds acceptTimeout,
                 const std::stop_token& stopToken)
{
    try
    {
        // A parent running as a service, LocalSystem or a group isn't one user's TaskSmack: refused as
        // the parent it is, before any pipe is made, not reported as a client that never came.
        if (!BrokerPipe::checkUserSid(user).has_value())
        {
            return Exit::NoParent;
        }
        const ProcessWatch watch(parent, stopToken);
        const auto pipe = BrokerPipe::createServerPipe(pipeName, user);
        if (!pipe.has_value())
        {
            return Exit::NoClient;
        }
        if (!BrokerPipe::acceptClient(pipe->get(), parent, acceptTimeout, watch.token()).has_value())
        {
            return Exit::NoClient;
        }
        // An over-the-shoulder elevation gets one action and its own user's processes' reads (#2550).
        const SessionPolicy policy = sessionPolicy(user, helperUser, idleTimeout);
        const BrokerPipeServer::RequestHandler served =
            policy.readsOwnProcessesOnly
                ? readsOnlyOwnProcesses(handler, [&user](const ProcessTarget& target) { return runsAs(target, user); })
                : handler;
        switch (BrokerPipeServer::serve(pipe->get(), served, policy.idleTimeout, watch.token(), {}, policy.grant))
        {
        case BrokerPipeServer::SessionEnd::ClientClosed:
        case BrokerPipeServer::SessionEnd::Idle:
        case BrokerPipeServer::SessionEnd::Stopped:
        case BrokerPipeServer::SessionEnd::ActionDone:
            return Exit::Done;
        case BrokerPipeServer::SessionEnd::HandshakeFailed:
        case BrokerPipeServer::SessionEnd::Broken:
            break;
        }
    }
    catch (...) // NOLINT(bugprone-empty-catch) - an allocation failure or the like: TaskSmack sees the pipe close
    {}
    return Exit::Failed;
}

std::vector<std::string_view> applyMitigations(const SetMitigationPolicy& set)
{
    std::vector<std::string_view> refused;
    const auto apply = [&set, &refused](PROCESS_MITIGATION_POLICY policy, auto value, std::string_view name)
    {
        if (!set(policy, &value, sizeof(value)))
        {
            refused.push_back(name);
        }
    };

    // NOLINTBEGIN(cppcoreguidelines-pro-type-union-access) - each policy is a union of its bitfields and Flags
    PROCESS_MITIGATION_IMAGE_LOAD_POLICY imageLoad{};
    imageLoad.NoRemoteImages = 1;
    imageLoad.NoLowMandatoryLabelImages = 1;
    imageLoad.PreferSystem32Images = 1;
    apply(ProcessImageLoadPolicy, imageLoad, "image load");

    PROCESS_MITIGATION_EXTENSION_POINT_DISABLE_POLICY extensionPoints{};
    extensionPoints.DisableExtensionPoints = 1;
    apply(ProcessExtensionPointDisablePolicy, extensionPoints, "extension points");

    PROCESS_MITIGATION_STRICT_HANDLE_CHECK_POLICY strictHandles{};
    strictHandles.RaiseExceptionOnInvalidHandleReference = 1;
    strictHandles.HandleExceptionsPermanentlyEnabled = 1;
    apply(ProcessStrictHandleCheckPolicy, strictHandles, "strict handle checks");

    PROCESS_MITIGATION_DYNAMIC_CODE_POLICY dynamicCode{};
    dynamicCode.ProhibitDynamicCode = 1;
    apply(ProcessDynamicCodePolicy, dynamicCode, "dynamic code");

    PROCESS_MITIGATION_FONT_DISABLE_POLICY fonts{};
    fonts.DisableNonSystemFonts = 1;
    apply(ProcessFontDisablePolicy, fonts, "non-system fonts");

    PROCESS_MITIGATION_REDIRECTION_TRUST_POLICY redirectionTrust{};
    redirectionTrust.EnforceRedirectionTrust = 1;
    apply(ProcessRedirectionTrustPolicy, redirectionTrust, "redirection trust");
    // NOLINTEND(cppcoreguidelines-pro-type-union-access)

    return refused;
}

} // namespace Platform::Windows::BrokerHelper
