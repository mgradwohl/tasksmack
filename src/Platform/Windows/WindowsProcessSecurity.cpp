#include "WindowsProcessSecurity.h"

#include "Platform/IProcessActions.h"
#include "Platform/IProcessSecurity.h"
#include "WinString.h"
#include "WindowsHandles.h"
#include "WindowsProcessIdentity.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <format>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

namespace Platform::Windows
{

namespace
{

/// The most account names kept: past it the cache starts over, so browsing many processes cannot grow
/// it without bound. A desktop's distinct token SIDs number in the dozens.
constexpr std::size_t MAX_CACHED_ACCOUNTS = 1024;

/// Tries at a variable-size token query before giving up: a token's privileges and groups can change
/// between the size query and the copy.
constexpr int MAX_TOKEN_QUERY_ATTEMPTS = 3;

/// The largest token query answer taken; a real token's groups fit in a few kilobytes.
constexpr DWORD MAX_TOKEN_INFORMATION_BYTES = 1U << 20U;

/// The first buffer for an account or domain name, in characters (names are at most 256 long).
constexpr DWORD INITIAL_NAME_CHARS = 257;

/// The first buffer for a privilege name ("SeDelegateSessionUserImpersonatePrivilege" is the longest).
constexpr DWORD INITIAL_PRIVILEGE_CHARS = 64;

[[nodiscard]] SecurityReadResult failure(SecurityReadStatus status, std::string detail = {})
{
    return {.status = status, .security = {}, .detail = std::move(detail)};
}

/// The system's message for a Win32 error code.
[[nodiscard]] std::string errorText(DWORD error)
{
    return std::system_category().message(static_cast<int>(error));
}

/// A failed token query's result: PermissionDenied for ERROR_ACCESS_DENIED, else Failed with the error.
[[nodiscard]] SecurityReadResult tokenFailure(DWORD error)
{
    return error == ERROR_ACCESS_DENIED ? failure(SecurityReadStatus::PermissionDenied)
                                        : failure(SecurityReadStatus::Failed, errorText(error));
}

/// Whether a failed GetTokenInformation only wanted a bigger buffer.
[[nodiscard]] bool wantsBiggerBuffer(DWORD error) noexcept
{
    return error == ERROR_INSUFFICIENT_BUFFER || error == ERROR_BAD_LENGTH;
}

/// GetTokenInformation for a variable-size class (TokenUser, TokenGroups, ...): sized by a first call,
/// and re-sized if the answer grew before the copy. The error when it could not be read.
[[nodiscard]] std::expected<std::vector<std::byte>, DWORD>
queryToken(const ProcessSecurityFunctions& api, HANDLE token, TOKEN_INFORMATION_CLASS infoClass)
{
    DWORD needed = 0;
    if (api.getTokenInformation(token, infoClass, nullptr, 0, &needed) == 0)
    {
        const DWORD error = GetLastError();
        if (!wantsBiggerBuffer(error))
        {
            return std::unexpected(error);
        }
    }
    std::vector<std::byte> buffer;
    int attempt = 1;
    while (attempt <= MAX_TOKEN_QUERY_ATTEMPTS)
    {
        if (needed == 0 || needed > MAX_TOKEN_INFORMATION_BYTES)
        {
            return std::unexpected(static_cast<DWORD>(ERROR_INVALID_DATA));
        }
        buffer.assign(needed, std::byte{0});
        DWORD written = 0;
        if (api.getTokenInformation(token, infoClass, buffer.data(), needed, &written) != 0)
        {
            if (written > 0 && written < needed)
            {
                buffer.resize(written);
            }
            return buffer;
        }
        const DWORD error = GetLastError();
        if (!wantsBiggerBuffer(error))
        {
            return std::unexpected(error);
        }
        needed = written; // the size it has grown to
        ++attempt;
    }
    return std::unexpected(static_cast<DWORD>(ERROR_INSUFFICIENT_BUFFER));
}

/// GetTokenInformation for a fixed-size class (TokenElevation, TokenSessionId, ...); empty when it fails.
template<typename T>
[[nodiscard]] std::optional<T> queryTokenValue(const ProcessSecurityFunctions& api, HANDLE token, TOKEN_INFORMATION_CLASS infoClass)
{
    static_assert(std::is_trivially_copyable_v<T>);
    T value{};
    DWORD written = 0;
    if (api.getTokenInformation(token, infoClass, &value, static_cast<DWORD>(sizeof(T)), &written) == 0 || written < sizeof(T))
    {
        return std::nullopt;
    }
    return value;
}

/// The @p T at byte @p offset of @p buffer, copied out (the buffer has no T objects to alias); empty when
/// the buffer is too short to hold it.
template<typename T> [[nodiscard]] std::optional<T> readAt(const std::vector<std::byte>& buffer, std::size_t offset)
{
    static_assert(std::is_trivially_copyable_v<T>);
    if (offset > buffer.size() || buffer.size() - offset < sizeof(T))
    {
        return std::nullopt;
    }
    T value{};
    std::memcpy(&value, buffer.data() + offset, sizeof(T));
    return value;
}

/// The RID of a mandatory label SID (S-1-16-<rid>): its last sub-authority.
[[nodiscard]] std::optional<std::uint32_t> integrityRid(PSID label)
{
    if (label == nullptr || IsValidSid(label) == FALSE)
    {
        return std::nullopt;
    }
    const UCHAR count = *GetSidSubAuthorityCount(label);
    if (count == 0)
    {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(*GetSidSubAuthority(label, static_cast<DWORD>(count - 1U)));
}

/// A TOKEN_ELEVATION_TYPE (read as its DWORD) as the platform-neutral enum; empty for a value Windows
/// does not define.
[[nodiscard]] std::optional<TokenElevationKind> elevationType(DWORD type) noexcept
{
    switch (type)
    {
    case TokenElevationTypeDefault:
        return TokenElevationKind::Default;
    case TokenElevationTypeFull:
        return TokenElevationKind::Full;
    case TokenElevationTypeLimited:
        return TokenElevationKind::Limited;
    default:
        break;
    }
    return std::nullopt;
}

/// "DOMAIN\name" for @p sid (just "name" for one with no domain, such as Everyone); empty when it does
/// not resolve (a deleted account, a logon SID on an older Windows, a domain that cannot be reached).
[[nodiscard]] std::string lookupAccountName(const ProcessSecurityFunctions& api, PSID sid)
{
    DWORD nameChars = INITIAL_NAME_CHARS;
    DWORD domainChars = INITIAL_NAME_CHARS;
    std::wstring name(nameChars, L'\0');
    std::wstring domain(domainChars, L'\0');
    SID_NAME_USE use = SidTypeUnknown;
    if (api.lookupAccountSid(nullptr, sid, name.data(), &nameChars, domain.data(), &domainChars, &use) == 0)
    {
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || nameChars == 0)
        {
            return {};
        }
        // The counts now say what is needed, terminators included.
        name.assign(nameChars, L'\0');
        domain.assign(std::max<DWORD>(domainChars, 1), L'\0');
        domainChars = static_cast<DWORD>(domain.size());
        if (api.lookupAccountSid(nullptr, sid, name.data(), &nameChars, domain.data(), &domainChars, &use) == 0)
        {
            return {};
        }
    }
    // On success the counts are the lengths written, terminators excluded.
    name.resize(std::min<std::size_t>(nameChars, name.size()));
    domain.resize(std::min<std::size_t>(domainChars, domain.size()));
    if (name.empty())
    {
        return {};
    }
    return WinString::wideToUtf8(domain.empty() ? name : domain + L"\\" + name);
}

/// The name of privilege @p luid ("SeDebugPrivilege"), or "LUID 0x<high>:<low>" when it has none.
[[nodiscard]] std::string privilegeName(const ProcessSecurityFunctions& api, LUID luid)
{
    DWORD chars = INITIAL_PRIVILEGE_CHARS;
    std::wstring name(chars, L'\0');
    bool named = api.lookupPrivilegeName(nullptr, &luid, name.data(), &chars) != 0;
    if (!named && GetLastError() == ERROR_INSUFFICIENT_BUFFER && chars > 0)
    {
        name.assign(chars, L'\0'); // the count now says what is needed, terminator included
        named = api.lookupPrivilegeName(nullptr, &luid, name.data(), &chars) != 0;
    }
    if (!named)
    {
        return std::format("LUID 0x{:X}:{:08X}", static_cast<std::uint32_t>(luid.HighPart), luid.LowPart);
    }
    name.resize(std::min<std::size_t>(chars, name.size()));
    return WinString::wideToUtf8(name);
}

/// TokenPrivileges' answer as privileges with their state; empty when its count overruns the buffer.
[[nodiscard]] std::optional<std::vector<TokenPrivilege>> parsePrivileges(const ProcessSecurityFunctions& api,
                                                                         const std::vector<std::byte>& buffer)
{
    const auto count = readAt<DWORD>(buffer, offsetof(TOKEN_PRIVILEGES, PrivilegeCount));
    if (!count.has_value())
    {
        return std::nullopt;
    }
    std::vector<TokenPrivilege> privileges;
    privileges.reserve(std::min<std::size_t>(*count, buffer.size() / sizeof(LUID_AND_ATTRIBUTES)));
    DWORD index = 0;
    while (index < *count)
    {
        const auto entry = readAt<LUID_AND_ATTRIBUTES>(
            buffer, offsetof(TOKEN_PRIVILEGES, Privileges) + (std::size_t{index} * sizeof(LUID_AND_ATTRIBUTES)));
        if (!entry.has_value())
        {
            return std::nullopt;
        }
        privileges.push_back({.name = privilegeName(api, entry->Luid), .enabled = (entry->Attributes & SE_PRIVILEGE_ENABLED) != 0});
        ++index;
    }
    return privileges;
}

} // namespace

std::string sidToString(PSID sid)
{
    if (sid == nullptr || IsValidSid(sid) == FALSE)
    {
        return {};
    }
    const SID_IDENTIFIER_AUTHORITY* authority = GetSidIdentifierAuthority(sid);
    std::uint64_t value = 0;
    for (const BYTE part : authority->Value)
    {
        value = (value << 8U) | part;
    }
    // ConvertSidToStringSidW's form: the authority in decimal while it fits 32 bits, else in hex.
    std::string text = (value >> 32U) == 0 ? std::format("S-1-{}", value) : std::format("S-1-0x{:X}", value);
    const UCHAR count = *GetSidSubAuthorityCount(sid);
    DWORD index = 0;
    while (index < count)
    {
        text += std::format("-{}", static_cast<std::uint32_t>(*GetSidSubAuthority(sid, index)));
        ++index;
    }
    return text;
}

bool WindowsProcessSecurityReader::hasSecurity() const
{
    return true;
}

SecurityAccount WindowsProcessSecurityReader::account(PSID sid)
{
    SecurityAccount result{.sid = sidToString(sid), .name = {}};
    if (result.sid.empty())
    {
        return result;
    }
    if (const auto cached = m_AccountNames.find(result.sid); cached != m_AccountNames.end())
    {
        result.name = cached->second;
        return result;
    }
    result.name = lookupAccountName(m_Api, sid);
    if (m_AccountNames.size() >= MAX_CACHED_ACCOUNTS)
    {
        m_AccountNames.clear();
    }
    m_AccountNames.emplace(result.sid, result.name);
    return result;
}

SecurityReadResult WindowsProcessSecurityReader::readSecurity(const ProcessTarget& target)
{
    const std::scoped_lock lock(m_Mutex); // one read at a time: the name cache is shared
    // Closed with the real CloseHandle: this seam's fakes hand out real handles.
    auto opened = openProcessVerified({.openProcess = m_Api.openProcess,
                                       .getProcessTimes = m_Api.getProcessTimes,
                                       .closeHandle = &::CloseHandle,
                                       .getExitCodeProcess = m_Api.getExitCodeProcess},
                                      target,
                                      PROCESS_QUERY_LIMITED_INFORMATION);
    if (!opened)
    {
        auto refused = toReadFailure<SecurityReadStatus>(opened.error());
        return failure(refused.status, std::move(refused.detail));
    }
    const InjectableHandle process = std::move(*opened);

    // A protected process (PPL, and System's) opens for PROCESS_QUERY_LIMITED_INFORMATION but refuses
    // its token: PermissionDenied, as on Linux.
    UniqueHandle token;
    if (m_Api.openProcessToken(process.get(), TOKEN_QUERY, token.put()) == 0)
    {
        return tokenFailure(GetLastError());
    }

    // The user is the one query a read needs; every other field is left empty when its query fails,
    // rather than failing the whole read.
    ProcessSecurity security;
    const auto user = queryToken(m_Api, token.get(), TokenUser);
    if (!user.has_value())
    {
        return tokenFailure(user.error());
    }
    const auto tokenUser = readAt<TOKEN_USER>(*user, 0);
    if (!tokenUser.has_value())
    {
        return failure(SecurityReadStatus::Failed, errorText(ERROR_INVALID_DATA));
    }
    security.tokenUser = account(tokenUser->User.Sid); // the SID points into *user, alive here

    if (const auto session = queryTokenValue<DWORD>(m_Api, token.get(), TokenSessionId); session.has_value())
    {
        security.sessionId = static_cast<std::uint32_t>(*session);
    }
    if (const auto label = queryToken(m_Api, token.get(), TokenIntegrityLevel); label.has_value())
    {
        if (const auto mandatory = readAt<TOKEN_MANDATORY_LABEL>(*label, 0); mandatory.has_value())
        {
            security.integrityLevel = integrityRid(mandatory->Label.Sid);
        }
    }
    if (const auto elevation = queryTokenValue<TOKEN_ELEVATION>(m_Api, token.get(), TokenElevation); elevation.has_value())
    {
        security.elevated = elevation->TokenIsElevated != 0;
    }
    if (const auto type = queryTokenValue<DWORD>(m_Api, token.get(), TokenElevationType); type.has_value())
    {
        security.elevationType = elevationType(*type);
    }
    if (const auto appContainer = queryTokenValue<DWORD>(m_Api, token.get(), TokenIsAppContainer); appContainer.has_value())
    {
        security.appContainer = *appContainer != 0;
    }
    if (const auto virtualization = queryTokenValue<DWORD>(m_Api, token.get(), TokenVirtualizationEnabled); virtualization.has_value())
    {
        security.virtualizationEnabled = *virtualization != 0;
    }
    if (const auto privileges = queryToken(m_Api, token.get(), TokenPrivileges); privileges.has_value())
    {
        security.privileges = parsePrivileges(m_Api, *privileges);
    }
    if (const auto groups = queryToken(m_Api, token.get(), TokenGroups); groups.has_value())
    {
        const auto count = readAt<DWORD>(*groups, offsetof(TOKEN_GROUPS, GroupCount));
        std::vector<TokenGroup> list;
        bool complete = count.has_value();
        DWORD index = 0;
        while (complete && index < *count)
        {
            const auto entry =
                readAt<SID_AND_ATTRIBUTES>(*groups, offsetof(TOKEN_GROUPS, Groups) + (std::size_t{index} * sizeof(SID_AND_ATTRIBUTES)));
            complete = entry.has_value();
            // The integrity label is listed as a group too; it has its own row.
            if (complete && (entry->Attributes & SE_GROUP_INTEGRITY) == 0)
            {
                list.push_back({.account = account(entry->Sid), // points into *groups, alive here
                                .enabled = (entry->Attributes & SE_GROUP_ENABLED) != 0,
                                .denyOnly = (entry->Attributes & SE_GROUP_USE_FOR_DENY_ONLY) != 0});
            }
            ++index;
        }
        if (complete)
        {
            security.tokenGroups = std::move(list);
        }
    }
    return {.status = SecurityReadStatus::Ok, .security = std::move(security), .detail = {}};
}

} // namespace Platform::Windows
