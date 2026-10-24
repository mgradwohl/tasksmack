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
#include <stop_token>
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
    Unsupported,      ///< This run cannot read another process's environment (synthetic runs).
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
    /// The process had more than were passed on (the administrator helper's reply holds one frame,
    /// #2403; the Windows reader caps the block and the count, #2569); `variables` holds the first.
    bool truncated = false;
    /// Why, for Failed, when the reader can say (the Windows reader's: a Win32 or NTSTATUS error, or a
    /// 32-bit process it does not read yet, #2569); else empty. Drawn after the status line. Never a
    /// value. Not carried in the administrator helper's reply (BrokerProtocol), which words its own.
    // NOLINTNEXTLINE(readability-redundant-member-init) - {} lets designated initializers omit it without a warning
    std::string detail{};
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

    /// Read @p target's environment now. Synchronous; one small /proc read on Linux, a few
    /// ReadProcessMemory calls on Windows.
    /// The target's start time is checked against the process holding the PID, so a process that reused
    /// it reports ProcessExited instead of showing a stranger's environment. An unknown start time (0)
    /// is refused with IdentityUnknown and nothing is read, as the process actions refuse it
    /// (Platform::checkProcessIdentity()): a PID alone cannot confirm whose environment it would be.
    [[nodiscard]] virtual EnvironmentReadResult readEnvironment(const ProcessTarget& target) = 0;

    /// Whether readAuthorized() can read as an administrator, after a password prompt (Linux: the
    /// pkexec-started helper, #2309). The UI then offers "Authenticate to read" on PermissionDenied.
    [[nodiscard]] virtual bool canReadAuthorized() const
    {
        return false;
    }

    /// Reads @p target's environment as an administrator, asking for the password first if need be
    /// (#2309). Blocks for as long as the prompt is up, so it runs on a worker. The default reads as
    /// readEnvironment() does.
    [[nodiscard]] virtual EnvironmentReadResult readAuthorized(const ProcessTarget& target, const std::stop_token& stopToken)
    {
        static_cast<void>(stopToken);
        return readEnvironment(target);
    }
};

/// The reader for a run that cannot read another process's environment: synthetic runs
/// (App/SyntheticScenario). Both platforms have a reader of their own (Linux: /proc/[pid]/environ,
/// #179; Windows: the target's PEB through ReadProcessMemory, #2569).
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
