#pragma once

// On-demand read of one process's security context (#1526): the Security section of Process Details.
// Linux reads /proc/[pid]/status (user and group IDs, supplementary groups, capability sets,
// no_new_privs, seccomp), /proc/[pid]/attr/current (the SELinux or AppArmor label) and
// /proc/[pid]/cgroup.
//
// Not part of the per-sample enumeration: it is read only for the process Process Details shows, only
// while its Security section is open, and at most every Domain::Sampling::PROCESS_SECURITY_REFRESH_MS.
// The App composition root creates the reader (Platform::makeProcessSecurityReader()) and hands it to
// the panel, which never calls the factory.
//
// The fields are raw: numeric IDs with the names the OS maps them to, capability sets as bit masks.
// Capability names and the display text are Domain's (Domain/ProcessSecurity.h).

#include "Platform/IProcessActions.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Platform
{

/// What came of one security read.
enum class SecurityReadStatus : std::uint8_t
{
    Ok,               ///< Read; `security` holds what the platform reported.
    PermissionDenied, ///< The OS refused (a protected process, or another user's where that is hidden).
    ProcessExited,    ///< The process is gone, or its PID now belongs to a different process.
    Unsupported,      ///< This run cannot read another process's security context (Windows for now, synthetic runs).
    IdentityUnknown,  ///< The target's start time is unknown (0), so the process could not be confirmed; nothing was read.
    Failed,           ///< Any other error.
};

/// A user or group ID with the name the system's databases give it (empty when it has none).
struct SecurityPrincipal
{
    std::uint32_t id = 0;
    std::string name;

    [[nodiscard]] friend bool operator==(const SecurityPrincipal&, const SecurityPrincipal&) = default;
};

/// The four IDs Linux keeps for each of a process's user and group: real, effective, saved set and
/// filesystem (credentials(7)).
struct SecurityIdSet
{
    SecurityPrincipal real;
    SecurityPrincipal effective;
    SecurityPrincipal saved;
    SecurityPrincipal filesystem;

    [[nodiscard]] friend bool operator==(const SecurityIdSet&, const SecurityIdSet&) = default;
};

/// A process's capability sets (capabilities(7)), as the bit masks /proc/[pid]/status shows. A set the
/// kernel does not report (CapAmb before Linux 4.3) is empty.
struct CapabilitySets
{
    std::optional<std::uint64_t> effective;
    std::optional<std::uint64_t> permitted;
    std::optional<std::uint64_t> inheritable;
    std::optional<std::uint64_t> bounding;
    std::optional<std::uint64_t> ambient;

    [[nodiscard]] friend bool operator==(const CapabilitySets&, const CapabilitySets&) = default;
};

/// The seccomp mode of a process (/proc/[pid]/status "Seccomp:", seccomp(2)).
enum class SeccompMode : std::uint8_t
{
    Disabled = 0,
    Strict = 1,
    Filter = 2,
};

/// One process's security context, as far as the platform reports it. Every field is optional: what a
/// platform or kernel does not report is left empty rather than shown as a made-up default.
struct ProcessSecurity
{
    std::optional<SecurityIdSet> users;
    std::optional<SecurityIdSet> groups;
    std::vector<SecurityPrincipal> supplementaryGroups; ///< In the order the kernel lists them.
    CapabilitySets capabilities;
    std::optional<bool> noNewPrivileges; ///< prctl(PR_SET_NO_NEW_PRIVS) is set.
    std::optional<SeccompMode> seccomp;  ///< Absent when the kernel lacks seccomp.
    std::string securityLabel;           ///< SELinux context or AppArmor profile; empty with no LSM label.
    std::string controlGroup;            ///< The cgroup v2 path ("/user.slice/..."); else the v1 lines joined.

    [[nodiscard]] friend bool operator==(const ProcessSecurity&, const ProcessSecurity&) = default;
};

/// The outcome of IProcessSecurityReader::readSecurity(): a status, and the security context when Ok.
struct SecurityReadResult
{
    SecurityReadStatus status = SecurityReadStatus::Unsupported;
    ProcessSecurity security;
    /// For Failed, what failed, if known (the OS's error message); else empty.
    std::string detail;
};

/// Reads one process's security context on request.
class IProcessSecurityReader
{
  public:
    virtual ~IProcessSecurityReader() = default;

    IProcessSecurityReader() = default;
    IProcessSecurityReader(const IProcessSecurityReader&) = default;
    IProcessSecurityReader& operator=(const IProcessSecurityReader&) = default;
    IProcessSecurityReader(IProcessSecurityReader&&) = default;
    IProcessSecurityReader& operator=(IProcessSecurityReader&&) = default;

    /// Whether this run can read a process's security context at all. When false the UI hides the
    /// Security section and readSecurity() only ever returns Unsupported.
    [[nodiscard]] virtual bool hasSecurity() const = 0;

    /// Read @p target's security context now. Synchronous and quick (a few small files). The target's
    /// start time is checked against the process holding the PID, so a reused PID reports
    /// ProcessExited; an unknown start time (0) is refused with IdentityUnknown.
    [[nodiscard]] virtual SecurityReadResult readSecurity(const ProcessTarget& target) = 0;
};

/// The reader for a run that cannot read another process's security context (Windows until its token
/// reader lands, synthetic scenarios).
class UnsupportedProcessSecurityReader final : public IProcessSecurityReader
{
  public:
    [[nodiscard]] bool hasSecurity() const override
    {
        return false;
    }

    [[nodiscard]] SecurityReadResult readSecurity(const ProcessTarget& /*target*/) override
    {
        return {.status = SecurityReadStatus::Unsupported, .security = {}, .detail = {}};
    }
};

} // namespace Platform
