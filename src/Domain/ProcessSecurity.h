#pragma once

// Display text for Process Details' Security section (#1526): capability names from their bit numbers,
// and the short phrases the section shows for IDs, seccomp and no_new_privs; and for a Windows token,
// its accounts, integrity level, elevation type, privileges and groups. Pure functions over
// Platform::ProcessSecurity's raw fields, so they are unit-tested without a platform
// (tests/Domain/test_ProcessSecurity.cpp).

#include "Platform/IProcessSecurity.h"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <vector>

namespace Domain::ProcessSecurity
{

/// capability.h's names by bit number (capabilities(7)), through CAP_CHECKPOINT_RESTORE (40), the last
/// one Linux defines today. A newer kernel's higher bit is named by its number (capabilityName()).
inline constexpr std::array<std::string_view, 41> CAPABILITY_NAMES{
    "CAP_CHOWN",              // 0
    "CAP_DAC_OVERRIDE",       // 1
    "CAP_DAC_READ_SEARCH",    // 2
    "CAP_FOWNER",             // 3
    "CAP_FSETID",             // 4
    "CAP_KILL",               // 5
    "CAP_SETGID",             // 6
    "CAP_SETUID",             // 7
    "CAP_SETPCAP",            // 8
    "CAP_LINUX_IMMUTABLE",    // 9
    "CAP_NET_BIND_SERVICE",   // 10
    "CAP_NET_BROADCAST",      // 11
    "CAP_NET_ADMIN",          // 12
    "CAP_NET_RAW",            // 13
    "CAP_IPC_LOCK",           // 14
    "CAP_IPC_OWNER",          // 15
    "CAP_SYS_MODULE",         // 16
    "CAP_SYS_RAWIO",          // 17
    "CAP_SYS_CHROOT",         // 18
    "CAP_SYS_PTRACE",         // 19
    "CAP_SYS_PACCT",          // 20
    "CAP_SYS_ADMIN",          // 21
    "CAP_SYS_BOOT",           // 22
    "CAP_SYS_NICE",           // 23
    "CAP_SYS_RESOURCE",       // 24
    "CAP_SYS_TIME",           // 25
    "CAP_SYS_TTY_CONFIG",     // 26
    "CAP_MKNOD",              // 27
    "CAP_LEASE",              // 28
    "CAP_AUDIT_WRITE",        // 29
    "CAP_AUDIT_CONTROL",      // 30
    "CAP_SETFCAP",            // 31
    "CAP_MAC_OVERRIDE",       // 32
    "CAP_MAC_ADMIN",          // 33
    "CAP_SYSLOG",             // 34
    "CAP_WAKE_ALARM",         // 35
    "CAP_BLOCK_SUSPEND",      // 36
    "CAP_AUDIT_READ",         // 37
    "CAP_PERFMON",            // 38
    "CAP_BPF",                // 39
    "CAP_CHECKPOINT_RESTORE", // 40
};

/// The name of capability bit @p bit: its capability.h name, or "cap_<bit>" for one this build does not
/// know, as capsh(1) prints it.
[[nodiscard]] inline std::string capabilityName(unsigned bit)
{
    if (bit < CAPABILITY_NAMES.size())
    {
        return std::string(CAPABILITY_NAMES[bit]);
    }
    return std::format("cap_{}", bit);
}

/// The names of the capabilities set in @p mask, lowest bit first.
[[nodiscard]] inline std::vector<std::string> capabilityNames(std::uint64_t mask)
{
    std::vector<std::string> names;
    names.reserve(static_cast<std::size_t>(std::popcount(mask)));
    for (unsigned bit = 0; bit < 64; ++bit)
    {
        if (((mask >> bit) & 1U) != 0)
        {
            names.push_back(capabilityName(bit));
        }
    }
    return names;
}

/// Whether @p mask holds every capability this build knows: the full set root has by default. The
/// section then says "all" rather than listing forty-one names.
[[nodiscard]] constexpr bool hasAllKnownCapabilities(std::uint64_t mask) noexcept
{
    constexpr std::uint64_t ALL_KNOWN = (std::uint64_t{1} << CAPABILITY_NAMES.size()) - 1;
    return (mask & ALL_KNOWN) == ALL_KNOWN;
}

/// One capability set as the section shows it: "none", "all (41)", or the names joined with ", ".
[[nodiscard]] inline std::string capabilitySetText(std::uint64_t mask)
{
    if (mask == 0)
    {
        return "none";
    }
    if (hasAllKnownCapabilities(mask))
    {
        const int extra = std::popcount(mask) - static_cast<int>(CAPABILITY_NAMES.size());
        return (extra > 0) ? std::format("all ({} + {} unknown)", CAPABILITY_NAMES.size(), extra)
                           : std::format("all ({})", CAPABILITY_NAMES.size());
    }
    std::string text;
    for (const std::string& name : capabilityNames(mask))
    {
        if (!text.empty())
        {
            text += ", ";
        }
        text += name;
    }
    return text;
}

/// A user or group as the section shows it: "1000 (matt)", or the bare number with no name.
[[nodiscard]] inline std::string principalText(const Platform::SecurityPrincipal& principal)
{
    return principal.name.empty() ? std::to_string(principal.id) : std::format("{} ({})", principal.id, principal.name);
}

/// An ID set as one line: the effective ID, then whichever of real, saved and filesystem differ from
/// it, so the usual case (all four the same) reads as just "1000 (matt)".
[[nodiscard]] inline std::string idSetText(const Platform::SecurityIdSet& ids)
{
    std::string text = principalText(ids.effective);
    const auto addIfDifferent = [&text, &ids](std::string_view label, const Platform::SecurityPrincipal& principal)
    {
        if (principal.id != ids.effective.id)
        {
            text += std::format(", {} {}", label, principalText(principal));
        }
    };
    addIfDifferent("real", ids.real);
    addIfDifferent("saved", ids.saved);
    addIfDifferent("filesystem", ids.filesystem);
    return text;
}

/// The seccomp mode in words.
[[nodiscard]] constexpr std::string_view seccompText(Platform::SeccompMode mode) noexcept
{
    switch (mode)
    {
    case Platform::SeccompMode::Disabled:
        return "Disabled";
    case Platform::SeccompMode::Strict:
        return "Strict";
    case Platform::SeccompMode::Filter:
        return "Filter";
    }
    return "Unknown";
}

/// The supplementary groups joined with ", ", or "none".
[[nodiscard]] inline std::string groupListText(const std::vector<Platform::SecurityPrincipal>& groups)
{
    if (groups.empty())
    {
        return "none";
    }
    std::string text;
    for (const Platform::SecurityPrincipal& group : groups)
    {
        if (!text.empty())
        {
            text += ", ";
        }
        text += principalText(group);
    }
    return text;
}

/// A Windows account as the section shows it: "DOMAIN\name (S-1-...)", or the bare SID when it does not
/// resolve to a name.
[[nodiscard]] inline std::string accountText(const Platform::SecurityAccount& account)
{
    if (account.name.empty())
    {
        return account.sid;
    }
    return account.sid.empty() ? account.name : std::format("{} ({})", account.name, account.sid);
}

/// A Windows integrity level, from its mandatory-label RID (SECURITY_MANDATORY_*_RID in winnt.h):
/// "Medium", or the RID in hex for one between the named levels.
[[nodiscard]] inline std::string integrityLevelText(std::uint32_t rid)
{
    switch (rid)
    {
    case 0x0000:
        return "Untrusted";
    case 0x1000:
        return "Low";
    case 0x2000:
        return "Medium";
    case 0x2100:
        return "Medium Plus";
    case 0x3000:
        return "High";
    case 0x4000:
        return "System";
    case 0x5000:
        return "Protected Process";
    default:
        break;
    }
    return std::format("0x{:04X}", rid);
}

/// A Windows token's elevation type in words.
[[nodiscard]] constexpr std::string_view elevationTypeText(Platform::TokenElevationKind type) noexcept
{
    switch (type)
    {
    case Platform::TokenElevationKind::Default:
        return "Default (not a split token)";
    case Platform::TokenElevationKind::Full:
        return "Full (elevated)";
    case Platform::TokenElevationKind::Limited:
        return "Limited (filtered)";
    }
    return "Unknown";
}

/// A token's privileges one per line, each with its state ("SeShutdownPrivilege: Disabled"), or "none".
[[nodiscard]] inline std::string privilegeListText(const std::vector<Platform::TokenPrivilege>& privileges)
{
    if (privileges.empty())
    {
        return "none";
    }
    std::string text;
    for (const Platform::TokenPrivilege& privilege : privileges)
    {
        if (!text.empty())
        {
            text += '\n';
        }
        text += std::format("{}: {}", privilege.name, privilege.enabled ? "Enabled" : "Disabled");
    }
    return text;
}

/// A token's groups one per line: the account's name (its SID when it has none), marked "(deny only)"
/// or "(disabled)" unless enabled; or "none".
[[nodiscard]] inline std::string tokenGroupListText(const std::vector<Platform::TokenGroup>& groups)
{
    if (groups.empty())
    {
        return "none";
    }
    std::string text;
    for (const Platform::TokenGroup& group : groups)
    {
        if (!text.empty())
        {
            text += '\n';
        }
        text += group.account.name.empty() ? group.account.sid : group.account.name;
        if (group.denyOnly)
        {
            text += " (deny only)";
        }
        else if (!group.enabled)
        {
            text += " (disabled)";
        }
    }
    return text;
}

} // namespace Domain::ProcessSecurity
