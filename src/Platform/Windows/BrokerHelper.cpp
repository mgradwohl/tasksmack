#include "BrokerHelper.h"

#include "Platform/IProcessActions.h"
#include "Platform/Windows/BrokerPipe.h"
#include "Platform/Windows/BrokerPipeServer.h"
#include "Platform/Windows/WindowsHandles.h"
#include "Platform/Windows/WindowsNtQuery.h"
#include "Platform/Windows/WindowsProcessIdentity.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

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
    return Self{.taskSmackPath = std::move(taskSmack), .creatorPid = *creator, .startTicks = *ticks};
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
    return {};
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
        return "the helper didn't accept TaskSmack as the process that started it";
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
        return serveParent(parent->get(), *user, arguments.pipeName, handler, idleTimeout, acceptTimeout, stopToken);
    }
    catch (...) // NOLINT(bugprone-empty-catch) - an allocation failure or the like: TaskSmack sees the pipe close
    {}
    return Exit::Failed;
}

Exit serveParent(HANDLE parent,
                 const BrokerPipe::Sid& user,
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
        switch (BrokerPipeServer::serve(pipe->get(), handler, idleTimeout, watch.token()))
        {
        case BrokerPipeServer::SessionEnd::ClientClosed:
        case BrokerPipeServer::SessionEnd::Idle:
        case BrokerPipeServer::SessionEnd::Stopped:
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

} // namespace Platform::Windows::BrokerHelper
