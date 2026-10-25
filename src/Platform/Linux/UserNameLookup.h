#pragma once

#include <cerrno>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <grp.h>
#include <pwd.h>
#include <sys/types.h>

namespace Platform
{

namespace UserNameLookupDetail
{

/// What @p pick takes from the entry for @p key, read through a getpwuid_r(3)-style @p lookUp (key,
/// entry, buffer, size, result) while its buffer still holds the entry's strings; nullopt if there is no
/// entry. An entry larger than the buffer (big LDAP/SSSD records, a group with many members) fails with
/// ERANGE: the buffer grows and the lookup is retried, up to 1 MiB (#1155, #1526).
template<typename Entry, typename Key, typename LookUp, typename Pick>
[[nodiscard]] auto lookUpEntry(Key key, LookUp lookUp, Pick pick) -> decltype(pick(std::declval<const Entry&>()))
{
    constexpr std::size_t INITIAL_BUFFER = 1024;
    constexpr std::size_t MAX_BUFFER = std::size_t{1024} * 1024;

    Entry entry = {};
    Entry* result = nullptr;
    std::vector<char> buffer(INITIAL_BUFFER);
    int lookupError = 0;
    while ((lookupError = lookUp(key, &entry, buffer.data(), buffer.size(), &result)) == ERANGE && buffer.size() < MAX_BUFFER)
    {
        buffer.resize(buffer.size() * 2);
    }
    if (lookupError == 0 && result != nullptr)
    {
        return pick(*result);
    }
    return std::nullopt;
}

} // namespace UserNameLookupDetail

/// The @p field of @p uid's passwd entry, via @p getpwuidR -- getpwuid_r(3), or a stand-in with its
/// signature in tests -- or nullopt if there is no entry or the field is unset.
template<typename GetPwUidR>
[[nodiscard]] std::optional<std::string> lookUpPasswdField(uid_t uid, GetPwUidR getpwuidR, char* passwd::* field)
{
    return UserNameLookupDetail::lookUpEntry<passwd>(uid,
                                                     getpwuidR,
                                                     [field](const passwd& entry) -> std::optional<std::string>
                                                     {
                                                         if (entry.*field == nullptr)
                                                         {
                                                             return std::nullopt;
                                                         }
                                                         return std::string(entry.*field);
                                                     });
}

/// The user name for @p uid (its passwd entry's pw_name), or nullopt if there is none.
template<typename GetPwUidR> [[nodiscard]] std::optional<std::string> lookUpUserName(uid_t uid, GetPwUidR getpwuidR)
{
    return lookUpPasswdField(uid, getpwuidR, &passwd::pw_name);
}

/// The home directory of @p uid (its passwd entry's pw_dir), or nullopt if there is none (#2197).
template<typename GetPwUidR> [[nodiscard]] std::optional<std::string> lookUpHomeDirectory(uid_t uid, GetPwUidR getpwuidR)
{
    return lookUpPasswdField(uid, getpwuidR, &passwd::pw_dir);
}

/// The group name for @p gid, via @p getgrgidR -- getgrgid_r(3), or a stand-in with its signature in
/// tests -- or nullopt if there is none.
template<typename GetGrGidR> [[nodiscard]] std::optional<std::string> lookUpGroupName(gid_t gid, GetGrGidR getgrgidR)
{
    return UserNameLookupDetail::lookUpEntry<group>(gid,
                                                    getgrgidR,
                                                    [](const group& entry) -> std::optional<std::string>
                                                    {
                                                        if (entry.gr_name == nullptr)
                                                        {
                                                            return std::nullopt;
                                                        }
                                                        return std::string(entry.gr_name);
                                                    });
}

/// The UID of the user named @p name, via @p getpwnamR -- getpwnam_r(3), or a stand-in with its
/// signature in tests -- or nullopt if there is none. Unlike getpwnam(3), safe on two threads at once:
/// System Information's read and the crash history's worker both read the crashes (#2416).
template<typename GetPwNamR> [[nodiscard]] std::optional<uid_t> lookUpUserId(const char* name, GetPwNamR getpwnamR)
{
    return UserNameLookupDetail::lookUpEntry<passwd>(
        name, getpwnamR, [](const passwd& entry) -> std::optional<uid_t> { return entry.pw_uid; });
}

/// The GID of the group named @p name, via @p getgrnamR -- getgrnam_r(3), or a stand-in with its
/// signature in tests -- or nullopt if there is none. Safe on two threads at once, as lookUpUserId() is.
template<typename GetGrNamR> [[nodiscard]] std::optional<gid_t> lookUpGroupId(const char* name, GetGrNamR getgrnamR)
{
    return UserNameLookupDetail::lookUpEntry<group>(
        name, getgrnamR, [](const group& entry) -> std::optional<gid_t> { return entry.gr_gid; });
}

} // namespace Platform
