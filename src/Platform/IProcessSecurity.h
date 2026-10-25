#pragma once

// On-demand read of one process's security context (#1526): the Security section of Process Details.
// Linux reads /proc/[pid]/status (user and group IDs, supplementary groups, capability sets,
// no_new_privs, seccomp), /proc/[pid]/attr/current (the SELinux or AppArmor label) and
// /proc/[pid]/cgroup. Windows reads the process token (OpenProcessToken, GetTokenInformation): its
// user, integrity level, elevation, privileges, groups, AppContainer and virtualization state, and
// session.
//
// Not part of the per-sample enumeration: it is read only for the process Process Details shows, only
// while its Security section is open, at most every Domain::Sampling::PROCESS_SECURITY_REFRESH_MS, and
// on a worker thread, one read at a time.
// The App composition root creates the reader (Platform::makeProcessSecurityReader()) and hands it to
// the panel, which never calls the factory.
//
// The fields are raw: numeric IDs and SIDs with the names the OS maps them to, capability sets as bit
// masks, the integrity level as its mandatory-label RID.
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
    Unsupported,      ///< This run cannot read another process's security context (synthetic runs).
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

/// A Windows security identifier (SID) and the account it names.
struct SecurityAccount
{
    std::string sid;  ///< The SID in string form ("S-1-5-32-544").
    std::string name; ///< "DOMAIN\name" from LookupAccountSid; empty when the SID does not resolve.

    [[nodiscard]] friend bool operator==(const SecurityAccount&, const SecurityAccount&) = default;
};

/// A Windows token's elevation type (TOKEN_ELEVATION_TYPE).
enum class TokenElevationKind : std::uint8_t
{
    Default = 1, ///< No split token: UAC is off, or the account is a standard user or a service.
    Full = 2,    ///< The elevated half of a split (UAC) token.
    Limited = 3, ///< The filtered half of a split token: an administrator running unelevated.
};

/// One privilege in a Windows token (TOKEN_PRIVILEGES), with whether it is enabled now.
struct TokenPrivilege
{
    std::string name; ///< "SeDebugPrivilege"; "LUID 0x..." when the name cannot be looked up.
    bool enabled = false;

    [[nodiscard]] friend bool operator==(const TokenPrivilege&, const TokenPrivilege&) = default;
};

/// One group in a Windows token (TOKEN_GROUPS) and how the token holds it.
struct TokenGroup
{
    SecurityAccount account;
    bool enabled = false;  ///< SE_GROUP_ENABLED: access checks use it.
    bool denyOnly = false; ///< SE_GROUP_USE_FOR_DENY_ONLY: it only denies (a filtered administrator token's Administrators).

    [[nodiscard]] friend bool operator==(const TokenGroup&, const TokenGroup&) = default;
};

/// One process's security context, as far as the platform reports it. Every field is optional: what a
/// platform or kernel does not report is left empty rather than shown as a made-up default. The first
/// block is Linux's, the second the Windows token's.
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

    std::optional<SecurityAccount> tokenUser;              ///< TokenUser.
    std::optional<std::uint32_t> integrityLevel;           ///< TokenIntegrityLevel's RID (SECURITY_MANDATORY_*_RID).
    std::optional<bool> elevated;                          ///< TokenElevation.
    std::optional<TokenElevationKind> elevationType;       ///< TokenElevationType.
    std::optional<std::vector<TokenPrivilege>> privileges; ///< TokenPrivileges, in the token's order.
    std::optional<std::vector<TokenGroup>> tokenGroups;    ///< TokenGroups, less the integrity label.
    std::optional<bool> appContainer;                      ///< TokenIsAppContainer.
    std::optional<bool> virtualizationEnabled;             ///< TokenVirtualizationEnabled (UAC file/registry virtualization).
    std::optional<std::uint32_t> sessionId;                ///< TokenSessionId.

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

    /// Read @p target's security context now. Synchronous, and it can block: a few small files on Linux,
    /// but on Windows naming the token's SIDs can wait on a domain controller, so callers run it off
    /// the UI thread (the Security section reads on a worker). The target's start time is checked against the process holding the PID, so a
    /// reused PID reports ProcessExited; an unknown start time (0) is refused with IdentityUnknown.
    [[nodiscard]] virtual SecurityReadResult readSecurity(const ProcessTarget& target) = 0;
};

/// The reader for a run that cannot read another process's security context (synthetic scenarios).
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
