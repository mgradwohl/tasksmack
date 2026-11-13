#include "WindowsSessionAccount.h"

#include "Platform/SessionAccount.h"
#include "WinString.h"
#include "WindowsElevation.h"
#include "WindowsHandles.h"

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

/// Whether @p a and @p b are equal ignoring case, as Windows compares account names.
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
    return CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

} // namespace

bool processIsElevated()
{
    return isProcessElevated();
}

std::optional<AccountName> readProcessUser()
{
    UniqueHandle token;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, token.put()) == FALSE)
    {
        spdlog::debug("Session account: OpenProcessToken failed (error {})", GetLastError());
        return std::nullopt;
    }
    DWORD size = 0;
    static_cast<void>(GetTokenInformation(token.get(), TokenUser, nullptr, 0, &size));
    std::vector<std::byte> buffer(size);
    if (size == 0 || GetTokenInformation(token.get(), TokenUser, buffer.data(), size, &size) == FALSE)
    {
        spdlog::debug("Session account: reading TokenUser failed (error {})", GetLastError());
        return std::nullopt;
    }
    // TokenUser's buffer is a TOKEN_USER.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    const auto* const user = reinterpret_cast<const TOKEN_USER*>(buffer.data());
    DWORD nameLength = 0;
    DWORD domainLength = 0;
    SID_NAME_USE use = SidTypeUnknown;
    static_cast<void>(LookupAccountSidW(nullptr, user->User.Sid, nullptr, &nameLength, nullptr, &domainLength, &use));
    if (nameLength == 0)
    {
        spdlog::debug("Session account: LookupAccountSidW failed (error {})", GetLastError());
        return std::nullopt;
    }
    std::wstring name(nameLength, L'\0');
    std::wstring domain(domainLength, L'\0');
    if (LookupAccountSidW(nullptr, user->User.Sid, name.data(), &nameLength, domain.data(), &domainLength, &use) == FALSE)
    {
        spdlog::debug("Session account: LookupAccountSidW failed (error {})", GetLastError());
        return std::nullopt;
    }
    // On success the lengths exclude the NUL.
    name.resize(nameLength);
    domain.resize(domainLength);
    return AccountName{.domain = std::move(domain), .user = std::move(name)};
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
    if (sameAccount(*process, *session))
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
