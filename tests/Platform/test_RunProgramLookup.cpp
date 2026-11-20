// Run new task's program lookup (#4532, #4400): found as the account it will run as would find it.

#include "Platform/Linux/RunProgramLookup.h"

#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

#include <unistd.h>

namespace
{

using Platform::RunProgramLookup::executableBy;
using Platform::RunProgramLookup::findProgram;
using Platform::RunProgramLookup::runsAsFor;

/// A user who may run every file with an execute bit (the predicate's real form is tested on its own below).
const auto anyone = [](const std::string& path)
{
    return Platform::RootTools::isExecutableFile(path);
};
using Platform::RunProgramLookup::pathIn;
using Platform::RunProgramLookup::RunsAs;

/// Two folders, each with its own "tool" program: one standing for TaskSmack's PATH, one for the invoking user's.
class RunProgramLookupTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Root = std::filesystem::temp_directory_path() / ("tasksmack-4532-" + std::to_string(::getpid()));
        std::error_code ec;
        std::filesystem::remove_all(m_Root, ec);
        for (const auto* dir : {"own", "user"})
        {
            std::filesystem::create_directories(m_Root / dir);
            const std::filesystem::path tool = m_Root / dir / "tool";
            std::ofstream(tool) << "#!/bin/sh\n";
            std::filesystem::permissions(tool, std::filesystem::perms(0755));
        }
    }
    void TearDown() override
    {
        std::error_code ec;
        std::filesystem::remove_all(m_Root, ec);
    }
    [[nodiscard]] std::string own() const
    {
        return (m_Root / "own").string();
    }
    [[nodiscard]] std::string user() const
    {
        return (m_Root / "user").string();
    }
    std::filesystem::path m_Root;
};

TEST_F(RunProgramLookupTest, AsRootABareNameIsFoundOnlyTheRootWayNeverOnTheInheritedPath)
{
    std::string askedFor;
    const auto rootFind = [&askedFor](std::string_view name)
    {
        askedFor = name;
        return std::optional<std::string>();
    };
    // sudo -E: TaskSmack's PATH is the invoking user's, and "tool" is on it, but as root that PATH isn't searched.
    EXPECT_EQ(findProgram("tool", RunsAs::Root, own(), user(), rootFind, anyone), std::nullopt);
    EXPECT_EQ(askedFor, "tool");

    const auto system = [](std::string_view)
    {
        return std::optional<std::string>("/usr/bin/tool");
    };
    EXPECT_EQ(findProgram("tool", RunsAs::Root, own(), user(), system, anyone), std::optional<std::string>("/usr/bin/tool"));
}

TEST_F(RunProgramLookupTest, AsRootAPathTypedIsThePathRun)
{
    const auto never = [](std::string_view) -> std::optional<std::string>
    {
        ADD_FAILURE() << "a path isn't looked up";
        return std::nullopt;
    };
    const std::string typed = own() + "/tool";
    EXPECT_EQ(findProgram(typed, RunsAs::Root, "", "", never, anyone), std::optional<std::string>(typed));
}

TEST_F(RunProgramLookupTest, AsTheInvokingUserTheirPathIsSearchedNotTaskSmacks)
{
    const auto never = [](std::string_view) -> std::optional<std::string>
    {
        return std::nullopt;
    };
    EXPECT_EQ(findProgram("tool", RunsAs::InvokingUser, "/nonexistent-4400", user(), never, anyone),
              std::optional<std::string>(user() + "/tool"))
        << "#4400: found as the user's shell would find it";
    EXPECT_EQ(findProgram("tool", RunsAs::InvokingUser, own(), "", never, anyone), std::nullopt) << "not on root's PATH instead";
}

TEST_F(RunProgramLookupTest, AsItselfTaskSmacksOwnPathIsSearched)
{
    const auto never = [](std::string_view) -> std::optional<std::string>
    {
        return std::nullopt;
    };
    EXPECT_EQ(findProgram("tool", RunsAs::Self, own() + ":" + user(), "/nonexistent-4400", never, anyone),
              std::optional<std::string>(own() + "/tool"));
    EXPECT_EQ(findProgram("no-such-tool-4532", RunsAs::Self, own(), user(), never, anyone), std::nullopt);
}

// The #4532 review: whom the program runs as. Run as administrator is root's, through pkexec or not, so its name is
// found the root way even when TaskSmack isn't root; root with an invoking user runs it as them.
TEST(RunProgramLookupPathTest, WhomTheProgramRunsAs)
{
    EXPECT_EQ(runsAsFor(false, false, false), RunsAs::Self);
    EXPECT_EQ(runsAsFor(false, true, false), RunsAs::Root) << "through pkexec: root runs it";
    EXPECT_EQ(runsAsFor(true, true, true), RunsAs::Root) << "asked for administrator: not the user's session";
    EXPECT_EQ(runsAsFor(true, false, true), RunsAs::InvokingUser);
    EXPECT_EQ(runsAsFor(true, false, false), RunsAs::Root) << "root with no invoking user runs it as itself";
}

// The #4532 review: as the invoking user, a file only root could run is skipped, and the search goes on, as their
// shell's would.
TEST_F(RunProgramLookupTest, AsTheInvokingUserAFileTheyCantRunIsSkipped)
{
    const std::filesystem::path first = m_Root / "first";
    std::filesystem::create_directories(first);
    std::ofstream(first / "tool") << "#!/bin/sh\n";
    std::filesystem::permissions(first / "tool", std::filesystem::perms(0700));
    const uid_t someoneElse = ::geteuid() + 1;
    const auto canRun = [someoneElse](const std::string& path)
    {
        return executableBy(path, someoneElse, someoneElse, {});
    };
    const auto never = [](std::string_view) -> std::optional<std::string>
    {
        return std::nullopt;
    };
    EXPECT_EQ(findProgram("tool", RunsAs::InvokingUser, "", first.string() + ":" + user(), never, canRun),
              std::optional<std::string>(user() + "/tool"))
        << "first/tool is 0700 and not theirs; user/tool is 0755";
    EXPECT_TRUE(executableBy((first / "tool").string(), ::geteuid(), ::getegid(), {})) << "its owner may run it";
    EXPECT_FALSE(executableBy((m_Root / "own").string(), ::geteuid(), ::getegid(), {})) << "a folder isn't a program";
}

TEST(RunProgramLookupPathTest, PathInReadsThePathEntryOfAnEnvironment)
{
    const std::array<std::string, 3> env{"DISPLAY=:0", "PATHLIKE=x", "PATH=/home/u/bin:/usr/bin"};
    EXPECT_EQ(pathIn(env), "/home/u/bin:/usr/bin");
    const std::array<std::string, 1> none{"HOME=/home/u"};
    EXPECT_EQ(pathIn(none), "");
}

} // namespace
