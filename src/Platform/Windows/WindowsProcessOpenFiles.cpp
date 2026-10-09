#include "WindowsProcessOpenFiles.h"

#include "Platform/IProcessActions.h"
#include "Platform/IProcessOpenFiles.h"
#include "Platform/ThreadName.h"
#include "WinString.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace Platform::Windows
{

namespace
{

constexpr LONG STATUS_INFO_LENGTH_MISMATCH_CODE = static_cast<LONG>(0xC0000004L);

/// The handle table's first buffer, and the largest it may grow to: a busy desktop holds a few hundred
/// thousand handles at 40 bytes each.
constexpr std::size_t INITIAL_SNAPSHOT_BYTES = std::size_t{4} << 20U;
constexpr std::size_t MAX_SNAPSHOT_BYTES = std::size_t{512} << 20U;

/// The longest path GetFinalPathNameByHandleW can return (an extended-length path), in characters.
constexpr DWORD MAX_FILE_PATH = 32768;

/// The longest one read waits on its namer, whatever each handle takes.
constexpr std::chrono::seconds READ_BUDGET{10};

[[nodiscard]] OpenFilesReadResult resultOf(OpenFilesReadStatus status, std::string detail = {})
{
    OpenFilesReadResult result;
    result.status = status;
    result.detail = std::move(detail);
    result.hexDescriptors = true;
    return result;
}

[[nodiscard]] std::string errorText(DWORD error)
{
    return std::system_category().message(static_cast<int>(error));
}

/// Closes a handle through the injected CloseHandle (a test's fake handle never reaches the real one).
struct InjectedHandleCloser
{
    ProcessOpenFilesFunctions::CloseHandleFn close = nullptr;

    void operator()(HANDLE handle) const noexcept
    {
        close(handle);
    }
};

using OwnedHandle = std::unique_ptr<std::remove_pointer_t<HANDLE>, InjectedHandleCloser>;

/// A handle listed but not queried: its type and name are unknown.
[[nodiscard]] OpenFile unqueried(const SystemHandleEntry& entry)
{
    return {.descriptor = entry.handleValue, .kind = OpenFileKind::Other, .path = {}, .flags = std::nullopt, .deleted = false};
}

/// "C:\dir\file" from GetFinalPathNameByHandleW's "\\?\C:\dir\file", and "\\server\share" from "\\?\UNC\server\share".
[[nodiscard]] std::wstring withoutExtendedPrefix(std::wstring path)
{
    if (path.starts_with(L"\\\\?\\UNC\\"))
    {
        return L"\\" + path.substr(7);
    }
    if (path.starts_with(L"\\\\?\\"))
    {
        return path.substr(4);
    }
    return path;
}

/// The path of the disk file @p handle (ours), DOS form when it has one, else the NT device path.
[[nodiscard]] std::string finalPath(const ProcessOpenFilesFunctions& api, HANDLE handle)
{
    std::wstring buffer(MAX_PATH, L'\0');
    for (const DWORD flags : {DWORD{FILE_NAME_NORMALIZED | VOLUME_NAME_DOS}, DWORD{FILE_NAME_NORMALIZED | VOLUME_NAME_NT}})
    {
        DWORD length = api.getFinalPathNameByHandle(handle, buffer.data(), static_cast<DWORD>(buffer.size()), flags);
        if (length >= buffer.size() && length < MAX_FILE_PATH)
        {
            buffer.resize(length + 1);
            length = api.getFinalPathNameByHandle(handle, buffer.data(), static_cast<DWORD>(buffer.size()), flags);
        }
        if (length > 0 && length < buffer.size())
        {
            return WinString::wideToUtf8(withoutExtendedPrefix(std::wstring(buffer.data(), length)));
        }
    }
    return {};
}

/// Types and, for a disk file, names @p entry, a handle in @p process, through a duplicate of it.
[[nodiscard]] OpenFile nameHandle(const ProcessOpenFilesFunctions& api, HANDLE process, const SystemHandleEntry& entry)
{
    OpenFile file = unqueried(entry);
    if (isHangProneAccess(entry.grantedAccess))
    {
        return file;
    }
    HANDLE duplicate = nullptr;
    // NOLINTNEXTLINE(performance-no-int-to-ptr) - a handle value from the handle table
    auto* const source = reinterpret_cast<HANDLE>(entry.handleValue);
    if (api.duplicateHandle(process, source, GetCurrentProcess(), &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS) == 0)
    {
        return file; // closed since the snapshot, or not duplicable (some kernel-owned handles)
    }
    const OwnedHandle owned(duplicate, InjectedHandleCloser{.close = api.closeHandle});
    switch (api.getFileType(owned.get()))
    {
    case FILE_TYPE_DISK:
        file.kind = OpenFileKind::File;
        file.path = finalPath(api, owned.get());
        break;
    case FILE_TYPE_PIPE:
        file.kind = OpenFileKind::Pipe; // named or anonymous: a name query is where a pipe can block
        break;
    case FILE_TYPE_CHAR:
        file.kind = OpenFileKind::Device; // a console, NUL, a COM port
        break;
    default:
        break;
    }
    return file;
}

} // namespace

/// What a read and its namer thread share. Either may outlive the other: a namer abandoned on a
/// handle that never answers keeps it (and the process handle it closes) alive.
struct WindowsProcessOpenFilesReader::NamingState
{
    NamingState(const ProcessOpenFilesFunctions& functions, HANDLE processHandle, std::vector<SystemHandleEntry> entries)
        : api(functions), process(processHandle), handles(std::move(entries))
    {}

    ~NamingState()
    {
        api.closeHandle(process);
    }

    NamingState(const NamingState&) = delete;
    NamingState& operator=(const NamingState&) = delete;
    NamingState(NamingState&&) = delete;
    NamingState& operator=(NamingState&&) = delete;

    const ProcessOpenFilesFunctions api;
    HANDLE process; // owned: closed when the last of the read and the namer lets go
    const std::vector<SystemHandleEntry> handles;

    std::mutex mutex;
    std::condition_variable progress;
    std::vector<OpenFile> files; // named so far, in `handles` order
    bool abandoned = false;      // the read stopped waiting: name no more
    bool finished = false;
};

namespace
{

void runNamer(const std::shared_ptr<WindowsProcessOpenFilesReader::NamingState>& state)
{
    static_cast<void>(setCurrentThreadName(OPEN_FILES_NAMER_THREAD_NAME));
    try
    {
        for (const SystemHandleEntry& entry : state->handles)
        {
            {
                const std::scoped_lock lock(state->mutex);
                if (state->abandoned)
                {
                    break;
                }
            }
            OpenFile file = nameHandle(state->api, state->process, entry);
            {
                const std::scoped_lock lock(state->mutex);
                state->files.push_back(std::move(file));
            }
            state->progress.notify_all();
        }
    }
    catch (const std::exception&)
    {
        // Out of memory: stop; the read lists the handles not named yet unnamed.
        const std::scoped_lock lock(state->mutex);
        state->abandoned = true;
    }
    {
        const std::scoped_lock lock(state->mutex);
        state->finished = true;
    }
    state->progress.notify_all();
}

} // namespace

std::vector<SystemHandleEntry> handlesOfProcess(std::span<const std::byte> snapshot, ULONG_PTR pid)
{
    std::vector<SystemHandleEntry> handles;
    constexpr std::size_t HEADER_BYTES = 2 * sizeof(ULONG_PTR); // NumberOfHandles, Reserved
    if (snapshot.size() < HEADER_BYTES)
    {
        return handles;
    }
    ULONG_PTR count = 0;
    std::memcpy(&count, snapshot.data(), sizeof(count));
    const std::size_t fits = (snapshot.size() - HEADER_BYTES) / sizeof(SystemHandleEntry);
    const std::size_t listed = std::min<std::size_t>(count, fits);
    for (std::size_t i = 0; i < listed; ++i)
    {
        SystemHandleEntry entry;
        std::memcpy(&entry, snapshot.data() + HEADER_BYTES + (i * sizeof(SystemHandleEntry)), sizeof(entry));
        if (entry.uniqueProcessId == pid)
        {
            handles.push_back(entry);
        }
    }
    return handles;
}

WindowsProcessOpenFilesReader::WindowsProcessOpenFilesReader() : WindowsProcessOpenFilesReader(ProcessOpenFilesFunctions{})
{}

WindowsProcessOpenFilesReader::WindowsProcessOpenFilesReader(const ProcessOpenFilesFunctions& api, std::chrono::milliseconds handleTimeout)
    : m_Api(api), m_HandleTimeout(handleTimeout)
{
    if (m_Api.ntQuerySystemInformation == nullptr)
    {
        if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll"); ntdll != nullptr)
        {
            m_Api.ntQuerySystemInformation =
                reinterpret_cast<ProcessOpenFilesFunctions::NtQuerySystemInformationFn>(GetProcAddress(ntdll, "NtQuerySystemInformation"));
        }
    }
}

WindowsProcessOpenFilesReader::~WindowsProcessOpenFilesReader() = default;

void WindowsProcessOpenFilesReader::releaseSnapshot() noexcept
{
    // Megabytes, needed only during a read: not kept for as long as the reader lives.
    std::vector<std::byte>().swap(m_Snapshot);
}

bool WindowsProcessOpenFilesReader::hasOpenFiles() const
{
    return m_Api.ntQuerySystemInformation != nullptr;
}

std::size_t WindowsProcessOpenFilesReader::stuckNamerCount()
{
    std::erase_if(m_StuckNamers,
                  [](const std::shared_ptr<NamingState>& state)
                  {
                      const std::scoped_lock lock(state->mutex);
                      return state->finished;
                  });
    return m_StuckNamers.size();
}

std::optional<std::string> WindowsProcessOpenFilesReader::querySnapshot()
{
    m_Snapshot.resize(std::max(INITIAL_SNAPSHOT_BYTES, m_SnapshotBytesHint));
    for (;;)
    {
        ULONG needed = 0;
        const LONG status = m_Api.ntQuerySystemInformation(
            SYSTEM_EXTENDED_HANDLE_INFORMATION, m_Snapshot.data(), static_cast<ULONG>(m_Snapshot.size()), &needed);
        if (status == STATUS_INFO_LENGTH_MISMATCH_CODE)
        {
            // The table grows between calls: ask for headroom beyond what it needed.
            const std::size_t next = std::max<std::size_t>(m_Snapshot.size() * 2, std::size_t{needed} + (std::size_t{1} << 20U));
            if (next > MAX_SNAPSHOT_BYTES)
            {
                releaseSnapshot();
                return "The system handle table is too large to read";
            }
            m_Snapshot.resize(next);
            m_SnapshotBytesHint = next;
            continue;
        }
        if (status < 0)
        {
            releaseSnapshot();
            return std::format("NtQuerySystemInformation failed (0x{:08X})", static_cast<std::uint32_t>(status));
        }
        return std::nullopt;
    }
}

std::optional<USHORT> WindowsProcessOpenFilesReader::fileTypeIndex()
{
    if (m_FileTypeIndex.has_value())
    {
        return m_FileTypeIndex;
    }
    // A handle of our own known to be a File object, found in a snapshot taken while it is open.
    auto* const nul = m_Api.createFile(L"NUL", 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (nul == INVALID_HANDLE_VALUE || nul == nullptr)
    {
        return std::nullopt;
    }
    const OwnedHandle probe(nul, InjectedHandleCloser{.close = m_Api.closeHandle});
    if (querySnapshot().has_value())
    {
        return std::nullopt;
    }
    for (const SystemHandleEntry& entry : handlesOfProcess(m_Snapshot, m_Api.getCurrentProcessId()))
    {
        if (entry.handleValue == reinterpret_cast<ULONG_PTR>(probe.get()))
        {
            m_FileTypeIndex = entry.objectTypeIndex;
            break;
        }
    }
    releaseSnapshot();
    return m_FileTypeIndex;
}

OpenFilesReadResult WindowsProcessOpenFilesReader::readOpenFiles(const ProcessTarget& target)
{
    if (!hasOpenFiles())
    {
        return resultOf(OpenFilesReadStatus::Unsupported);
    }
    if (target.pid <= 0)
    {
        return resultOf(OpenFilesReadStatus::ProcessExited);
    }
    if (target.startTimeTicks == 0)
    {
        return resultOf(OpenFilesReadStatus::IdentityUnknown); // Idle, System: refused rather than read by PID alone
    }

    // PROCESS_DUP_HANDLE: protected processes, and elevated ones from an unelevated TaskSmack, refuse it.
    OwnedHandle process(m_Api.openProcess(PROCESS_DUP_HANDLE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(target.pid)),
                        InjectedHandleCloser{.close = m_Api.closeHandle});
    if (!process)
    {
        const DWORD error = GetLastError();
        if (error == ERROR_ACCESS_DENIED)
        {
            return resultOf(OpenFilesReadStatus::PermissionDenied);
        }
        return error == ERROR_INVALID_PARAMETER ? resultOf(OpenFilesReadStatus::ProcessExited)
                                                : resultOf(OpenFilesReadStatus::Failed, errorText(error));
    }
    FILETIME creation{};
    FILETIME exitTime{};
    FILETIME kernelTime{};
    FILETIME userTime{};
    if (m_Api.getProcessTimes(process.get(), &creation, &exitTime, &kernelTime, &userTime) == 0)
    {
        return resultOf(OpenFilesReadStatus::Failed, errorText(GetLastError()));
    }
    const std::uint64_t actualTicks =
        (static_cast<std::uint64_t>(creation.dwHighDateTime) << 32U) | static_cast<std::uint64_t>(creation.dwLowDateTime);
    if (actualTicks != target.startTimeTicks)
    {
        return resultOf(OpenFilesReadStatus::ProcessExited); // the PID now names another process
    }

    const std::optional<USHORT> fileType = fileTypeIndex();
    if (!fileType.has_value())
    {
        return resultOf(OpenFilesReadStatus::Failed, "Could not identify file handles");
    }
    if (const std::optional<std::string> error = querySnapshot())
    {
        return resultOf(OpenFilesReadStatus::Failed, *error);
    }

    OpenFilesReadResult result = resultOf(OpenFilesReadStatus::Ok);
    const auto pid = static_cast<ULONG_PTR>(target.pid);
    std::vector<SystemHandleEntry> fileHandles;
    std::vector<OpenFile> skipped; // known to hang: listed, never touched
    for (const SystemHandleEntry& entry : handlesOfProcess(m_Snapshot, pid))
    {
        if (entry.objectTypeIndex != *fileType)
        {
            continue;
        }
        if (fileHandles.size() + skipped.size() >= MAX_OPEN_FILES)
        {
            result.truncated = true;
            break;
        }
        if (m_HungHandles.contains({pid, entry.handleValue}))
        {
            skipped.push_back(unqueried(entry));
            result.namesIncomplete = true;
        }
        else
        {
            fileHandles.push_back(entry);
        }
    }
    releaseSnapshot();

    // Too many namers already stuck on handles that never answered: list the rest unnamed.
    if (stuckNamerCount() >= MAX_STUCK_NAMERS)
    {
        for (const SystemHandleEntry& entry : fileHandles)
        {
            result.files.push_back(unqueried(entry));
        }
        result.namesIncomplete = result.namesIncomplete || !fileHandles.empty();
        fileHandles.clear();
    }

    if (!fileHandles.empty())
    {
        // The namer owns the process handle from here: it may outlive this read.
        const auto state = std::make_shared<NamingState>(m_Api, process.release(), std::move(fileHandles));
        try
        {
            std::thread([state] { runNamer(state); }).detach();
        }
        catch (const std::system_error& e)
        {
            return resultOf(OpenFilesReadStatus::Failed, e.what());
        }
        const auto deadline = std::chrono::steady_clock::now() + READ_BUDGET;
        std::unique_lock lock(state->mutex);
        std::size_t seen = 0;
        while (!state->finished)
        {
            const bool advanced =
                state->progress.wait_for(lock, m_HandleTimeout, [&] { return state->finished || state->files.size() != seen; });
            if (!advanced || std::chrono::steady_clock::now() > deadline)
            {
                // The handle being named did not answer: abandon the namer and list the rest unnamed.
                state->abandoned = true;
                result.namesIncomplete = true;
                if (!advanced && seen < state->handles.size())
                {
                    m_HungHandles.emplace(pid, state->handles[seen].handleValue);
                }
                m_StuckNamers.push_back(state);
                break;
            }
            seen = state->files.size();
        }
        result.files.insert(result.files.end(), state->files.begin(), state->files.end());
        for (std::size_t i = state->files.size(); i < state->handles.size(); ++i)
        {
            result.files.push_back(unqueried(state->handles[i]));
        }
    }
    result.files.insert(result.files.end(), skipped.begin(), skipped.end());
    std::ranges::sort(result.files, {}, &OpenFile::descriptor);
    return result;
}

} // namespace Platform::Windows
