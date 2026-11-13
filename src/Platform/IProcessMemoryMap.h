#pragma once

// On-demand read of one process's memory map (#3089): its address space as regions, each with its
// protection, what backs it (a file, the heap, a stack, anonymous memory) and, where the OS measures it,
// how much of it is resident, shared, private and swapped out. Linux reads /proc/[pid]/smaps
// (Platform/Linux/SmapsParser.h), one mapping per entry; Windows walks the address space with
// VirtualQueryEx, one mapping per region, naming a mapped file with GetMappedFileNameW (free regions are
// left out).
//
// Not part of the per-sample enumeration: a process's map is read only for the process Process Details
// shows, only while its Memory map section is open. The App composition root creates the reader
// (App::makeProcessMemoryMapReader(), over Platform::makeProcessMemoryMapReader()) and hands it to the
// panel, which never calls the factory.
//
// The rows are raw: addresses, flags and byte counts. Totals by kind and by file, labels and the
// protection text are Domain's (Domain/ProcessMemoryMap.h).

#include "Platform/IProcessActions.h"

#include <cstdint>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace Platform
{

/// What came of one memory map read.
enum class MemoryMapReadStatus : std::uint8_t
{
    Ok,               ///< Read; `mappings` holds them (at least one while the process runs).
    PermissionDenied, ///< The OS refused (another user's process, or a protected or elevated one).
    ProcessExited,    ///< The process is gone, or its PID now belongs to a different process.
    Unsupported,      ///< This run cannot read another process's memory map (synthetic runs).
    IdentityUnknown,  ///< The target's start time is unknown (0), so the process could not be confirmed; nothing was read.
    Failed,           ///< Any other error.
};

/// What backs a mapping. The values travel in Windows' helper's MemoryMapResult (BrokerProtocol.h, #3665),
/// whose decoder refuses one past Other: a new kind goes last, with a protocol version bump.
enum class MemoryMappingKind : std::uint8_t
{
    /// An executable image: Windows MEM_IMAGE; on Linux a mapping of a file that has at least one executable
    /// mapping, the main executable and every shared object, as the Modules section groups them.
    Image,
    File,      ///< Any other mapped file: Windows MEM_MAPPED with a file name; Linux every other file mapping.
    Anonymous, ///< Unnamed private memory: Windows MEM_PRIVATE; Linux a mapping with no name.
    Heap,      ///< The process's heap: Linux [heap].
    Stack,     ///< A stack: Linux [stack].
    System,    ///< Kernel-provided pages: Linux [vdso], [vvar], [vvar_vclock], [vsyscall], [uprobes].
    /// A named special mapping: Linux [anon:name], [anon_shmem:name], anon_inode:...; Windows MEM_MAPPED
    /// with no file (a pagefile-backed section).
    Other,
};

/// One mapping (Linux) or region (Windows) of the process's address space, [start, end).
///
/// The sizes past sizeBytes() are what the OS measured, absent where it doesn't: Linux's smaps gives every
/// one (an older kernel lacks SwapPss), Windows' VirtualQueryEx none, though its lane may fill residentBytes
/// and the private and shared split from QueryWorkingSetEx. A field the platform never fills is documented
/// at the field.
struct MemoryMapping
{
    std::uint64_t start = 0; ///< The first byte's address.
    std::uint64_t end = 0;   ///< One past the last byte's address.
    bool readable = false;
    bool writable = false; ///< Copy-on-write (Linux private writable, Windows PAGE_WRITECOPY) reads writable and not shared.
    bool executable = false;
    /// Linux 's' (a shared mapping) rather than 'p'; Windows MEM_MAPPED or MEM_IMAGE (section-backed), false
    /// for MEM_PRIVATE and for copy-on-write protection.
    bool shared = false;
    bool guard = false;    ///< Windows PAGE_GUARD; never on Linux.
    bool reserved = false; ///< Windows MEM_RESERVE: address space only, nothing behind it; never on Linux.
    MemoryMappingKind kind = MemoryMappingKind::Anonymous;
    /// The file's full path, UTF-8, for Image and File ("/usr/lib/libc.so.6", "C:\Windows\System32\ntdll.dll");
    /// the kernel's name for Heap, Stack, System and Other ("[heap]", "[anon:dalvik-main]"); empty for Anonymous.
    // NOLINTNEXTLINE(readability-redundant-member-init) - {} lets designated initializers omit it without a warning
    std::string path{};
    bool deleted = false;         ///< The file was deleted or replaced after it was mapped (Linux " (deleted)").
    std::uint64_t fileOffset = 0; ///< Where in the file the mapping starts (Linux); 0 on Windows.
    // NOLINTBEGIN(readability-redundant-member-init) - {} lets designated initializers omit them without a warning
    std::optional<std::uint64_t> residentBytes{}; ///< In memory now (Linux Rss).
    std::optional<std::uint64_t> pssBytes{};      ///< Proportional set size: resident, shared pages split among their sharers (Linux Pss).
    std::optional<std::uint64_t> sharedCleanBytes{};  ///< Resident, shared with another process, unmodified.
    std::optional<std::uint64_t> sharedDirtyBytes{};  ///< Resident, shared with another process, modified.
    std::optional<std::uint64_t> privateCleanBytes{}; ///< Resident, this process's alone, unmodified.
    std::optional<std::uint64_t> privateDirtyBytes{}; ///< Resident, this process's alone, modified.
    std::optional<std::uint64_t> swapBytes{};         ///< Swapped out (Linux Swap).
    std::optional<std::uint64_t> swapPssBytes{};      ///< Swapped out, shared pages split among their sharers (Linux SwapPss, 4.3 on).
    // NOLINTEND(readability-redundant-member-init)

    /// The mapping's span of address space (end - start; 0 for a malformed one whose end is before its start).
    [[nodiscard]] constexpr std::uint64_t sizeBytes() const noexcept
    {
        return end > start ? end - start : 0;
    }

    friend bool operator==(const MemoryMapping&, const MemoryMapping&) = default;
};

/// The outcome of IProcessMemoryMapReader::readMemoryMap(): a status, and the mappings when Ok.
struct MemoryMapReadResult
{
    MemoryMapReadStatus status = MemoryMapReadStatus::Unsupported;
    std::vector<MemoryMapping> mappings; ///< In address order.
    /// For Failed, what failed, if known (the OS's error message); else empty.
    std::string detail;
    /// The process had more than were passed on (a read past its cap, or the administrator helper's reply,
    /// which holds one frame); `mappings` holds the first.
    bool truncated = false;
};

/// Reads one process's memory map on request.
class IProcessMemoryMapReader
{
  public:
    virtual ~IProcessMemoryMapReader() = default;

    IProcessMemoryMapReader() = default;
    IProcessMemoryMapReader(const IProcessMemoryMapReader&) = default;
    IProcessMemoryMapReader& operator=(const IProcessMemoryMapReader&) = default;
    IProcessMemoryMapReader(IProcessMemoryMapReader&&) = default;
    IProcessMemoryMapReader& operator=(IProcessMemoryMapReader&&) = default;

    /// Whether this run can read a process's memory map at all. When false the UI hides the Memory map
    /// section and readMemoryMap() only ever returns Unsupported.
    [[nodiscard]] virtual bool hasMemoryMap() const = 0;

    /// Read @p target's memory map now. Synchronous, and possibly slow (smaps walks every page table; a
    /// large process takes tens of milliseconds): the view runs it on a worker. The target's start time is
    /// checked against the process holding the PID, so a reused PID reports ProcessExited; an unknown start
    /// time (0) is refused with IdentityUnknown.
    [[nodiscard]] virtual MemoryMapReadResult readMemoryMap(const ProcessTarget& target) = 0;

    /// Whether readAuthorized() can read as an administrator, after a UAC prompt (Windows' helper,
    /// BrokeredMemoryMapReader, #3665). The UI then offers "Authenticate to read" on PermissionDenied.
    [[nodiscard]] virtual bool canReadAuthorized() const
    {
        return false;
    }

    /// Reads @p target as an administrator, asking for the password first if need be. Blocks for as long as
    /// the prompt is up, so it runs on a worker. The default reads as readMemoryMap() does.
    [[nodiscard]] virtual MemoryMapReadResult readAuthorized(const ProcessTarget& target, const std::stop_token& stopToken)
    {
        static_cast<void>(stopToken);
        return readMemoryMap(target);
    }
};

/// The reader for a run that cannot read another process's memory map (synthetic scenarios).
class UnsupportedProcessMemoryMapReader final : public IProcessMemoryMapReader
{
  public:
    [[nodiscard]] bool hasMemoryMap() const override
    {
        return false;
    }

    [[nodiscard]] MemoryMapReadResult readMemoryMap(const ProcessTarget& /*target*/) override
    {
        return {.status = MemoryMapReadStatus::Unsupported, .mappings = {}, .detail = {}};
    }
};

} // namespace Platform
