// Whether the user a root session runs for could open a folder (#4765): by the permission bits along the path.

#include "Platform/Linux/UserFolderAccess.h"

#include <gtest/gtest.h>

#include <map>
#include <optional>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <sys/types.h>

namespace Platform::UserFolderAccess
{
namespace
{

constexpr uid_t USER = 1000;
constexpr gid_t USER_GROUP = 1000;
constexpr gid_t STAFF = 50;

/// A tree of folders by path, each with its owner, group and mode.
StatPath tree(std::map<std::string, PathModes> nodes)
{
    return [nodes = std::move(nodes)](const std::string& path) -> std::optional<PathModes>
    {
        if (const auto found = nodes.find(path); found != nodes.end())
        {
            return found->second;
        }
        return std::nullopt;
    };
}

PathModes dir(uid_t uid, gid_t gid, mode_t mode)
{
    return PathModes{.uid = uid, .gid = gid, .mode = static_cast<mode_t>(S_IFDIR | mode)};
}

TEST(UserFolderAccessTest, RootsDumpFolderIsClosedToTheUserItRunsFor)
{
    // #4765: /root is 0700 root's, so the user can't search it, whatever TaskSmack-dumps' own mode is.
    const StatPath stat = tree({
        {"/", dir(0, 0, 0755)},
        {"/root", dir(0, 0, 0700)},
        {"/root/TaskSmack-dumps", dir(0, 0, 0700)},
    });
    const std::vector<gid_t> groups{USER_GROUP};
    EXPECT_FALSE(canOpenFolder("/root/TaskSmack-dumps", USER, USER_GROUP, groups, stat));
    EXPECT_TRUE(canOpenFolder("/root/TaskSmack-dumps", 0, 0, {}, stat)) << "root opens anything";
}

TEST(UserFolderAccessTest, AFolderTheUserMaySearchToAndReadOpens)
{
    const StatPath stat = tree({
        {"/", dir(0, 0, 0755)},
        {"/srv", dir(0, 0, 0711)},       // searchable, not listable: enough to pass through
        {"/srv/dumps", dir(0, 0, 0755)}, // readable by others
        {"/srv/team", dir(0, STAFF, 0750)},
        {"/srv/mine", dir(USER, USER_GROUP, 0700)},
        {"/srv/listonly", dir(0, 0, 0744)}, // readable but not searchable
    });
    const std::vector<gid_t> groups{USER_GROUP};
    EXPECT_TRUE(canOpenFolder("/srv/dumps", USER, USER_GROUP, groups, stat));
    EXPECT_TRUE(canOpenFolder("/srv/dumps/", USER, USER_GROUP, groups, stat)) << "a trailing slash names the same folder";
    EXPECT_TRUE(canOpenFolder("/srv/mine", USER, USER_GROUP, groups, stat)) << "the owner's bits";
    EXPECT_FALSE(canOpenFolder("/srv/team", USER, USER_GROUP, groups, stat)) << "not in staff: the others' bits, none";
    const std::vector<gid_t> withStaff{USER_GROUP, STAFF};
    EXPECT_TRUE(canOpenFolder("/srv/team", USER, USER_GROUP, withStaff, stat)) << "a supplementary group's bits";
    EXPECT_FALSE(canOpenFolder("/srv/listonly", USER, USER_GROUP, groups, stat)) << "a file manager must also search it";
}

TEST(UserFolderAccessTest, WhatCantBeReadOrIsntAbsoluteIsClosed)
{
    const StatPath stat = tree({{"/", dir(0, 0, 0755)}});
    const std::vector<gid_t> groups{USER_GROUP};
    EXPECT_FALSE(canOpenFolder("/missing", USER, USER_GROUP, groups, stat));
    EXPECT_FALSE(canOpenFolder("relative/folder", USER, USER_GROUP, groups, stat));
    EXPECT_TRUE(canOpenFolder("/", USER, USER_GROUP, groups, stat));
}

TEST(UserFolderAccessTest, TheOwnersBitsWinOverTheGroupsAsTheKernelDecides)
{
    // The owner with no read bit is refused even when its group could read: the kernel checks the owner's bits alone.
    const StatPath stat = tree({{"/", dir(0, 0, 0755)}, {"/x", dir(USER, USER_GROUP, 0070)}});
    const std::vector<gid_t> groups{USER_GROUP};
    EXPECT_FALSE(canOpenFolder("/x", USER, USER_GROUP, groups, stat));
}

TEST(UserFolderAccessTest, TheRealStatReadsThisProcesssOwnTempFolder)
{
    const std::optional<PathModes> modes = statPath("/");
    ASSERT_TRUE(modes.has_value());
    EXPECT_TRUE(S_ISDIR(modes->mode));
    EXPECT_FALSE(statPath("/no/such/folder/here").has_value());
}

} // namespace
} // namespace Platform::UserFolderAccess
