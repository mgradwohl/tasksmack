#include "WindowsProcessDump.h"

#include "Platform/IProcessActions.h"
#include "WinString.h"
#include "WindowsHandles.h"
#include "WindowsProcAddress.h"
#include "WindowsProcessDumpMath.h"
#include "WindowsProcessMachine.h"

#include <spdlog/spdlog.h>

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h> // NOLINT(misc-include-cleaner) - umbrella header; symbols reside in sub-headers
#include <dbghelp.h> // NOLINT(misc-include-cleaner) - MINIDUMP_* types; the function itself is looked up at run time
#include <processsnapshot.h> // NOLINT(misc-include-cleaner) - PssFreeSnapshot, PSS_* flags
#include <psapi.h> // NOLINT(misc-include-cleaner) - GetProcessMemoryInfo, PROCESS_MEMORY_COUNTERS_EX
// clang-format on

#include <cstdint>
#include <expected>
#include <filesystem>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>

namespace Platform::Windows
{

// NOLINTBEGIN(misc-include-cleaner) - Windows APIs; Win32 types come from windows.h sub-headers
namespace
{

using MiniDumpWriteDumpFn = decltype(&MiniDumpWriteDump);

/// MiniDumpWriteDump from dbghelp.dll, loaded from System32 the first time a dump is written and kept
/// for the process's life: only System32's copy, so a dbghelp.dll beside a portable TaskSmack is never
/// loaded. Null when either is missing.
[[nodiscard]] MiniDumpWriteDumpFn miniDumpWriteDump()
{
    static const UniqueModule dbghelp(LoadLibraryExW(L"dbghelp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32));
    static const auto fn = getProcAddress<MiniDumpWriteDumpFn>(dbghelp.get(), "MiniDumpWriteDump");
    return fn;
}

/// dbghelp's functions are single-threaded: two dumps at once (Process Details' and the Processes
/// table's) take turns.
std::mutex& dbghelpMutex()
{
    static std::mutex mutex;
    return mutex;
}

/// Task Manager's full dump: all of the process's memory, its handles, the modules it has unloaded,
/// and its memory regions' and threads' details; memory that can't be read is skipped, not fatal.
// NOLINTBEGIN(bugprone-signed-bitwise) - MINIDUMP_TYPE is a C enum of flags, ORed as dbghelp documents
constexpr auto DUMP_TYPE =
    static_cast<MINIDUMP_TYPE>(MiniDumpWithFullMemory | MiniDumpWithHandleData | MiniDumpWithUnloadedModules | MiniDumpWithFullMemoryInfo |
                               MiniDumpWithThreadInfo | MiniDumpIgnoreInaccessibleMemory);
// NOLINTEND(bugprone-signed-bitwise)

/// What the snapshot captures, and how (#2001): Microsoft's documented recipe for a MiniDumpWriteDump
/// of a snapshot, which Task Manager follows, chosen for what DUMP_TYPE writes.
///  - VA_CLONE: a copy-on-write clone of the address space. The dump reads the clone, so the process
///    pauses only while it is made; the full-memory stream comes from it.
///  - HANDLES, with their NAME, BASIC, TYPE_SPECIFIC information and TRACE: the handle data stream
///    (MiniDumpWithHandleData) -- names and types, as a live dump has them.
///  - THREADS, THREAD_CONTEXT and THREAD_CONTEXT_EXTENDED: the thread list and every thread's registers,
///    the extended (AVX/XSAVE) state included, which the stacks are walked from
///    (MiniDumpWithThreadInfo).
///  - CREATE_BREAKAWAY with BREAKAWAY_OPTIONAL: the clone leaves the process's job where the job lets
///    it, so the job's limits (a process count, kill-on-close) don't apply to it, and the snapshot
///    still succeeds where the job doesn't.
///  - CREATE_USE_VM_ALLOCATIONS: the snapshot's own buffers come from VirtualAlloc, not TaskSmack's
///    heap.
///  - CREATE_RELEASE_SECTION: the clone holds no reference to the process's image file, so the clone
///    never keeps an executable in use. The module list is read from the clone's memory, and the
///    file's name from the process itself, so the dump loses nothing by it.
/// Left out: VA_SPACE and VA_SPACE_SECTION_INFORMATION (only PssWalkSnapshot() reads those; dbghelp
/// reads the clone's memory itself), IPT_TRACE, FORCE_BREAKAWAY (needs the TCB privilege) and
/// MEASURE_PERFORMANCE.
// processsnapshot.h gives PSS_CAPTURE_FLAGS its own operator| (DEFINE_ENUM_FLAG_OPERATORS), so this
// stays a PSS_CAPTURE_FLAGS throughout.
constexpr PSS_CAPTURE_FLAGS SNAPSHOT_CAPTURE_FLAGS =
    PSS_CAPTURE_VA_CLONE | PSS_CAPTURE_HANDLES | PSS_CAPTURE_HANDLE_NAME_INFORMATION | PSS_CAPTURE_HANDLE_BASIC_INFORMATION |
    PSS_CAPTURE_HANDLE_TYPE_SPECIFIC_INFORMATION | PSS_CAPTURE_HANDLE_TRACE | PSS_CAPTURE_THREADS | PSS_CAPTURE_THREAD_CONTEXT |
    PSS_CAPTURE_THREAD_CONTEXT_EXTENDED | PSS_CREATE_BREAKAWAY | PSS_CREATE_BREAKAWAY_OPTIONAL | PSS_CREATE_USE_VM_ALLOCATIONS |
    PSS_CREATE_RELEASE_SECTION;

/// Every register the threads have, for THREAD_CONTEXT.
constexpr DWORD SNAPSHOT_CONTEXT_FLAGS = CONTEXT_ALL;

/// A snapshot from PssCaptureSnapshot(), freed (its clone ended) on scope exit.
class Snapshot
{
  public:
    Snapshot() = default;
    explicit Snapshot(HPSS handle) noexcept : m_Handle(handle)
    {}
    ~Snapshot()
    {
        if (m_Handle != nullptr)
        {
            // The snapshot was captured into this process, so it is freed through this process's handle.
            static_cast<void>(PssFreeSnapshot(GetCurrentProcess(), m_Handle));
        }
    }
    Snapshot(const Snapshot&) = delete;
    Snapshot& operator=(const Snapshot&) = delete;
    Snapshot(Snapshot&&) = delete;
    Snapshot& operator=(Snapshot&&) = delete;

    [[nodiscard]] HPSS get() const noexcept
    {
        return m_Handle;
    }

  private:
    HPSS m_Handle = nullptr;
};

/// What dumpCallback() is handed: the stop token, and whether dbghelp is reading a snapshot.
struct CallbackState
{
    const std::stop_token* stopToken = nullptr;
    bool snapshot = false;
};

/// Tells dbghelp the handle it was given is a snapshot's, and asks it to stop once the stop token is
/// requested.
BOOL CALLBACK dumpCallback(PVOID param, PMINIDUMP_CALLBACK_INPUT input, PMINIDUMP_CALLBACK_OUTPUT output)
{
    if (param == nullptr || input == nullptr || output == nullptr)
    {
        return TRUE;
    }
    const auto* state = static_cast<const CallbackState*>(param);
    // NOLINTBEGIN(cppcoreguidelines-pro-type-union-access) - CallbackType selects the member
    if (input->CallbackType == IsProcessSnapshotCallback)
    {
        // S_FALSE: the handle is a PSS snapshot, which dbghelp then reads through the Pss* functions.
        output->Status = state->snapshot ? S_FALSE : S_OK;
    }
    else if (input->CallbackType == CancelCallback && state->stopToken != nullptr)
    {
        output->Cancel = state->stopToken->stop_requested() ? TRUE : FALSE;
        output->CheckCancel = TRUE; // keep asking, until it is done
    }
    // NOLINTEND(cppcoreguidelines-pro-type-union-access)
    return TRUE;
}

/// The process's image file name (its full path), UTF-8; empty when it can't be read.
[[nodiscard]] std::string imageName(HANDLE process)
{
    constexpr DWORD MAX_PATH_CHARS = 32768; // the longest path Windows allows, with its terminator
    std::wstring buffer(MAX_PATH_CHARS, L'\0');
    DWORD size = MAX_PATH_CHARS;
    if (QueryFullProcessImageNameW(process, 0, buffer.data(), &size) == 0)
    {
        return {};
    }
    return WinString::wideToUtf8(std::wstring_view(buffer.data(), size));
}

/// The machine this TaskSmack is built for: x64 or ARM64 only (the build is 64-bit only, #2152).
#if defined(_M_ARM64EC)
constexpr std::uint16_t SELF_MACHINE = DumpMath::MACHINE_ARM64EC;
#elif defined(_M_ARM64) || defined(__aarch64__)
constexpr std::uint16_t SELF_MACHINE = DumpMath::MACHINE_ARM64;
#elif defined(_M_X64) || defined(__x86_64__)
constexpr std::uint16_t SELF_MACHINE = DumpMath::MACHINE_AMD64;
#else
#error "TaskSmack is built for x64 or ARM64 only"
#endif

[[nodiscard]] DumpMath::DumpTime localTimeNow()
{
    SYSTEMTIME now{};
    GetLocalTime(&now);
    return {.year = now.wYear, .month = now.wMonth, .day = now.wDay, .hour = now.wHour, .minute = now.wMinute, .second = now.wSecond};
}

/// A new file in @p directory, named for the process: the next free name when one exists already.
struct NewFile
{
    UniqueHandle handle;
    std::filesystem::path path;
    DWORD error = 0; ///< Why there is no handle
};

[[nodiscard]] NewFile createDumpFile(const std::filesystem::path& directory, std::string_view image, std::int32_t pid)
{
    const DumpMath::DumpTime time = localTimeNow();
    NewFile file;
    for (int attempt = 1; attempt <= DumpMath::MAX_NAME_ATTEMPTS; ++attempt)
    {
        file.path = directory / WinString::utf8ToWide(DumpMath::dumpFileName(image, pid, time, attempt));
        file.handle.reset(CreateFileW(file.path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (file.handle)
        {
            file.error = 0;
            return file;
        }
        file.error = GetLastError();
        if (file.error != ERROR_FILE_EXISTS)
        {
            break;
        }
    }
    return file;
}

} // namespace

bool isProtectedProcess(DWORD pid)
{
    // A protected process still lets anyone open it for limited queries, which is all this needs.
    const UniqueHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (!process)
    {
        return false;
    }
    PROCESS_PROTECTION_LEVEL_INFORMATION protection{};
    if (GetProcessInformation(process.get(), ProcessProtectionLevelInfo, &protection, sizeof(protection)) == 0)
    {
        return false;
    }
    return protection.ProtectionLevel != PROTECTION_LEVEL_NONE;
}

ProcessDumpResult writeProcessDump(
    HANDLE process, std::int32_t pid, const std::filesystem::path& directory, const std::stop_token& stopToken, const DumpSource& source)
{
    if (directory.empty())
    {
        return std::unexpected(std::string("No folder was given to write the dump file in"));
    }
    // dbghelp writes a usable dump only of a process of its own architecture, or of one of its WOW64
    // guests: an x64 TaskSmack emulated on Windows on ARM can't dump a native ARM64 process (#2152).
    const std::uint16_t targetMachine = (source.processMachine != nullptr) ? source.processMachine(process) : processMachine(process);
    const DumpMath::DumpArchitecture architecture = DumpMath::dumpArchitecture(SELF_MACHINE, targetMachine);
    if (architecture == DumpMath::DumpArchitecture::Unsupported)
    {
        std::string message = DumpMath::unsupportedArchitectureMessage(pid, SELF_MACHINE, targetMachine);
        spdlog::warn("Dump of PID {}: {}", pid, message);
        return std::unexpected(std::move(message));
    }
    const std::string note =
        (architecture == DumpMath::DumpArchitecture::Wow64Target) ? DumpMath::wow64DumpNote(targetMachine) : std::string{};

    const MiniDumpWriteDumpFn writeDump = miniDumpWriteDump();
    if (writeDump == nullptr)
    {
        return std::unexpected(std::string("Can't create a dump file: dbghelp.dll could not be loaded from System32"));
    }

    const std::string directoryText = WinString::wideToUtf8(directory.wstring());
    NewFile file = createDumpFile(directory, imageName(process), pid);
    if (!file.handle)
    {
        std::string message = DumpMath::createFileFailureMessage(file.error, directoryText);
        spdlog::warn("Dump of PID {}: {}", pid, message);
        return std::unexpected(std::move(message));
    }

    // The snapshot is taken only once the file exists, so a folder that can't be written never pauses
    // the process at all. A failure falls back to the live dump, and the result says why.
    std::string liveReason = source.liveReason;
    HPSS captured = nullptr;
    if (source.captureSnapshot != nullptr && !stopToken.stop_requested())
    {
        const DWORD captureError = source.captureSnapshot(process, SNAPSHOT_CAPTURE_FLAGS, SNAPSHOT_CONTEXT_FLAGS, &captured);
        if (captureError != ERROR_SUCCESS)
        {
            captured = nullptr;
            liveReason = DumpMath::snapshotFailureReason(captureError, false);
            spdlog::warn("Dump of PID {}: no snapshot ({}); writing it from the running process", pid, liveReason);
        }
    }
    const Snapshot snapshot(captured);
    const bool fromSnapshot = snapshot.get() != nullptr;
    if (fromSnapshot)
    {
        liveReason.clear();
    }
    else if (liveReason.empty())
    {
        liveReason = DumpMath::snapshotFailureReason(DumpMath::ERROR_CODE_NOT_SUPPORTED, false);
    }
    spdlog::info("Writing a full dump of PID {} from {} to {}",
                 pid,
                 fromSnapshot ? "a snapshot" : "the running process",
                 WinString::wideToUtf8(file.path.wstring()));

    CallbackState state{.stopToken = &stopToken, .snapshot = fromSnapshot};
    MINIDUMP_CALLBACK_INFORMATION callback{};
    callback.CallbackRoutine = &dumpCallback;
    callback.CallbackParam = &state;

    // A snapshot's handle stands in for the process's: dbghelp asks dumpCallback() which it is.
    HANDLE readFrom =
        fromSnapshot ? reinterpret_cast<HANDLE>(snapshot.get()) : process; // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
    BOOL written = FALSE;
    DWORD error = 0;
    {
        const std::scoped_lock lock(dbghelpMutex());
        written = writeDump(readFrom, static_cast<DWORD>(pid), file.handle.get(), DUMP_TYPE, nullptr, nullptr, &callback);
        error = (written == FALSE) ? GetLastError() : 0;
    }

    if (written == FALSE || stopToken.stop_requested())
    {
        file.handle.reset(); // closed before it can be deleted
        DeleteFileW(file.path.c_str());
        std::string message = DumpMath::writeFailureMessage(error, pid, directoryText, stopToken.stop_requested());
        spdlog::warn("Dump of PID {}: {}", pid, message);
        return std::unexpected(std::move(message));
    }

    LARGE_INTEGER size{};
    const std::uint64_t sizeBytes = (GetFileSizeEx(file.handle.get(), &size) != 0) ? static_cast<std::uint64_t>(size.QuadPart) : 0;
    spdlog::info("Wrote a {}-byte dump of PID {}", sizeBytes, pid);
    return ProcessDump{.file = std::move(file.path),
                       .sizeBytes = sizeBytes,
                       .note = note,
                       .fromSnapshot = fromSnapshot,
                       .liveReason = std::move(liveReason)};
}

std::optional<std::uint64_t> processPrivateBytes(HANDLE process)
{
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    // PROCESS_MEMORY_COUNTERS_EX is the documented extension GetProcessMemoryInfo() fills by its cb.
    if (GetProcessMemoryInfo(process, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters)) ==
        0) // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
    {
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(counters.PrivateUsage);
}

std::optional<std::uint64_t> freeBytesInFolder(const std::filesystem::path& directory)
{
    if (directory.empty())
    {
        return std::nullopt;
    }
    ULARGE_INTEGER freeToCaller{};
    if (GetDiskFreeSpaceExW(directory.c_str(), &freeToCaller, nullptr, nullptr) == 0)
    {
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(freeToCaller.QuadPart);
}
// NOLINTEND(misc-include-cleaner)

} // namespace Platform::Windows
