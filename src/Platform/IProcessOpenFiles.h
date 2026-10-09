#pragma once

// On-demand read of the files one process has open (#183): the Open files section of Process
// Details, beside Modules (IProcessModules.h) and on the same terms: read only for the process
// Process Details shows, only while the section is open, at most every
// Domain::Sampling::PROCESS_OPEN_FILES_REFRESH_MS, on a worker. The App composition root creates the
// reader (Platform::makeProcessOpenFilesReader()) and hands it to the panel.
//
// Linux reads /proc/[pid]/fd (ProcFdParser.h); Windows enumerates the system handle table filtered
// to the process and names its File handles (WindowsProcessOpenFiles.h). Neither spawns lsof.

#include "Platform/IProcessActions.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Platform
{

/// What came of one open-files read.
enum class OpenFilesReadStatus : std::uint8_t
{
    Ok,               ///< Read; `files` holds them (may be empty).
    PermissionDenied, ///< The OS refused (a protected or elevated process, or another user's).
    ProcessExited,    ///< The process is gone, or its PID now belongs to a different process.
    Unsupported,      ///< This run cannot list another process's files (synthetic runs).
    IdentityUnknown,  ///< The target's start time is unknown (0); nothing was read.
    Failed,           ///< Any other error.
};

/// What an open descriptor or handle refers to.
enum class OpenFileKind : std::uint8_t
{
    File,
    Directory,
    Device,    ///< A character or block device (/dev/..., a console)
    Socket,    ///< "socket:[inode]"
    Pipe,      ///< "pipe:[inode]", or a Windows pipe
    AnonInode, ///< "anon_inode:[eventfd]", ... (Linux)
    Memfd,     ///< "memfd:name" (Linux)
    Other,     ///< Anything else ("net:[...]" namespaces, a handle that was not queried)
};

/// One open descriptor (Linux) or File handle (Windows).
struct OpenFile
{
    std::uint64_t descriptor = 0; ///< The fd, or the handle value.
    OpenFileKind kind = OpenFileKind::Other;
    std::string path;                   ///< The path, UTF-8, or the kernel's name ("socket:[1234]"); may be empty.
    std::optional<std::uint32_t> flags; ///< The open flags from /proc/[pid]/fdinfo (Linux), when read.
    bool deleted = false;               ///< The file was deleted after it was opened (Linux).
};

/// The outcome of IProcessOpenFilesReader::readOpenFiles().
struct OpenFilesReadResult
{
    OpenFilesReadStatus status = OpenFilesReadStatus::Unsupported;
    std::vector<OpenFile> files;
    std::string detail;           ///< For Failed, what failed (the OS's message), if known.
    bool truncated = false;       ///< More were open than a read lists (MAX_OPEN_FILES); `files` holds the first.
    bool namesIncomplete = false; ///< Some names were not read: a handle did not answer in time (Windows).
    bool hexDescriptors = false;  ///< Descriptors are Windows handle values, shown in hex.
};

/// The most entries one read lists: a process can hold hundreds of thousands of descriptors.
inline constexpr std::size_t MAX_OPEN_FILES = 10000;

/// Lists one process's open files on request.
class IProcessOpenFilesReader
{
  public:
    virtual ~IProcessOpenFilesReader() = default;

    IProcessOpenFilesReader() = default;
    IProcessOpenFilesReader(const IProcessOpenFilesReader&) = default;
    IProcessOpenFilesReader& operator=(const IProcessOpenFilesReader&) = default;
    IProcessOpenFilesReader(IProcessOpenFilesReader&&) = default;
    IProcessOpenFilesReader& operator=(IProcessOpenFilesReader&&) = default;

    /// Whether this run can list a process's open files. When false the UI hides the section.
    [[nodiscard]] virtual bool hasOpenFiles() const = 0;

    /// Read @p target's open files now. Synchronous and possibly slow: the view runs it on a worker.
    /// A reused PID reports ProcessExited; an unknown start time (0) is refused with IdentityUnknown.
    [[nodiscard]] virtual OpenFilesReadResult readOpenFiles(const ProcessTarget& target) = 0;
};

/// The reader for a run that cannot list another process's open files (synthetic scenarios).
class UnsupportedProcessOpenFilesReader final : public IProcessOpenFilesReader
{
  public:
    [[nodiscard]] bool hasOpenFiles() const override
    {
        return false;
    }

    [[nodiscard]] OpenFilesReadResult readOpenFiles(const ProcessTarget& /*target*/) override
    {
        return {};
    }
};

} // namespace Platform
