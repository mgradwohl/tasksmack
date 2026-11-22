#include "WindowsSessionAccount.h"

#include "OrdinalCase.h"
#include "Platform/SessionAccount.h"
#include "WinString.h"
#include "WindowsElevation.h"

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wtsapi32.h>
// clang-format on

#include <spdlog/spdlog.h>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Platform::Windows
{

namespace
{

/// What WTS allocated, given back with WTSFreeMemory.
struct WtsFree
{
    void operator()(wchar_t* memory) const noexcept
    {
        WTSFreeMemory(memory);
    }
};

/// The current session's @p infoClass as text; nullopt when WTS gives none.
[[nodiscard]] std::optional<std::wstring> querySessionText(WTS_INFO_CLASS infoClass)
{
    LPWSTR buffer = nullptr;
    DWORD bytes = 0;
    if (WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, WTS_CURRENT_SESSION, infoClass, &buffer, &bytes) == FALSE ||
        buffer == nullptr)
    {
        spdlog::debug("Session account: WTSQuerySessionInformationW({}) failed (error {})", static_cast<int>(infoClass), GetLastError());
        return std::nullopt;
    }
    const std::unique_ptr<wchar_t, WtsFree> owned(buffer);
    // Text up to its NUL, within what WTS says it gave.
    const std::size_t limit = bytes / sizeof(wchar_t);
    std::size_t length = 0;
    while (length < limit && owned.get()[length] != L'\0') // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic) - bounded by limit
    {
        ++length;
    }
    return std::wstring(owned.get(), length);
}

/// Whether @p a and @p b are equal ignoring case, as Windows compares account names: two of different lengths never
/// are, and two empty ones always are, before Windows is asked (equalsOrdinalIgnoreCase(), OrdinalCase.h).
[[nodiscard]] bool equalIgnoringCase(const std::wstring& a, const std::wstring& b)
{
    if (a.size() != b.size())
    {
        return false;
    }
    if (a.empty())
    {
        return true;
    }
    return equalsOrdinalIgnoreCase(std::wstring_view(a), std::wstring_view(b));
}

/// Whether the token's user is @p session's, by SID (#4077); nullopt when either SID can't be read.
[[nodiscard]] std::optional<bool> sameSid(const SessionAccountFunctions& fns, const AccountName& session)
{
    const std::optional<SidBytes> process = fns.processUserSid();
    if (!process.has_value())
    {
        spdlog::info("Session account: couldn't read this TaskSmack's user SID; comparing account names instead");
        return std::nullopt;
    }
    const std::optional<SidBytes> signedIn = fns.accountSid(session);
    if (!signedIn.has_value())
    {
        spdlog::info("Session account: couldn't find the signed-in user's SID; comparing account names instead");
        return std::nullopt;
    }
    return *process == *signedIn;
}

} // namespace

std::optional<AccountName> readProcessUser()
{
    return currentUserName();
}

std::optional<SidBytes> readProcessUserSid()
{
    auto sid = currentUserSid();
    if (!sid.has_value())
    {
        spdlog::debug("Session account: reading the token's user SID failed (error {})", sid.error());
        return std::nullopt;
    }
    return std::move(*sid);
}

std::optional<AccountName> readSessionUser()
{
    std::optional<std::wstring> user = querySessionText(WTSUserName);
    if (!user.has_value())
    {
        return std::nullopt;
    }
    std::optional<std::wstring> domain = querySessionText(WTSDomainName);
    if (!domain.has_value())
    {
        return std::nullopt;
    }
    return AccountName{.domain = std::move(*domain), .user = std::move(*user)};
}

std::optional<SidBytes> lookupUserSid(const AccountName& account, LookupAccountNameFn lookupByName)
{
    // Qualified, so a local account and a domain account of the same user name can't be confused.
    const std::wstring name = account.domain.empty() ? account.user : account.domain + L'\\' + account.user;
    // A first call with no buffers asks how big the SID and the domain are; another
    // ERROR_INSUFFICIENT_BUFFER sizes them again, a few times at most, as lookupAccountName() does.
    DWORD sidSize = 0;
    DWORD domainLength = 0;
    SID_NAME_USE use = SidTypeUnknown;
    static_cast<void>(lookupByName(nullptr, name.c_str(), nullptr, &sidSize, nullptr, &domainLength, &use));
    constexpr int ATTEMPTS = 3;
    for (int attempt = 0; attempt < ATTEMPTS && sidSize != 0; ++attempt)
    {
        SidBytes sid(sidSize);
        std::wstring domain(domainLength, L'\0');
        if (lookupByName(nullptr, name.c_str(), sid.data(), &sidSize, domain.data(), &domainLength, &use) != FALSE)
        {
            // A WTS session's user is a user account; anything else (a group of the same name) isn't it.
            if (use != SidTypeUser || IsValidSid(sid.data()) == FALSE || GetLengthSid(sid.data()) > sid.size())
            {
                spdlog::debug("Session account: the signed-in user's name resolves to no user SID (type {})", static_cast<int>(use));
                return std::nullopt;
            }
            sid.resize(GetLengthSid(sid.data()));
            return sid;
        }
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER)
        {
            break;
        }
    }
    spdlog::debug("Session account: LookupAccountNameW failed (error {})", GetLastError());
    return std::nullopt;
}

std::optional<SidBytes> readAccountSid(const AccountName& account)
{
    return lookupUserSid(account);
}

bool sameAccount(const AccountName& a, const AccountName& b)
{
    return equalIgnoringCase(a.user, b.user) && equalIgnoringCase(a.domain, b.domain);
}

SessionAccount readSessionAccount(const SessionAccountFunctions& fns)
{
    if (!fns.isElevated())
    {
        return {}; // a Standard TaskSmack is always the session's user
    }
    const std::optional<AccountName> process = fns.processUser();
    if (!process.has_value() || process->user.empty())
    {
        spdlog::info("Session account: couldn't tell which account this TaskSmack runs as; treating it as the signed-in user");
        return {};
    }
    const std::optional<AccountName> session = fns.sessionUser();
    if (!session.has_value() || session->user.empty())
    {
        spdlog::info("Session account: couldn't read the session's signed-in user; treating this TaskSmack as that user");
        return {};
    }
    // By SID, as the helper decides its session (BrokerHelper::sessionPolicy()); by name only when a SID
    // can't be read, which is how this was decided before #4077. The names stay for the log and the app.
    const std::optional<bool> sameBySid = sameSid(fns, *session);
    if (sameBySid.has_value() ? *sameBySid : sameAccount(*process, *session))
    {
        return {};
    }
    std::string account = WinString::wideToUtf8(process->user);
    spdlog::info("Session account: this TaskSmack runs elevated as {}\\{}, not as the signed-in user {}\\{}: it leaves the user's sign-in "
                 "startup and per-user startup entries alone",
                 WinString::wideToUtf8(process->domain),
                 account,
                 WinString::wideToUtf8(session->domain),
                 WinString::wideToUtf8(session->user));
    return {.otherAccount = true, .account = std::move(account)};
}

} // namespace Platform::Windows
