#pragma once

// Whether an elevated TaskSmack runs as another account than the session's signed-in user (#3671): the
// user of TaskSmack's own token (TokenUser, named by LookupAccountSidW) against the session's
// (WTSQuerySessionInformationW's WTSUserName and WTSDomainName for WTS_CURRENT_SESSION, as the
// signed-in sessions are read). Platform::readSessionAccount() (Factory.h) is this, with the real calls.
//
// Blocking (a token read, an account lookup that may ask a domain controller): on a worker, never the UI
// thread (App::SessionAccountCheck runs it once, at startup). The calls are a table of function pointers,
// so tests drive every answer with fakes (test_WindowsSessionAccount.cpp).

#ifdef _WIN32

#include "Platform/SessionAccount.h"

#include <optional>
#include <string>

namespace Platform::Windows
{

/// An account as Windows names it: its domain (the computer's name for a local account, "AzureAD" for an
/// Entra ID one) and its user name.
struct AccountName
{
    std::wstring domain;
    std::wstring user;
};

/// Whether TaskSmack's own token is elevated (isProcessElevated()).
[[nodiscard]] bool processIsElevated();
/// The user of TaskSmack's own token, by name; nullopt when it can't be read.
[[nodiscard]] std::optional<AccountName> readProcessUser();
/// The current session's signed-in user, as WTS reports it; nullopt when it can't be read.
[[nodiscard]] std::optional<AccountName> readSessionUser();

/// The calls readSessionAccount() makes: the real ones, or a test's fakes.
struct SessionAccountFunctions
{
    using ElevatedFn = bool (*)();
    using AccountFn = std::optional<AccountName> (*)();

    ElevatedFn isElevated = &processIsElevated;
    AccountFn processUser = &readProcessUser;
    AccountFn sessionUser = &readSessionUser;
};

/// Whether @p a and @p b are the same account: the same domain and user name, ignoring case as Windows does.
[[nodiscard]] bool sameAccount(const AccountName& a, const AccountName& b);

/// Whether TaskSmack runs elevated as another account than the session's signed-in user. Not elevated, the
/// same account, or either user unreadable (or empty, as in a session no one signed in to): the same
/// account, so nothing changes. Asks for nothing more once it knows the copy isn't elevated.
[[nodiscard]] SessionAccount readSessionAccount(const SessionAccountFunctions& fns = {});

} // namespace Platform::Windows

#endif // _WIN32
