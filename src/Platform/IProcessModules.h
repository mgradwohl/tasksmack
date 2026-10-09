#pragma once

// On-demand read of the code modules one process has loaded (#802): the Modules section of Process
// Details. Windows lists them with EnumProcessModulesEx; Linux groups the file-backed mappings in
// /proc/[pid]/maps (Platform/Linux/ProcMapsParser.h).
//
// Not part of the per-sample enumeration: a process's modules are read only for the process Process
// Details shows, only while its Modules section is open, and at most every
// Domain::Sampling::PROCESS_MODULES_REFRESH_MS. The App composition root creates the reader
// (Platform::makeProcessModulesReader()) and hands it to the panel, which never calls the factory.
//
// The rows are raw: a full path, numbers, the version as its four parts. Display names, hex addresses
// and version strings are Domain's (Domain/ProcessModules.h).

#include "Platform/IProcessActions.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Platform
{

/// What came of one modules read.
enum class ModulesReadStatus : std::uint8_t
{
    Ok,               ///< Read; `modules` holds them (may be empty).
    PermissionDenied, ///< The OS refused (a protected or elevated process, or another user's).
    ProcessExited,    ///< The process is gone, or its PID now belongs to a different process.
    Unsupported,      ///< This run cannot list another process's modules (synthetic runs).
    IdentityUnknown,  ///< The target's start time is unknown (0), so the process could not be confirmed; nothing was read.
    Failed,           ///< Any other error.
};

/// A module's file version (VS_FIXEDFILEINFO's dwFileVersionMS/LS on Windows), as four parts.
struct ModuleVersion
{
    std::uint16_t major = 0;
    std::uint16_t minor = 0;
    std::uint16_t build = 0;
    std::uint16_t revision = 0;

    [[nodiscard]] friend constexpr auto operator<=>(const ModuleVersion&, const ModuleVersion&) noexcept = default;
};

/// One loaded module: an executable image or shared library.
struct ProcessModule
{
    std::string path;                     ///< Full path, UTF-8 ("C:\Windows\System32\ntdll.dll", "/usr/lib/libc.so.6").
    std::uint64_t baseAddress = 0;        ///< Where it is loaded (Linux: its lowest mapping).
    std::uint64_t sizeBytes = 0;          ///< Its image size (Linux: the sum of its mappings' spans).
    std::optional<ModuleVersion> version; ///< When the file carries one (Windows); Linux has none.
    bool deleted = false;                 ///< The file was deleted or replaced after it was mapped (Linux).
};

/// The outcome of IProcessModulesReader::readModules(): a status, and the modules when Ok.
struct ModulesReadResult
{
    ModulesReadStatus status = ModulesReadStatus::Unsupported;
    std::vector<ProcessModule> modules;
    /// For Failed, what failed, if known (the OS's error message); else empty.
    std::string detail;
};

/// Lists one process's loaded modules on request.
class IProcessModulesReader
{
  public:
    virtual ~IProcessModulesReader() = default;

    IProcessModulesReader() = default;
    IProcessModulesReader(const IProcessModulesReader&) = default;
    IProcessModulesReader& operator=(const IProcessModulesReader&) = default;
    IProcessModulesReader(IProcessModulesReader&&) = default;
    IProcessModulesReader& operator=(IProcessModulesReader&&) = default;

    /// Whether this run can list a process's modules at all. When false the UI hides the Modules
    /// section and readModules() only ever returns Unsupported.
    [[nodiscard]] virtual bool hasModules() const = 0;

    /// Read @p target's modules now. Synchronous, and possibly slow (file version reads): the view
    /// runs it on a worker. The target's start time is checked against the process holding the PID,
    /// so a reused PID reports ProcessExited; an unknown start time (0) is refused with IdentityUnknown.
    [[nodiscard]] virtual ModulesReadResult readModules(const ProcessTarget& target) = 0;
};

/// The reader for a run that cannot list another process's modules (synthetic scenarios).
class UnsupportedProcessModulesReader final : public IProcessModulesReader
{
  public:
    [[nodiscard]] bool hasModules() const override
    {
        return false;
    }

    [[nodiscard]] ModulesReadResult readModules(const ProcessTarget& /*target*/) override
    {
        return {.status = ModulesReadStatus::Unsupported, .modules = {}, .detail = {}};
    }
};

} // namespace Platform
