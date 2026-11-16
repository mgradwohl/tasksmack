#pragma once

// Whether an elevated TaskSmack runs as another account than the session's signed-in user (#3671): the
// user of TaskSmack's own token (TokenUser) against the session's (WTSQuerySessionInformationW's
// WTSUserName and WTSDomainName for WTS_CURRENT_SESSION, as the signed-in sessions are read).
// Platform::readSessionAccount() (Factory.h) is this, with the real calls.
//
// The two accounts are compared by SID (#4077), as the elevated helper decides its session
// (BrokerHelper::sessionPolicy()), so a rename or an account whose WTS name differs from what its SID
// looks up as can't make the two checks disagree. WTS gives the session's user only by name, and the
// one documented call that gives its token, WTSQueryUserToken, needs SeTcbPrivilege (LocalSystem), which
// even an elevated TaskSmack doesn't hold; so the session's name is turned into a SID with
// LookupAccountNameW. When either SID can't be read, the names are compared instead, as before.
// The names are kept for the log and for the account the app shows.
//
// Blocking (a token read, account lookups that may ask a domain controller): on a worker, never the UI
// thread (App::SessionAccountCheck runs it once, at startup). The calls are a table of function pointers,
// so tests drive every answer with fakes (test_WindowsSessionAccount.cpp).

#ifdef _WIN32

#include "Platform/SessionAccount.h"
#include "Platform/Windows/WindowsElevation.h"

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// clang-format on

#include <cstddef>
#include <optional>
#include <vector>

namespace Platform::Windows
{

/// A SID as its bytes (readTokenUserSid()'s form, and BrokerPipe::Sid's).
using SidBytes = std::vector<std::byte>;

/// The user of TaskSmack's own token, by name (currentUserName()); nullopt when it can't be read.
[[nodiscard]] std::optional<AccountName> readProcessUser();
/// The user of TaskSmack's own token, by SID (currentUserSid()); nullopt when it can't be read.
[[nodiscard]] std::optional<SidBytes> readProcessUserSid();
/// The current session's signed-in user, as WTS reports it; nullopt when it can't be read.
[[nodiscard]] std::optional<AccountName> readSessionUser();

/// LookupAccountNameW's signature, so tests can fake it.
using LookupAccountNameFn = BOOL(WINAPI*)(LPCWSTR, LPCWSTR, PSID, LPDWORD, LPWSTR, LPDWORD, PSID_NAME_USE);

/// The SID of the user account @p account names ("domain\user", or the user alone when the domain is
/// empty), through @p lookupByName on the local computer, its buffers sized by the call itself.
/// nullopt when the name doesn't resolve, or resolves to something other than a user (a group, a
/// computer, a deleted account).
[[nodiscard]] std::optional<SidBytes> lookupUserSid(const AccountName& account, LookupAccountNameFn lookupByName = &::LookupAccountNameW);

/// lookupUserSid() with the real call, as SessionAccountFunctions takes it.
[[nodiscard]] std::optional<SidBytes> readAccountSid(const AccountName& account);

/// The calls readSessionAccount() makes: the real ones, or a test's fakes. AccountName is WindowsElevation.h's.
struct SessionAccountFunctions
{
    using ElevatedFn = bool (*)();
    using AccountFn = std::optional<AccountName> (*)();
    using SidFn = std::optional<SidBytes> (*)();
    using AccountSidFn = std::optional<SidBytes> (*)(const AccountName&);

    ElevatedFn isElevated = &isProcessElevated;
    AccountFn processUser = &readProcessUser;
    AccountFn sessionUser = &readSessionUser;
    SidFn processUserSid = &readProcessUserSid;
    AccountSidFn accountSid = &readAccountSid;
};

/// Whether @p a and @p b are the same account: the same domain and user name, ignoring case as Windows does.
/// readSessionAccount()'s fallback when a SID can't be read.
[[nodiscard]] bool sameAccount(const AccountName& a, const AccountName& b);

/// Whether TaskSmack runs elevated as another account than the session's signed-in user: by SID when both
/// can be read, by name (sameAccount()) otherwise. Not elevated, the same account, or either user unreadable
/// (or empty, as in a session no one signed in to): the same account, so nothing changes. Asks for nothing
/// more once it knows the copy isn't elevated.
[[nodiscard]] SessionAccount readSessionAccount(const SessionAccountFunctions& fns = {});

} // namespace Platform::Windows

#endif // _WIN32
