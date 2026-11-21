#include "WindowsSecurityOwners.h"

#include "WindowsElevation.h"

// clang-format off
#include <aclapi.h> // GetSecurityInfo
// clang-format on

#include <array>
#include <cstddef>
#include <expected>

namespace Platform::Windows
{

namespace
{

/// A SID with six sub-authorities, laid out as SID is (winnt.h declares SubAuthority[ANYSIZE_ARRAY], room for one):
/// Revision, SubAuthorityCount, IdentifierAuthority, then the sub-authorities, each a DWORD at its natural alignment.
struct SixSubAuthoritySid
{
    BYTE revision;
    BYTE subAuthorityCount;
    SID_IDENTIFIER_AUTHORITY identifierAuthority;
    std::array<DWORD, 6> subAuthority;
};
static_assert(sizeof(SixSubAuthoritySid) == SECURITY_SID_SIZE(6), "a six-sub-authority SID, with no padding");
static_assert(alignof(SixSubAuthoritySid) >= alignof(SID), "aligned as a SID is");
static_assert(offsetof(SixSubAuthoritySid, revision) == offsetof(SID, Revision));
static_assert(offsetof(SixSubAuthoritySid, subAuthorityCount) == offsetof(SID, SubAuthorityCount));
static_assert(offsetof(SixSubAuthoritySid, identifierAuthority) == offsetof(SID, IdentifierAuthority));
static_assert(offsetof(SixSubAuthoritySid, subAuthority) == offsetof(SID, SubAuthority));

/// NT SERVICE\TrustedInstaller: S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464 (#4699). Constant
/// data, built at compile time: nothing converts it, allocates it or frees it.
constexpr SixSubAuthoritySid TRUSTED_INSTALLER{
    .revision = SID_REVISION,
    .subAuthorityCount = 6,
    .identifierAuthority = {SECURITY_NT_AUTHORITY},
    .subAuthority = {SECURITY_SERVICE_ID_BASE_RID, 956008885, 3418522649, 1831038044, 1853292631, 2271478464},
};

} // namespace

std::expected<HandleOwner, DWORD> ownerSidOf(HANDLE handle)
{
    PSID owner = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const DWORD read = GetSecurityInfo(handle, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION, &owner, nullptr, nullptr, nullptr, &descriptor);
    // Owned at once, so it is freed on every path, whatever GetSecurityInfo answered.
    HandleOwner result{.descriptor = {descriptor, &::LocalFree}, .sid = owner};
    if (read != ERROR_SUCCESS)
    {
        return std::unexpected(read);
    }
    if (owner == nullptr)
    {
        return std::unexpected(static_cast<DWORD>(ERROR_INVALID_OWNER));
    }
    return result;
}

bool isAdministrators(PSID sid) noexcept
{
    return IsWellKnownSid(sid, WinBuiltinAdministratorsSid) != FALSE;
}

bool isAdministratorsOrSystem(PSID sid) noexcept
{
    return isAdministrators(sid) || IsWellKnownSid(sid, WinLocalSystemSid) != FALSE;
}

PSID trustedInstallerSid() noexcept
{
    // PSID is a non-const void*, but every call it is handed to (EqualSid, IsValidSid, the SDDL and ACL readers) only
    // reads it, so the constant is passed as it is, never copied.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast) - Win32's PSID isn't const; the SID is only ever read
    return const_cast<SixSubAuthoritySid*>(&TRUSTED_INSTALLER);
}

bool isTrustedInstaller(PSID sid) noexcept
{
    return sid != nullptr && IsValidSid(sid) != FALSE && EqualSid(sid, trustedInstallerSid()) != FALSE;
}

std::expected<bool, DWORD> isTokenUser(PSID sid)
{
    // The current-process token pseudo-handle, which always has TOKEN_QUERY: no OpenProcessToken access check that
    // could fail where the read itself wouldn't.
    auto user = readTokenUserSid(GetCurrentProcessToken(), &::GetTokenInformation);
    if (!user.has_value())
    {
        return std::unexpected(user.error());
    }
    // readTokenUserSid() returns the SID's own bytes, checked valid (IsValidSid) and copied (CopySid).
    return EqualSid(sid, static_cast<PSID>(user->data())) != FALSE;
}

} // namespace Platform::Windows
