#pragma once

#include <cerrno>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include <grp.h>
#include <pwd.h>
#include <sys/types.h>

namespace Platform
{

/// The user name for @p uid, via @p getpwuidR -- getpwuid_r(3), or a stand-in with its signature in
/// tests -- or nullopt if there is none.
///
/// An entry larger than the buffer (big LDAP/SSSD records) fails with ERANGE: the buffer grows and
/// the lookup is retried, up to 1 MiB, rather than the caller caching the numeric UID for good (#1155).
template<typename GetPwUidR> [[nodiscard]] std::optional<std::string> lookUpUserName(uid_t uid, GetPwUidR getpwuidR)
{
    constexpr std::size_t INITIAL_BUFFER = 1024;
    constexpr std::size_t MAX_BUFFER = std::size_t{1024} * 1024;

    struct passwd entry = {};
    struct passwd* result = nullptr;
    std::vector<char> buffer(INITIAL_BUFFER);
    int lookupError = 0;
    while ((lookupError = getpwuidR(uid, &entry, buffer.data(), buffer.size(), &result)) == ERANGE && buffer.size() < MAX_BUFFER)
    {
        buffer.resize(buffer.size() * 2);
    }
    if (lookupError == 0 && result != nullptr && result->pw_name != nullptr)
    {
        return std::string(result->pw_name);
    }
    return std::nullopt;
}

/// The group name for @p gid, via @p getgrgidR -- getgrgid_r(3), or a stand-in with its signature in
/// tests -- or nullopt if there is none. A group with many members makes a large entry, so the buffer
/// grows on ERANGE as lookUpUserName()'s does (#1526).
template<typename GetGrGidR> [[nodiscard]] std::optional<std::string> lookUpGroupName(gid_t gid, GetGrGidR getgrgidR)
{
    constexpr std::size_t INITIAL_BUFFER = 1024;
    constexpr std::size_t MAX_BUFFER = std::size_t{1024} * 1024;

    struct group entry = {};
    struct group* result = nullptr;
    std::vector<char> buffer(INITIAL_BUFFER);
    int lookupError = 0;
    while ((lookupError = getgrgidR(gid, &entry, buffer.data(), buffer.size(), &result)) == ERANGE && buffer.size() < MAX_BUFFER)
    {
        buffer.resize(buffer.size() * 2);
    }
    if (lookupError == 0 && result != nullptr && result->gr_name != nullptr)
    {
        return std::string(result->gr_name);
    }
    return std::nullopt;
}

} // namespace Platform
