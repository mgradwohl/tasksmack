#pragma once

// On-demand read of one process's environment variables (#179).
//
// Not part of the per-sample enumeration (ProcessCounters): a process's environment is read only for
// the process Process Details shows, only while its Environment section is open, and at most every
// Domain::Sampling::PROCESS_ENVIRONMENT_REFRESH_MS. The App composition root creates the reader
// (Platform::makeProcessEnvironmentReader()) and hands it to the panel, which never calls the
// factory itself.

#include "Platform/IProcessActions.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Platform
{

/// What came of one environment read.
enum class EnvironmentReadStatus : std::uint8_t
{
    Ok,               ///< Read; `variables` holds the entries (may be empty: a kernel thread or zombie has none).
    PermissionDenied, ///< The OS refused (Linux: EACCES/EPERM -- another user's process without ptrace rights).
    ProcessExited,    ///< The process is gone, or its PID now belongs to a different process (ENOENT/ESRCH).
    Unsupported,      ///< This platform cannot read another process's environment (Windows, synthetic runs).
    IdentityUnknown,  ///< The target's start time is unknown (0), so the process could not be confirmed; nothing was read.
    Failed,           ///< Any other error.
};

/// One NAME=VALUE entry, both already safe to draw: valid UTF-8 with control characters and invalid
/// bytes escaped (Platform::Environment::escapeForDisplay()).
struct EnvironmentVariable
{
    std::string name;
    std::string value;
};

/// The outcome of IProcessEnvironmentReader::readEnvironment(): a status, and the variables when Ok.
struct EnvironmentReadResult
{
    EnvironmentReadStatus status = EnvironmentReadStatus::Unsupported;
    std::vector<EnvironmentVariable> variables;
};

/// Reads one process's environment variables on request.
///
/// Implementations must never log the values they read (#179): an environment routinely carries API
/// keys, tokens and passwords.
class IProcessEnvironmentReader
{
  public:
    virtual ~IProcessEnvironmentReader() = default;

    IProcessEnvironmentReader() = default;
    IProcessEnvironmentReader(const IProcessEnvironmentReader&) = default;
    IProcessEnvironmentReader& operator=(const IProcessEnvironmentReader&) = default;
    IProcessEnvironmentReader(IProcessEnvironmentReader&&) = default;
    IProcessEnvironmentReader& operator=(IProcessEnvironmentReader&&) = default;

    /// Whether this platform can read a process's environment at all. When false the UI hides the
    /// Environment section and readEnvironment() only ever returns Unsupported.
    [[nodiscard]] virtual bool hasEnvironment() const = 0;

    /// Read @p target's environment now. Synchronous; one small /proc read on Linux.
    /// The target's start time is checked against the process holding the PID, so a process that reused
    /// it reports ProcessExited instead of showing a stranger's environment. An unknown start time (0)
    /// is refused with IdentityUnknown and nothing is read, as the process actions refuse it
    /// (Platform::checkProcessIdentity()): a PID alone cannot confirm whose environment it would be.
    [[nodiscard]] virtual EnvironmentReadResult readEnvironment(const ProcessTarget& target) = 0;
};

/// The reader for a platform (or run) that cannot read another process's environment.
/// Windows would need ReadProcessMemory on the target's PEB, which is out of scope (#179).
class UnsupportedProcessEnvironmentReader final : public IProcessEnvironmentReader
{
  public:
    [[nodiscard]] bool hasEnvironment() const override
    {
        return false;
    }

    [[nodiscard]] EnvironmentReadResult readEnvironment(const ProcessTarget& /*target*/) override
    {
        return {.status = EnvironmentReadStatus::Unsupported, .variables = {}};
    }
};

} // namespace Platform
