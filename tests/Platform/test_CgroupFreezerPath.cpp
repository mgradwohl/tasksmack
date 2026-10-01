#include "Platform/Linux/CgroupFreezerPath.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <string_view>

namespace Platform
{
namespace
{

using CgroupPath::buildContainedCgroupPath;
using CgroupPath::isContainedIn;

// A string_view constant, not a std::filesystem::path: a namespace-scope path has a
// throwing constructor whose exception could not be caught (bugprone-throwing-static-initialization).
constexpr std::string_view FREEZER_BASE{"/sys/fs/cgroup/freezer"};

/// The built path as a generic (forward-slash) string, or an empty string when the builder refused
/// it. Two reasons for the indirection: generic_string() keeps assertions independent of the host's
/// preferred separator, and returning a plain value keeps clang-tidy's optional dataflow satisfied
/// (bugprone-unchecked-optional-access does not model gtest's ASSERT_TRUE early return, so it flags
/// even a checked .value() that follows one). The builder never yields an empty path on success --
/// it always contains at least the base -- so empty is an unambiguous "refused".
[[nodiscard]] std::string built(std::string_view relative, std::string_view leaf = "freezer.state")
{
    const auto path = buildContainedCgroupPath(std::filesystem::path(FREEZER_BASE), relative, leaf);
    return path.has_value() ? path->generic_string() : std::string{};
}

// The header is deliberately platform-API-free so this runs on every host, not just Linux: the
// containment rule is the security property behind CodeQL cpp/path-injection alert #2948, and it
// should not be verifiable only on the one platform that compiles LinuxProcessProbe.cpp.

TEST(CgroupFreezerPathTest, BuildsPathForOrdinaryCgroupSubPath)
{
    EXPECT_EQ(built("user.slice/user-1000.slice"), "/sys/fs/cgroup/freezer/user.slice/user-1000.slice/freezer.state");
}

TEST(CgroupFreezerPathTest, RejectsParentTraversal)
{
    // The escape this exists to prevent: /proc/<pid>/cgroup is parsed from a file, so a ".."
    // component would walk out of the freezer hierarchy and make the caller's FROZEN/FREEZING
    // prefix test a content oracle for any readable file.
    EXPECT_TRUE(built("../../../../etc/passwd", "freezer.state").empty());
    EXPECT_TRUE(built("user.slice/../../../etc/shadow", "freezer.state").empty());
    EXPECT_TRUE(built("..", "freezer.state").empty());
}

TEST(CgroupFreezerPathTest, RejectsTraversalThatWouldNormaliseBackInsideTheBase)
{
    // Rejected by the ".." scan even though the normalised result would still sit under the base.
    // Refusing to build it at all is the stricter and more predictable rule, and a cgroup path
    // from the kernel never contains "..".
    EXPECT_TRUE(built("user.slice/../system.slice", "freezer.state").empty());
}

TEST(CgroupFreezerPathTest, RejectsRerootingPaths)
{
    // An absolute relative-part would otherwise replace the base entirely when appended.
    EXPECT_TRUE(built("/etc/passwd", "freezer.state").empty());
}

TEST(CgroupFreezerPathTest, BuildsTheRootCgroupPathForAnEmptyRelativePart)
{
    // A process in the freezer hierarchy's root has a cgroup-v1 line of ".../freezer:/", and the
    // caller strips the leading slash, so the root arrives here as "". Rejecting it would silently
    // skip /sys/fs/cgroup/freezer/freezer.state and lose Suspended detection for root-cgroup
    // processes. Verified byte-identical to what the unvalidated construction produced for "".
    EXPECT_EQ(built(""), "/sys/fs/cgroup/freezer/freezer.state");
}

TEST(CgroupFreezerPathTest, RejectsAnEmptyLeaf)
{
    // Unlike the relative part, an empty leaf has no meaning: it would name the base directory,
    // and there is no file there to read.
    EXPECT_TRUE(built("user.slice", "").empty());
    EXPECT_TRUE(built("", "").empty());
}

TEST(CgroupFreezerPathTest, AcceptsASingleDotComponentWhichNormalisesAway)
{
    // "." is harmless: it cannot leave the base, and normalisation removes it.
    EXPECT_EQ(built("./user.slice"), "/sys/fs/cgroup/freezer/user.slice/freezer.state");
}

// isContainedIn() is the backstop, and no input buildContainedCgroupPath() admits can reach its
// rejection -- the ".." policy catches them all first. Verified: disabling the containment check
// left all of the tests above passing. So it is tested directly here instead, because a guard no
// test can reach is indistinguishable from dead code.
TEST(ContainedPathTest, RejectsACandidateOutsideTheBase)
{
    EXPECT_FALSE(isContainedIn("/sys/fs/cgroup/freezer", "/etc/passwd"));
    EXPECT_FALSE(isContainedIn("/sys/fs/cgroup/freezer", "/sys/fs/cgroup/memory/x"));
    EXPECT_FALSE(isContainedIn("/sys/fs/cgroup/freezer", "/sys/fs"));
}

TEST(ContainedPathTest, RejectsASiblingWhoseNameMerelyStartsWithTheBase)
{
    // The case a string-prefix check gets wrong: the text of the second path does begin with the
    // first, but it is a different directory.
    EXPECT_FALSE(isContainedIn("/sys/fs/cgroup/freezer", "/sys/fs/cgroup/freezer-evil/freezer.state"));
}

TEST(ContainedPathTest, RejectsTheBaseItself)
{
    // Strictly inside: the base is a directory, so there is no file to read at exactly that path.
    EXPECT_FALSE(isContainedIn("/sys/fs/cgroup/freezer", "/sys/fs/cgroup/freezer"));
}

TEST(ContainedPathTest, AcceptsADescendantAndNormalisesBeforeComparing)
{
    EXPECT_TRUE(isContainedIn("/sys/fs/cgroup/freezer", "/sys/fs/cgroup/freezer/user.slice/freezer.state"));
    // Normalisation happens on both sides, so an un-normalised candidate still compares correctly.
    EXPECT_TRUE(isContainedIn("/sys/fs/cgroup/freezer", "/sys/fs/cgroup/freezer/./user.slice/freezer.state"));
    EXPECT_TRUE(isContainedIn("/sys/fs/cgroup/./freezer", "/sys/fs/cgroup/freezer/user.slice/freezer.state"));
    // And a candidate that traverses out and back in is contained, which is why the ".." policy in
    // buildContainedCgroupPath() is a separate, stricter rule rather than a consequence of this one.
    EXPECT_TRUE(isContainedIn("/sys/fs/cgroup/freezer", "/sys/fs/cgroup/freezer/a/../b/freezer.state"));
}

TEST(CgroupFreezerPathTest, DoesNotAcceptASiblingDirectorySharingTheBaseNamePrefix)
{
    // A string-prefix containment check would accept "/sys/fs/cgroup/freezer-evil/..." because it
    // starts with the base's text. The component-wise check does not, which is why containment is
    // verified per path component rather than per character.
    EXPECT_FALSE(built("x").empty());
    EXPECT_FALSE(buildContainedCgroupPath(std::filesystem::path("/sys/fs/cgroup/freezer-evil"), "..", "freezer.state").has_value());
}

} // namespace
} // namespace Platform
