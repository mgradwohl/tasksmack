#include "WindowsSecurityOwners.h"

#include "WindowsElevation.h"

// clang-format off
#include <aclapi.h> // GetSecurityInfo
#include <sddl.h>   // ConvertStringSidToSidW
// clang-format on

#include <expected>
#include <memory>
#include <optional>

namespace Platform::Windows
{

namespace
{

/// A SID ConvertStringSidToSidW allocated, LocalFree'd when it dies.
using ConvertedSid = std::unique_ptr<void, decltype(&::LocalFree)>;

/// TrustedInstaller's SID, converted from TRUSTED_INSTALLER_SID now; null when the conversion fails.
[[nodiscard]] ConvertedSid convertTrustedInstallerSid()
{
    PSID made = nullptr;
    // TRUSTED_INSTALLER_SID views a whole string literal, so its data() is NUL-terminated: no copy (and no allocation
    // the old per-call conversions didn't make) is needed.
    // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage) - a view of a whole literal, NUL-terminated
    if (ConvertStringSidToSidW(TRUSTED_INSTALLER_SID.data(), &made) == FALSE)
    {
        made = nullptr; // LCOV_EXCL_LINE - a constant, well-formed SID: only an allocation failure gets here
    }
    return {made, &::LocalFree};
}

/// Whether @p sid is @p trustedInstaller; nullopt when that SID couldn't be converted.
[[nodiscard]] std::optional<bool> equalsTrustedInstaller(PSID sid, const ConvertedSid& trustedInstaller)
{
    if (trustedInstaller == nullptr)
    {
        return std::nullopt; // LCOV_EXCL_LINE - see convertTrustedInstallerSid()
    }
    return EqualSid(sid, trustedInstaller.get()) != FALSE;
}

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

std::optional<bool> isTrustedInstaller(PSID sid)
{
    const ConvertedSid trustedInstaller = convertTrustedInstallerSid();
    return equalsTrustedInstaller(sid, trustedInstaller);
}

std::optional<bool> isTrustedInstallerConvertedOnce(PSID sid)
{
    static const ConvertedSid trustedInstaller = convertTrustedInstallerSid();
    return equalsTrustedInstaller(sid, trustedInstaller);
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
