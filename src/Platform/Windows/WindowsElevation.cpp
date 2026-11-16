#include "WindowsElevation.h"

#include "WindowsHandles.h"

// clang-format off
#include <sddl.h>
// clang-format on

#include <spdlog/spdlog.h>

#include <cstddef>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Platform::Windows
{

namespace
{

/// What ConvertSidToStringSidW allocated, given back with LocalFree.
struct LocalFreeDeleter
{
    void operator()(wchar_t* text) const noexcept
    {
        LocalFree(text);
    }
};

/// The current process token, opened with TOKEN_QUERY through @p fns; the Win32 error when it can't be.
[[nodiscard]] std::expected<UniqueHandle, DWORD> openCurrentToken(const CurrentTokenFunctions& fns)
{
    UniqueHandle token;
    if (fns.openProcessToken(GetCurrentProcess(), TOKEN_QUERY, token.put()) == FALSE)
    {
        return std::unexpected(GetLastError());
    }
    return token;
}

/// GetLastError(), or ERROR_GEN_FAILURE when a call failed without setting one.
[[nodiscard]] DWORD lastErrorOrFailure() noexcept
{
    const DWORD error = GetLastError();
    return (error != ERROR_SUCCESS) ? error : static_cast<DWORD>(ERROR_GEN_FAILURE);
}

} // namespace

std::optional<bool> readTokenElevation(HANDLE token, CurrentTokenFunctions::GetTokenInformationFn getTokenInformation)
{
    TOKEN_ELEVATION elevation{};
    DWORD size = sizeof(elevation);
    if (getTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size) == FALSE)
    {
        return std::nullopt;
    }
    return elevation.TokenIsElevated != 0;
}

std::expected<std::vector<std::byte>, DWORD> readTokenUserSid(HANDLE token,
                                                              CurrentTokenFunctions::GetTokenInformationFn getTokenInformation)
{
    DWORD size = 0;
    static_cast<void>(getTokenInformation(token, TokenUser, nullptr, 0, &size));
    if (size < sizeof(TOKEN_USER))
    {
        return std::unexpected(lastErrorOrFailure());
    }
    std::vector<std::byte> buffer(size);
    if (getTokenInformation(token, TokenUser, buffer.data(), size, &size) == FALSE)
    {
        return std::unexpected(lastErrorOrFailure());
    }
    const auto* const user = reinterpret_cast<const TOKEN_USER*>(
        buffer.data()); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast) - TokenUser's buffer is a TOKEN_USER
    if (IsValidSid(user->User.Sid) == FALSE)
    {
        return std::unexpected(static_cast<DWORD>(ERROR_INVALID_SID));
    }
    std::vector<std::byte> sid(GetLengthSid(user->User.Sid));
    if (CopySid(static_cast<DWORD>(sid.size()), sid.data(), user->User.Sid) == FALSE)
    {
        return std::unexpected(lastErrorOrFailure());
    }
    return sid;
}

std::optional<AccountName> lookupAccountName(PSID sid, CurrentTokenFunctions::LookupAccountSidFn lookupAccountSid)
{
    // A first call with no buffers asks how long the names are (each length with its NUL); a name can be
    // longer than any fixed buffer (#3864). The account can be renamed between the calls, so a second
    // ERROR_INSUFFICIENT_BUFFER sizes the buffers again, a few times at most.
    DWORD nameLength = 0;
    DWORD domainLength = 0;
    SID_NAME_USE use = SidTypeUnknown;
    static_cast<void>(lookupAccountSid(nullptr, sid, nullptr, &nameLength, nullptr, &domainLength, &use));
    constexpr int ATTEMPTS = 3;
    for (int attempt = 0; attempt < ATTEMPTS && nameLength != 0; ++attempt)
    {
        std::wstring name(nameLength, L'\0');
        std::wstring domain(domainLength, L'\0');
        if (lookupAccountSid(nullptr, sid, name.data(), &nameLength, domain.data(), &domainLength, &use) != FALSE)
        {
            // On success the lengths exclude the NUL.
            name.resize(nameLength);
            domain.resize(domainLength);
            return AccountName{.domain = std::move(domain), .user = std::move(name)};
        }
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER)
        {
            break;
        }
    }
    spdlog::debug("lookupAccountName: LookupAccountSidW failed (error {})", GetLastError());
    return std::nullopt;
}

bool readProcessElevated(const CurrentTokenFunctions& fns)
{
    const auto token = openCurrentToken(fns);
    if (!token.has_value())
    {
        spdlog::debug("isProcessElevated: OpenProcessToken failed (error {}); reporting not elevated", token.error());
        return false;
    }
    const std::optional<bool> elevated = readTokenElevation(token->get(), fns.getTokenInformation);
    if (!elevated.has_value())
    {
        spdlog::debug("isProcessElevated: reading TokenElevation failed (error {}); reporting not elevated", GetLastError());
        return false;
    }
    return *elevated;
}

bool isProcessElevated()
{
    static const bool elevated = readProcessElevated(CurrentTokenFunctions{});
    return elevated;
}

TOKEN_ELEVATION_TYPE currentElevationType(const CurrentTokenFunctions& fns) noexcept
{
    UniqueHandle token;
    if (fns.openProcessToken(GetCurrentProcess(), TOKEN_QUERY, token.put()) == FALSE)
    {
        return TokenElevationTypeDefault;
    }
    TOKEN_ELEVATION_TYPE type = TokenElevationTypeDefault;
    DWORD size = 0;
    if (fns.getTokenInformation(token.get(), TokenElevationType, &type, sizeof(type), &size) == FALSE)
    {
        return TokenElevationTypeDefault;
    }
    return type;
}

std::expected<std::vector<std::byte>, DWORD> currentUserSid(const CurrentTokenFunctions& fns)
{
    auto token = openCurrentToken(fns);
    if (!token.has_value())
    {
        return std::unexpected(token.error());
    }
    return readTokenUserSid(token->get(), fns.getTokenInformation);
}

std::expected<std::wstring, DWORD> currentUserSidText(const CurrentTokenFunctions& fns)
{
    auto sid = currentUserSid(fns);
    if (!sid.has_value())
    {
        return std::unexpected(sid.error());
    }
    LPWSTR text = nullptr;
    if (ConvertSidToStringSidW(static_cast<PSID>(sid->data()), &text) == FALSE)
    {
        return std::unexpected(lastErrorOrFailure());
    }
    const std::unique_ptr<wchar_t, LocalFreeDeleter> owned(text);
    return std::wstring(owned.get());
}

std::optional<AccountName> currentUserName(const CurrentTokenFunctions& fns)
{
    auto sid = currentUserSid(fns);
    if (!sid.has_value())
    {
        spdlog::debug("currentUserName: reading the token's user failed (error {})", sid.error());
        return std::nullopt;
    }
    return lookupAccountName(static_cast<PSID>(sid->data()), fns.lookupAccountSid);
}

} // namespace Platform::Windows
