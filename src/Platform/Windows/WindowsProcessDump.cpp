#include "WindowsProcessDump.h"

#include "Platform/IProcessActions.h"
#include "WinString.h"
#include "WindowsHandles.h"
#include "WindowsProcAddress.h"
#include "WindowsProcessDumpMath.h"

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
// clang-format on

#include <cstdint>
#include <expected>
#include <filesystem>
#include <mutex>
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

/// Asks dbghelp to stop once the stop token passed as @p param is requested.
BOOL CALLBACK dumpCallback(PVOID param, PMINIDUMP_CALLBACK_INPUT input, PMINIDUMP_CALLBACK_OUTPUT output)
{
    if (param == nullptr || input == nullptr || output == nullptr)
    {
        return TRUE;
    }
    if (input->CallbackType == CancelCallback)
    {
        const auto* stopToken = static_cast<const std::stop_token*>(param);
        // NOLINTBEGIN(cppcoreguidelines-pro-type-union-access) - CallbackType selects the member
        output->Cancel = stopToken->stop_requested() ? TRUE : FALSE;
        output->CheckCancel = TRUE; // keep asking, until it is done
        // NOLINTEND(cppcoreguidelines-pro-type-union-access)
    }
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

[[nodiscard]] bool isWow64(HANDLE process)
{
    BOOL wow64 = FALSE;
    return IsWow64Process(process, &wow64) != 0 && wow64 != FALSE;
}

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

ProcessDumpResult
writeProcessDump(HANDLE process, std::int32_t pid, const std::filesystem::path& directory, const std::stop_token& stopToken)
{
    if (directory.empty())
    {
        return std::unexpected(std::string("No folder was given to write the dump file in"));
    }
    const DumpMath::DumpBitness bitness = DumpMath::dumpBitness(isWow64(GetCurrentProcess()), isWow64(process));
    if (bitness == DumpMath::DumpBitness::NativeTargetOfWow64)
    {
        return std::unexpected(DumpMath::nativeTargetOfWow64Message(pid));
    }
    const std::string note = (bitness == DumpMath::DumpBitness::Wow64Target) ? std::string(DumpMath::WOW64_DUMP_NOTE) : std::string{};

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
    spdlog::info("Writing a full dump of PID {} to {}", pid, WinString::wideToUtf8(file.path.wstring()));

    MINIDUMP_CALLBACK_INFORMATION callback{};
    callback.CallbackRoutine = &dumpCallback;
    // dbghelp hands the pointer back to dumpCallback() unchanged, and only reads through it.
    callback.CallbackParam = const_cast<std::stop_token*>(&stopToken); // NOLINT(cppcoreguidelines-pro-type-const-cast)

    BOOL written = FALSE;
    DWORD error = 0;
    {
        const std::scoped_lock lock(dbghelpMutex());
        written = writeDump(process, static_cast<DWORD>(pid), file.handle.get(), DUMP_TYPE, nullptr, nullptr, &callback);
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
    return ProcessDump{.file = std::move(file.path), .sizeBytes = sizeBytes, .note = note};
}
// NOLINTEND(misc-include-cleaner)

} // namespace Platform::Windows
