#pragma once

// A process owner's account as the Processes table and Process Details write it (#3328): "PC\alice" on
// Windows, where the probe reads the owner's domain with its user name (ProcessSnapshot::userDomain,
// #3310), and the user name alone on Linux, which has no domain. Pure and allocation-free except for
// qualified(), so the Processes filter can run matches() on every row.

#include "Platform/Utf8CaseFold.h"

#include <cstddef>
#include <string>
#include <string_view>

namespace Domain::AccountName
{

/// The separator Windows writes between an account's domain and its user name.
inline constexpr char DOMAIN_SEPARATOR = '\\';

/// "PC\\alice" for an owner whose domain is known, else @p user as it is (Linux, or a Windows owner whose
/// domain wasn't read); empty when the user couldn't be read, whatever the domain.
[[nodiscard]] inline std::string qualified(std::string_view domain, std::string_view user)
{
    if (user.empty() || domain.empty())
    {
        return std::string{user};
    }
    std::string text;
    text.reserve(domain.size() + 1 + user.size());
    text.append(domain);
    text += DOMAIN_SEPARATOR;
    text.append(user);
    return text;
}

/// Whether @p term is in the account qualified() writes for @p domain and @p user, by Unicode simple case
/// folding (#4302: "élodie" matches "PC\\Élodie"):
/// the short name ("alice", "lic"), the domain ("PC"), or a span across the separator ("pc\\al"). Nothing
/// matches an owner whose user couldn't be read. Allocates nothing.
[[nodiscard]] constexpr bool matches(std::string_view domain, std::string_view user, std::string_view term) noexcept
{
    if (user.empty())
    {
        return false;
    }
    if (Platform::containsFoldedCase(user, term))
    {
        return true;
    }
    if (domain.empty())
    {
        return false;
    }
    if (Platform::containsFoldedCase(domain, term))
    {
        return true;
    }
    // A span across the separator: the term's text before its first separator ends the domain, and the
    // text after it starts the user name.
    const std::size_t separator = term.find(DOMAIN_SEPARATOR);
    if (separator == std::string_view::npos)
    {
        return false;
    }
    // The term's head must end the domain and its tail start the user name, by Unicode simple case folding
    // (#4302). Views built from data() and lengths, not substr(), which can throw: separator < term.size().
    const std::string_view head(term.data(), separator);
    const std::string_view tail(term.data() + separator + 1, term.size() - separator - 1);
    return Platform::endsWithFoldedCase(domain, head) && Platform::startsWithFoldedCase(user, tail);
}

} // namespace Domain::AccountName
