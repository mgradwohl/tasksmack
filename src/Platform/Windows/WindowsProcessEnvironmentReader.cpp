#include "WindowsProcessEnvironmentReader.h"

#include "Platform/IProcessActions.h"
#include "Platform/IProcessEnvironment.h"
#include "Platform/ProcessEnvironment.h"
#include "WinString.h"
#include "WindowsHandles.h"
#include "WindowsProcessActionsMath.h"
#include "WindowsProcessIdentity.h"

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h> // NOLINT(misc-include-cleaner) - umbrella header; symbols reside in sub-headers
#include <winternl.h> // PEB, RTL_USER_PROCESS_PARAMETERS, PROCESS_BASIC_INFORMATION
// clang-format on

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace Platform::Windows
{

namespace
{

// NOLINTBEGIN(misc-include-cleaner) - Windows APIs; Win32 types come from windows.h sub-headers

/// ProcessBasicInformation, NtQueryInformationProcess's class 0: PROCESS_BASIC_INFORMATION.
constexpr ULONG PROCESS_BASIC_INFORMATION_CLASS = 0;

/// Where the fields read sit in the target, for the reader's own pointer size. winternl.h names
/// PEB::ProcessParameters and RTL_USER_PROCESS_PARAMETERS::CommandLine; Environment follows
/// CommandLine, and EnvironmentSize (Windows 8.1 and later; 0 before) follows StartingX ..
/// ShowWindowFlags (nine ULONGs), WindowTitle, DesktopInfo, ShellInfo and RuntimeData (four
/// UNICODE_STRINGs) and CurrentDirectores (32 RTL_DRIVE_LETTER_CURDIR: USHORT Flags, USHORT Length,
/// ULONG TimeStamp, STRING DosPath).
constexpr bool READER_IS_64_BIT = sizeof(void*) == 8;
constexpr std::size_t PEB_PROCESS_PARAMETERS_OFFSET = offsetof(PEB, ProcessParameters);
constexpr std::size_t PARAMETERS_ENVIRONMENT_OFFSET = offsetof(RTL_USER_PROCESS_PARAMETERS, CommandLine) + sizeof(UNICODE_STRING);
constexpr std::size_t PARAMETERS_ENVIRONMENT_SIZE_OFFSET = READER_IS_64_BIT ? 0x3F0 : 0x290;
static_assert(PEB_PROCESS_PARAMETERS_OFFSET == (READER_IS_64_BIT ? 0x20 : 0x10));
static_assert(PARAMETERS_ENVIRONMENT_OFFSET == (READER_IS_64_BIT ? 0x80 : 0x48));

/// The block is read a page at a time: a page is readable whole or not at all, so a read never fails
/// part-way for want of the next page (ERROR_PARTIAL_COPY), and the last page of a block whose
/// EnvironmentSize overstates it is still read.
constexpr std::size_t READ_PAGE_BYTES = 4096;

[[nodiscard]] EnvironmentReadResult failure(EnvironmentReadStatus status, std::string detail = {})
{
    return {.status = status, .variables = {}, .truncated = false, .detail = std::move(detail)};
}

/// The system's message for a Win32 error code ("Only part of a ReadProcessMemory ... was completed").
[[nodiscard]] std::string errorText(DWORD error)
{
    return std::system_category().message(static_cast<int>(error));
}

/// @p address in the target, as ReadProcessMemory takes it: never dereferenced here.
[[nodiscard]] LPCVOID remote(std::uintptr_t address) noexcept
{
    return reinterpret_cast<LPCVOID>(address); // NOLINT(performance-no-int-to-ptr) - another process's address
}

/// One pointer-sized field of the target at @p address (a pointer, or the ULONG_PTR EnvironmentSize),
/// or the Win32 error of the read.
[[nodiscard]] std::expected<std::uintptr_t, DWORD>
readPointer(const ProcessEnvironmentFunctions& api, HANDLE process, std::uintptr_t address)
{
    std::uintptr_t value = 0;
    SIZE_T read = 0;
    const BOOL ok = api.readProcessMemory(process, remote(address), &value, sizeof(value), &read);
    if (ok == 0 || read != sizeof(value))
    {
        return std::unexpected((ok == 0) ? GetLastError() : static_cast<DWORD>(ERROR_PARTIAL_COPY));
    }
    return value;
}

/// Whether character @p index of the UTF-16 @p bytes is a NUL.
[[nodiscard]] bool isNulChar(std::span<const std::byte> bytes, std::size_t index) noexcept
{
    const std::size_t at = index * sizeof(wchar_t);
    return bytes[at] == std::byte{0} && bytes[at + 1] == std::byte{0};
}

/// The character index of the block's terminator -- a NUL character followed by another -- at or after
/// character @p fromChar of @p bytes, or nullopt if there is none there.
[[nodiscard]] std::optional<std::size_t> terminatorAt(std::span<const std::byte> bytes, std::size_t fromChar) noexcept
{
    const std::size_t chars = bytes.size() / sizeof(wchar_t);
    for (std::size_t i = fromChar; i + 1 < chars; ++i)
    {
        if (isNulChar(bytes, i) && isNulChar(bytes, i + 1))
        {
            return i;
        }
    }
    return std::nullopt;
}

/// What readBlock() got.
struct BlockRead
{
    std::vector<std::byte> bytes; ///< The block up to its terminator (left out), the limit, or the first unreadable page
    bool reachedLimit = false;    ///< The limit came before any terminator
    DWORD error = 0;              ///< A page could not be read before any terminator: the Win32 error
};

/// Reads the block at @p address, page by page, until its terminator, @p limit bytes, or a page that
/// cannot be read. The terminator is looked for across page boundaries, so a NUL ending one page pairs
/// with a NUL starting the next.
[[nodiscard]] BlockRead readBlock(const ProcessEnvironmentFunctions& api, HANDLE process, std::uintptr_t address, std::size_t limit)
{
    BlockRead block;
    std::size_t offset = 0;
    while (offset < limit)
    {
        const std::size_t toPageEnd = READ_PAGE_BYTES - ((address + offset) % READ_PAGE_BYTES);
        const std::size_t chunk = std::min(toPageEnd, limit - offset);
        const std::size_t have = block.bytes.size();
        block.bytes.resize(have + chunk);
        SIZE_T read = 0;
        const BOOL ok = api.readProcessMemory(process, remote(address + offset), block.bytes.data() + have, chunk, &read);
        if (ok == 0 || read == 0)
        {
            block.error = (ok == 0) ? GetLastError() : static_cast<DWORD>(ERROR_PARTIAL_COPY);
            block.bytes.resize(have);
            return block;
        }
        block.bytes.resize(have + read);
        const std::size_t haveChars = have / sizeof(wchar_t);
        if (const auto end = terminatorAt(block.bytes, haveChars - std::min<std::size_t>(haveChars, 1)); end.has_value())
        {
            block.bytes.resize(*end * sizeof(wchar_t));
            return block;
        }
        if (read < chunk)
        {
            block.error = ERROR_PARTIAL_COPY; // less than a page within a page: nothing more can be relied on
            return block;
        }
        offset += read;
    }
    block.reachedLimit = true;
    return block;
}

/// How a failed read of the target's memory reports: a process that exited since the identity check as
/// ProcessExited; a refused read as PermissionDenied; anything else as Failed, saying @p what could not
/// be read and the system's message (never what was read). The detail follows the section's "Could
/// not be read: ".
[[nodiscard]] EnvironmentReadResult readFailure(const ProcessEnvironmentFunctions& api, HANDLE process, DWORD error, std::string_view what)
{
    DWORD exitCode = STILL_ACTIVE;
    if (api.getExitCodeProcess(process, &exitCode) != 0 && exitCode != STILL_ACTIVE)
    {
        return failure(EnvironmentReadStatus::ProcessExited);
    }
    if (error == ERROR_ACCESS_DENIED)
    {
        return failure(EnvironmentReadStatus::PermissionDenied);
    }
    return failure(EnvironmentReadStatus::Failed, std::format("ReadProcessMemory failed on {}: {}", what, errorText(error)));
}

/// Whether @p process runs with the reader's pointer size, so its PEB and process parameters have the
/// layouts read here: both are WOW64 processes, or neither is. IsWow64Process2 reports a 32-bit process
/// on 64-bit Windows (x86 or ARM32, under WOW64) by its machine, and IMAGE_FILE_MACHINE_UNKNOWN for a
/// native one (an x64 process emulated on ARM64 included: its PEB has the 64-bit layout).
[[nodiscard]] std::expected<bool, std::string> sameBitness(const ProcessEnvironmentFunctions& api, HANDLE process)
{
    USHORT targetMachine = IMAGE_FILE_MACHINE_UNKNOWN;
    USHORT ownMachine = IMAGE_FILE_MACHINE_UNKNOWN;
    USHORT native = IMAGE_FILE_MACHINE_UNKNOWN;
    if (api.isWow64Process2(process, &targetMachine, &native) == 0)
    {
        return std::unexpected("the process's architecture could not be read: " + errorText(GetLastError()));
    }
    if (api.isWow64Process2(GetCurrentProcess(), &ownMachine, &native) == 0)
    {
        return std::unexpected("TaskSmack's own architecture could not be read: " + errorText(GetLastError()));
    }
    return (targetMachine == IMAGE_FILE_MACHINE_UNKNOWN) == (ownMachine == IMAGE_FILE_MACHINE_UNKNOWN);
}

} // namespace

bool WindowsProcessEnvironmentReader::hasEnvironment() const
{
    return true;
}

EnvironmentReadResult WindowsProcessEnvironmentReader::readEnvironment(const ProcessTarget& target)
{
    // PROCESS_VM_READ: the block lives in the process's own memory. Protected processes, and elevated
    // ones from an unelevated TaskSmack, refuse it (PermissionDenied).
    auto opened = openProcessVerified({.openProcess = m_Api.openProcess,
                                       .getProcessTimes = m_Api.getProcessTimes,
                                       .closeHandle = m_Api.closeHandle,
                                       .getExitCodeProcess = m_Api.getExitCodeProcess},
                                      target,
                                      PROCESS_VM_READ);
    if (!opened)
    {
        auto refused = toReadFailure<EnvironmentReadStatus>(opened.error());
        return failure(refused.status, std::move(refused.detail));
    }
    const InjectableHandle process = std::move(*opened);

    // The layouts read are the reader's own; a process of the other pointer size keeps its block behind
    // a PEB of the other layout, which isn't read yet (see the header).
    const std::expected<bool, std::string> same = sameBitness(m_Api, process.get());
    if (!same.has_value())
    {
        return failure(EnvironmentReadStatus::Failed, same.error());
    }
    if (!*same)
    {
        return failure(EnvironmentReadStatus::Failed,
                       READER_IS_64_BIT ? "a 32-bit process (WOW64), whose environment TaskSmack does not read yet"
                                        : "a 64-bit process, whose environment this 32-bit TaskSmack cannot read");
    }

    // The PEB's address.
    if (m_Api.ntQueryInformationProcess == nullptr)
    {
        return failure(EnvironmentReadStatus::Failed, "NtQueryInformationProcess is not available");
    }
    PROCESS_BASIC_INFORMATION information{};
    ULONG written = 0;
    const LONG status =
        m_Api.ntQueryInformationProcess(process.get(), PROCESS_BASIC_INFORMATION_CLASS, &information, sizeof(information), &written);
    if (status < 0)
    {
        switch (static_cast<std::uint32_t>(status))
        {
        case NT_STATUS_ACCESS_DENIED:
            return failure(EnvironmentReadStatus::PermissionDenied);
        case NT_STATUS_PROCESS_IS_TERMINATING:
            return failure(EnvironmentReadStatus::ProcessExited);
        default:
            return failure(EnvironmentReadStatus::Failed,
                           std::format("NtQueryInformationProcess failed: NTSTATUS 0x{:08X}", static_cast<std::uint32_t>(status)));
        }
    }
    if (written < sizeof(information) || information.PebBaseAddress == nullptr)
    {
        // A minimal process (System, Registry, Memory Compression) has no PEB; one still starting may not yet.
        return failure(EnvironmentReadStatus::Failed, "the process has no PEB");
    }
    const auto peb = reinterpret_cast<std::uintptr_t>(information.PebBaseAddress);

    // PEB::ProcessParameters, then its Environment and EnvironmentSize.
    const std::expected<std::uintptr_t, DWORD> parameters = readPointer(m_Api, process.get(), peb + PEB_PROCESS_PARAMETERS_OFFSET);
    if (!parameters.has_value())
    {
        return readFailure(m_Api, process.get(), parameters.error(), "the PEB");
    }
    if (*parameters == 0)
    {
        return failure(EnvironmentReadStatus::Failed, "the process has no process parameters yet");
    }
    const std::expected<std::uintptr_t, DWORD> environment = readPointer(m_Api, process.get(), *parameters + PARAMETERS_ENVIRONMENT_OFFSET);
    if (!environment.has_value())
    {
        return readFailure(m_Api, process.get(), environment.error(), "the process parameters");
    }
    if (*environment == 0)
    {
        return {.status = EnvironmentReadStatus::Ok, .variables = {}, .truncated = false, .detail = {}}; // no block: nothing set
    }
    const std::expected<std::uintptr_t, DWORD> size = readPointer(m_Api, process.get(), *parameters + PARAMETERS_ENVIRONMENT_SIZE_OFFSET);
    if (!size.has_value())
    {
        return readFailure(m_Api, process.get(), size.error(), "the process parameters");
    }

    // The block: as far as EnvironmentSize says, under the cap -- its terminator ends it sooner, before
    // any slack the allocation has past it -- or, where EnvironmentSize is 0, as far as the terminator.
    const auto declared = static_cast<std::size_t>(*size);
    const bool sized = declared != 0 && declared <= ENVIRONMENT_BLOCK_READ_CAP_BYTES;
    const std::size_t limit = sized ? declared : ENVIRONMENT_BLOCK_READ_CAP_BYTES;
    const BlockRead block = readBlock(m_Api, process.get(), *environment, limit);
    if (block.bytes.empty() && block.error != 0)
    {
        return readFailure(m_Api, process.get(), block.error, "the environment block");
    }
    // Cut at the cap, or at a page that could not be read before the terminator: what follows is unknown.
    const bool truncated = (block.reachedLimit && !sized) || block.error != 0;

    // UTF-16 to UTF-8 with the NULs kept (the length is explicit), then the NUL-separated NAME=value
    // entries as /proc/[pid]/environ's are parsed, escaped for display. Never logged, at any level: the
    // values are routinely secrets (#179).
    const std::size_t chars = block.bytes.size() / sizeof(wchar_t);
    std::wstring text(chars, L'\0');
    if (chars > 0)
    {
        std::memcpy(text.data(), block.bytes.data(), chars * sizeof(wchar_t));
    }
    const std::string utf8 = WinString::wideToUtf8(std::wstring_view{text});
    EnvironmentReadResult result{
        .status = EnvironmentReadStatus::Ok, .variables = Environment::parseEnvironBlock(utf8), .truncated = truncated, .detail = {}};
    if (result.variables.size() > ENVIRONMENT_VARIABLE_CAP)
    {
        result.variables.resize(ENVIRONMENT_VARIABLE_CAP);
        result.truncated = true;
    }
    return result;
}

// NOLINTEND(misc-include-cleaner)

} // namespace Platform::Windows
