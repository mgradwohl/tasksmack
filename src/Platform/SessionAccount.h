#pragma once

// Whose TaskSmack this is (#3671). A different account can approve the UAC prompt for Restart as
// administrator (over the shoulder, or Administrator Protection): the elevated copy then runs as that
// administrator, not as the session's signed-in user, and its HKEY_CURRENT_USER and profile are the
// administrator's. TaskSmack never launches as another user, or acts as one, so in that copy the per-user
// startup it would change (Start TaskSmack when I sign in, the Startup tab's per-user entries) is left alone.
//
// Read once, at startup, on a worker (App::SessionAccountCheck): Platform::readSessionAccount() (Factory.h)
// compares the user of TaskSmack's own token with the session's signed-in user, as Windows reports them.
// Never from a command-line value, which anything running as the user could have passed. When either can't be
// read, the copy counts as the same account: nothing changes from what it did before.

#include <string>

namespace Platform
{

/// Whether TaskSmack runs as an account other than the session's signed-in user.
struct SessionAccount
{
    /// Elevated as another account than the session's signed-in user: the per-user startup TaskSmack would
    /// change is that account's, not the user's. False when it is the same account, or either can't be told.
    bool otherAccount = false;
    /// otherAccount: the account TaskSmack runs as, by its name ("admin"), for the labels that say whose
    /// entries those are. Empty otherwise.
    std::string account;

    friend bool operator==(const SessionAccount&, const SessionAccount&) = default;
};

} // namespace Platform
