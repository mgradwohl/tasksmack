#pragma once

// A process owner's account as the Processes table and Process Details write it (#3328): "PC\alice" on
// Windows, where the probe reads the owner's domain with its user name (ProcessSnapshot::userDomain,
// #3310), and the user name alone on Linux, which has no domain. Pure and allocation-free except for
// qualified(), so the Processes filter can run matches() on every row.

#include "Platform/AsciiCase.h"

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

/// Whether @p term is in the account qualified() writes for @p domain and @p user, ASCII case aside:
/// the short name ("alice", "lic"), the domain ("PC"), or a span across the separator ("pc\\al"). Nothing
/// matches an owner whose user couldn't be read. Allocates nothing.
[[nodiscard]] constexpr bool matches(std::string_view domain, std::string_view user, std::string_view term) noexcept
{
    if (user.empty())
    {
        return false;
    }
    if (Platform::containsIgnoreCase(user, term))
    {
        return true;
    }
    if (domain.empty())
    {
        return false;
    }
    if (Platform::containsIgnoreCase(domain, term))
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
    // Compared character by character, not through substr(), which can throw out_of_range: every index
    // below is checked to lie inside its string first, and operator[] and foldAscii() can't throw.
    const std::size_t headSize = separator;
    const std::size_t tailSize = term.size() - separator - 1;
    if (headSize > domain.size() || tailSize > user.size())
    {
        return false;
    }
    const std::size_t domainStart = domain.size() - headSize;
    for (std::size_t i = 0; i < headSize; ++i)
    {
        if (Platform::foldAscii(domain[domainStart + i]) != Platform::foldAscii(term[i]))
        {
            return false;
        }
    }
    for (std::size_t i = 0; i < tailSize; ++i)
    {
        if (Platform::foldAscii(user[i]) != Platform::foldAscii(term[separator + 1 + i]))
        {
            return false;
        }
    }
    return true;
}

} // namespace Domain::AccountName
