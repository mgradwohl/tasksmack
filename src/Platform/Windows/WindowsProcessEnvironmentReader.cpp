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
#include <limits>
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

/// ProcessWow64Information, NtQueryInformationProcess's class 26: a ULONG_PTR, the address of a WOW64
/// process's 32-bit PEB, or 0 for a process that doesn't run under WOW64 (#3364).
constexpr ULONG PROCESS_WOW64_INFORMATION_CLASS = 26;

/// The layout of a target's PEB and process parameters, as far as the reader follows them: the size of
/// their pointers (and of EnvironmentSize, a ULONG_PTR), the offsets of the three fields read, and the
/// highest address a pointer of that size can hold.
struct PebLayout
{
    std::size_t pointerBytes = 0;
    std::size_t parametersOffset = 0;      ///< PEB::ProcessParameters
    std::size_t environmentOffset = 0;     ///< RTL_USER_PROCESS_PARAMETERS::Environment
    std::size_t environmentSizeOffset = 0; ///< RTL_USER_PROCESS_PARAMETERS::EnvironmentSize
    std::uintptr_t highestAddress = 0;
};

/// The reader's own pointer size: the PEB PROCESS_BASIC_INFORMATION points at.
constexpr PebLayout NATIVE_LAYOUT{
    .pointerBytes = sizeof(void*),
    .parametersOffset = PEB_PROCESS_PARAMETERS_OFFSET,
    .environmentOffset = PARAMETERS_ENVIRONMENT_OFFSET,
    .environmentSizeOffset = PARAMETERS_ENVIRONMENT_SIZE_OFFSET,
    .highestAddress = std::numeric_limits<std::uintptr_t>::max(),
};

/// A WOW64 process's PEB32 and RTL_USER_PROCESS_PARAMETERS32 (#3364): the 32-bit build's offsets above,
/// with 32-bit pointers, so nothing they point at lies above 4 GiB.
constexpr PebLayout WOW64_LAYOUT{
    .pointerBytes = 4,
    .parametersOffset = 0x10,
    .environmentOffset = 0x48,
    .environmentSizeOffset = 0x290,
    .highestAddress = 0xFFFF'FFFFU,
};

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

/// One pointer-sized field of @p layout at @p address in the target (a pointer, or the ULONG_PTR
/// EnvironmentSize), zero-extended, or the Win32 error of the read. A read short of the field is
/// ERROR_PARTIAL_COPY: nothing of a partly read field is used.
[[nodiscard]] std::expected<std::uintptr_t, DWORD>
readField(const ProcessEnvironmentFunctions& api, HANDLE process, const PebLayout& layout, std::uintptr_t address)
{
    static_assert(WOW64_LAYOUT.pointerBytes <= sizeof(std::uintptr_t));
    std::uintptr_t value = 0; // little-endian: a 4-byte field fills the low bytes, the rest stay 0
    SIZE_T read = 0;
    const BOOL ok = api.readProcessMemory(process, remote(address), &value, layout.pointerBytes, &read);
    if (ok == 0 || read != layout.pointerBytes)
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

/// Whether IsWow64Process2's process machine @p machine is a 32-bit one: x86 or ARM32, which run under
/// WOW64 on the 64-bit Windows TaskSmack ships for. IMAGE_FILE_MACHINE_UNKNOWN is a native process,
/// the OS's own size (64-bit); AMD64 and ARM64 are 64-bit too (an x64 process emulated on ARM64 Windows
/// is reported as AMD64 by some Windows versions, and its PEB has the 64-bit layout).
[[nodiscard]] constexpr bool is32BitMachine(USHORT machine) noexcept
{
    return machine == IMAGE_FILE_MACHINE_I386 || machine == IMAGE_FILE_MACHINE_ARMNT;
}

/// Which PEB of @p process holds its live environment.
enum class TargetPeb : std::uint8_t
{
    Native,      ///< The reader's pointer size: the PEB ProcessBasicInformation gives
    Wow64,       ///< A 32-bit process seen from the 64-bit reader: its PEB32 (ProcessWow64Information)
    Unreachable, ///< A 64-bit process seen from a 32-bit reader, whose memory it cannot address
};

/// Which PEB of @p process to read: decided by the pointer size each machine implies
/// (is32BitMachine()), not by whether either runs under WOW64, which an x64 process on ARM64 Windows
/// does with a 64-bit PEB.
[[nodiscard]] std::expected<TargetPeb, std::string> targetPeb(const ProcessEnvironmentFunctions& api, HANDLE process)
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
    const bool target32 = is32BitMachine(targetMachine);
    const bool own32 = !READER_IS_64_BIT || is32BitMachine(ownMachine);
    if (target32 == own32)
    {
        return TargetPeb::Native;
    }
    return target32 ? TargetPeb::Wow64 : TargetPeb::Unreachable;
}

/// How a failed NtQueryInformationProcess reports.
[[nodiscard]] EnvironmentReadResult queryFailure(LONG status)
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

/// The address of @p process's PEB of the kind @p which names (Native or Wow64), or the failure to report.
[[nodiscard]] std::expected<std::uintptr_t, EnvironmentReadResult>
pebAddress(const ProcessEnvironmentFunctions& api, HANDLE process, TargetPeb which)
{
    if (api.ntQueryInformationProcess == nullptr)
    {
        return std::unexpected(failure(EnvironmentReadStatus::Failed, "NtQueryInformationProcess is not available"));
    }
    ULONG written = 0;
    if (which == TargetPeb::Wow64)
    {
        ULONG_PTR peb32 = 0;
        const LONG status = api.ntQueryInformationProcess(process, PROCESS_WOW64_INFORMATION_CLASS, &peb32, sizeof(peb32), &written);
        if (status < 0)
        {
            return std::unexpected(queryFailure(status));
        }
        if (written < sizeof(peb32) || peb32 == 0 || peb32 > WOW64_LAYOUT.highestAddress)
        {
            // IsWow64Process2 said 32-bit, yet there is no 32-bit PEB (or none a 32-bit pointer reaches).
            return std::unexpected(failure(EnvironmentReadStatus::Failed, "the 32-bit process has no 32-bit PEB"));
        }
        return static_cast<std::uintptr_t>(peb32);
    }
    PROCESS_BASIC_INFORMATION information{};
    const LONG status =
        api.ntQueryInformationProcess(process, PROCESS_BASIC_INFORMATION_CLASS, &information, sizeof(information), &written);
    if (status < 0)
    {
        return std::unexpected(queryFailure(status));
    }
    if (written < sizeof(information) || information.PebBaseAddress == nullptr)
    {
        // A minimal process (System, Registry, Memory Compression) has no PEB; one still starting may not yet.
        return std::unexpected(failure(EnvironmentReadStatus::Failed, "the process has no PEB"));
    }
    return reinterpret_cast<std::uintptr_t>(information.PebBaseAddress);
}

/// Whether @p address is a multiple of @p alignment (a power of two).
[[nodiscard]] constexpr bool isAligned(std::uintptr_t address, std::size_t alignment) noexcept
{
    return (address & (alignment - 1)) == 0;
}

/// Whether the @p bytes bytes from @p address all lie at or below @p highest, without overflowing.
[[nodiscard]] constexpr bool fits(std::uintptr_t address, std::size_t bytes, std::uintptr_t highest) noexcept
{
    return address <= highest && bytes - 1 <= highest - address;
}

/// The environment behind the PEB at @p peb in @p process, whose PEB and process parameters have
/// @p layout. Every address is checked before it is followed: a corrupt or hostile pointer that is
/// misaligned, or whose field would run past what a pointer of the layout's size reaches, is Failed.
[[nodiscard]] EnvironmentReadResult
readEnvironmentAt(const ProcessEnvironmentFunctions& api, HANDLE process, std::uintptr_t peb, const PebLayout& layout)
{
    if (!isAligned(peb, layout.pointerBytes) || !fits(peb, layout.parametersOffset + layout.pointerBytes, layout.highestAddress))
    {
        return failure(EnvironmentReadStatus::Failed, "the PEB's address is not valid");
    }

    // PEB::ProcessParameters, then its Environment and EnvironmentSize.
    const std::expected<std::uintptr_t, DWORD> parameters = readField(api, process, layout, peb + layout.parametersOffset);
    if (!parameters.has_value())
    {
        return readFailure(api, process, parameters.error(), "the PEB");
    }
    if (*parameters == 0)
    {
        return failure(EnvironmentReadStatus::Failed, "the process has no process parameters yet");
    }
    if (!isAligned(*parameters, layout.pointerBytes) ||
        !fits(*parameters, layout.environmentSizeOffset + layout.pointerBytes, layout.highestAddress))
    {
        return failure(EnvironmentReadStatus::Failed, "the PEB points at process parameters at an address that is not valid");
    }
    const std::expected<std::uintptr_t, DWORD> environment = readField(api, process, layout, *parameters + layout.environmentOffset);
    if (!environment.has_value())
    {
        return readFailure(api, process, environment.error(), "the process parameters");
    }
    if (*environment == 0)
    {
        return {.status = EnvironmentReadStatus::Ok, .variables = {}, .truncated = false, .detail = {}}; // no block: nothing set
    }
    if (!isAligned(*environment, sizeof(wchar_t)))
    {
        return failure(EnvironmentReadStatus::Failed,
                       "the process parameters point at an environment block at an address that is not valid");
    }
    const std::expected<std::uintptr_t, DWORD> size = readField(api, process, layout, *parameters + layout.environmentSizeOffset);
    if (!size.has_value())
    {
        return readFailure(api, process, size.error(), "the process parameters");
    }

    // The block: as far as EnvironmentSize says, under the cap -- its terminator ends it sooner, before
    // any slack the allocation has past it -- or, where EnvironmentSize is 0, as far as the terminator;
    // and never past the highest address the layout's pointers reach.
    const auto declared = static_cast<std::size_t>(*size);
    const bool sized = declared != 0 && declared <= ENVIRONMENT_BLOCK_READ_CAP_BYTES;
    const std::size_t wanted = sized ? declared : ENVIRONMENT_BLOCK_READ_CAP_BYTES;
    const std::uintptr_t room = layout.highestAddress - *environment; // one less than the bytes from it to the top
    const std::size_t limit = (room < wanted) ? static_cast<std::size_t>(room) + 1 : wanted;
    const BlockRead block = readBlock(api, process, *environment, limit);
    if (block.bytes.empty() && block.error != 0)
    {
        return readFailure(api, process, block.error, "the environment block");
    }
    // Cut at the cap (or the top of the address space) before the size, or at a page that could not be
    // read before the terminator: what follows is unknown.
    const bool truncated = (block.reachedLimit && (!sized || limit < declared)) || block.error != 0;

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
        .status = EnvironmentReadStatus::Ok,
        .variables = Environment::parseEnvironBlock(utf8),
        .truncated = truncated,
        .detail = {},
    };
    if (result.variables.size() > ENVIRONMENT_VARIABLE_CAP)
    {
        result.variables.resize(ENVIRONMENT_VARIABLE_CAP);
        result.truncated = true;
    }
    return result;
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
    auto opened = openProcessVerified(
        {
            .openProcess = m_Api.openProcess,
            .getProcessTimes = m_Api.getProcessTimes,
            .closeHandle = m_Api.closeHandle,
            .getExitCodeProcess = m_Api.getExitCodeProcess,
        },
        target,
        PROCESS_VM_READ);
    if (!opened)
    {
        auto refused = toReadFailure<EnvironmentReadStatus>(opened.error());
        return failure(refused.status, std::move(refused.detail));
    }
    const InjectableHandle process = std::move(*opened);

    // A process of the reader's own pointer size keeps its live block behind the PEB that
    // ProcessBasicInformation gives. A 32-bit (WOW64) one seen from the 64-bit reader keeps it behind its
    // PEB32 instead (#3364): it has a 64-bit PEB too, but that one holds only the block it started with.
    const std::expected<TargetPeb, std::string> which = targetPeb(m_Api, process.get());
    if (!which.has_value())
    {
        return failure(EnvironmentReadStatus::Failed, which.error());
    }
    if (*which == TargetPeb::Unreachable)
    {
        return failure(EnvironmentReadStatus::Failed, "a 64-bit process, whose environment this 32-bit TaskSmack cannot read");
    }
    const std::expected<std::uintptr_t, EnvironmentReadResult> peb = pebAddress(m_Api, process.get(), *which);
    if (!peb.has_value())
    {
        return peb.error();
    }
    return readEnvironmentAt(m_Api, process.get(), *peb, (*which == TargetPeb::Wow64) ? WOW64_LAYOUT : NATIVE_LAYOUT);
}

// NOLINTEND(misc-include-cleaner)

} // namespace Platform::Windows
