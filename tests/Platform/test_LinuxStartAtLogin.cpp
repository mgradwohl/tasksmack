/// @file test_LinuxStartAtLogin.cpp
/// @brief Start at sign-in on Linux (#3254): the XDG autostart entry's Exec escaping both ways, the autostart
/// directory from the environment, and the read, write and remove round trip in a scratch directory, never the
/// real ~/.config/autostart.

#include "Domain/StartAtLogin.h"
#include "Platform/IStartAtLogin.h"
#include "Platform/Linux/LinuxStartAtLogin.h"
#include "ScopedTempDir.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

#include <unistd.h>

namespace Platform
{
namespace
{

using namespace StartAtLoginEntry;

TEST(LinuxStartAtLoginEntryTest, TheExecValueQuotesAndEscapesTheProgramAndReadsBackTheSame)
{
    EXPECT_EQ(execValue(R"("/usr/bin/TaskSmack" --minimized)"), std::optional<std::string>(R"("/usr/bin/TaskSmack" --minimized)"));
    // Spaces, a backtick, a dollar, a backslash and a percent: the quoting rule's escapes, % doubled, then
    // every backslash doubled again by the string type. (A double quote in the path can't be: the command line
    // Domain::StartAtLogin reads, as Windows does, ends the program at its first quote.)
    const std::string program = R"(/home/a b/`y`/$z/w\v/100%/TaskSmack)";
    const std::string command = Domain::StartAtLogin::runCommand(program);
    const std::optional<std::string> exec = execValue(command);
    ASSERT_TRUE(exec.has_value());
    EXPECT_EQ(*exec, R"("/home/a b/\\`y\\`/\\$z/w\\\\v/100%%/TaskSmack" --minimized)");
    EXPECT_EQ(commandFromExec(*exec), std::optional<std::string>(command)) << "the round trip";
    EXPECT_EQ(Domain::StartAtLogin::commandProgram(commandFromExec(*exec).value_or("")), std::optional<std::string>(program));

    EXPECT_EQ(execValue(R"("/a/b" x)"), std::optional<std::string>(R"("/a/b" x)"));
    EXPECT_EQ(execValue("/usr/bin/TaskSmack"), std::optional<std::string>(R"("/usr/bin/TaskSmack")")) << "no arguments, unquoted";
    EXPECT_EQ(execValue(""), std::nullopt);
    EXPECT_EQ(execValue("\"/a\nb\" --minimized"), std::nullopt) << "an Exec can't hold a newline";
}

TEST(LinuxStartAtLoginEntryTest, AnExecWrittenElsewhereReadsAsACommand)
{
    EXPECT_EQ(commandFromExec("TaskSmack --minimized"), std::optional<std::string>(R"("TaskSmack" --minimized)")) << "unquoted";
    EXPECT_EQ(commandFromExec("/opt/TaskSmack"), std::optional<std::string>(R"("/opt/TaskSmack")"));
    // \s is a space, which outside quotes separates the arguments, as the spec reads it.
    EXPECT_EQ(commandFromExec(R"(/opt/my\sapp/TaskSmack)"), std::optional<std::string>(R"("/opt/my" app/TaskSmack)"));
    EXPECT_EQ(commandFromExec(R"("/a\\tb" x)"), std::optional<std::string>(R"("/a\tb" x)")) << "an escaped backslash before a t";
    EXPECT_EQ(commandFromExec(R"("/a\qb")"), std::optional<std::string>(R"("/a\qb")")) << "not an escape: kept";
    EXPECT_EQ(commandFromExec("  "), std::nullopt);
    EXPECT_EQ(commandFromExec(R"("/never/closed --minimized)"), std::nullopt);
    EXPECT_EQ(commandFromExec("\"/a\\nb\\t\\rc\""), std::optional<std::string>("\"/a\nb\t\rc\""));
}

TEST(LinuxStartAtLoginEntryTest, TheAutostartDirectoryFollowsXdgConfigHomeThenHome)
{
    EXPECT_EQ(autostartDirectory("/x/config", "/home/a"), std::filesystem::path("/x/config/autostart"));
    EXPECT_EQ(autostartDirectory("relative", "/home/a"), std::filesystem::path("/home/a/.config/autostart")) << "ignored when relative";
    EXPECT_EQ(autostartDirectory(nullptr, "/home/a"), std::filesystem::path("/home/a/.config/autostart"));
    EXPECT_TRUE(autostartDirectory(nullptr, nullptr).empty());
    EXPECT_TRUE(autostartDirectory("", "rel").empty());
    const std::string text = entryText(R"("/usr/bin/TaskSmack" --minimized)");
    EXPECT_TRUE(text.starts_with("[Desktop Entry]\n")) << text;
    EXPECT_TRUE(text.contains("\nType=Application\n")) << text;
    EXPECT_TRUE(text.contains("\nExec=\"/usr/bin/TaskSmack\" --minimized\n")) << text;
}

TEST(LinuxStartAtLoginTest, WriteReadAndRemoveTheEntryInItsOwnDirectory)
{
    const TestSupport::ScopedTempDir home("tasksmack_start_at_login");
    const std::filesystem::path autostart = home.path / "config" / "autostart"; // made by the first write
    const std::filesystem::path neighbour = home.path / "config" / "autostart" / "other.desktop";
    LinuxStartAtLogin entry(autostart, "/home/a b/TaskSmack");

    auto facts = entry.read();
    ASSERT_TRUE(facts.has_value()) << facts.error();
    EXPECT_EQ(facts->command, std::nullopt) << "no file: off";
    EXPECT_EQ(facts->executable, "/home/a b/TaskSmack");
    EXPECT_FALSE(facts->installed) << "a portable copy";
    EXPECT_FALSE(facts->caseInsensitivePaths);

    const std::string command = Domain::StartAtLogin::runCommand("/home/a b/TaskSmack");
    ASSERT_TRUE(entry.write(command).has_value());
    EXPECT_FALSE(std::filesystem::exists(autostart / "tasksmack.desktop.partial"));
    facts = entry.read();
    ASSERT_TRUE(facts.has_value());
    EXPECT_EQ(facts->command, std::optional<std::string>(command));
    EXPECT_EQ(Domain::StartAtLogin::classify(facts->command, facts->executable, facts->installed, false).state,
              Domain::StartAtLogin::State::On);

    // Switched off in the desktop's own settings, or with no Exec: off, and an unreadable command.
    std::ofstream(neighbour) << "[Desktop Entry]\nExec=other\n";
    std::ofstream(autostart / "tasksmack.desktop") << "[Desktop Entry]\nExec=\"/x\"\nHidden=true\n";
    EXPECT_EQ(entry.read()->command, std::nullopt);
    std::ofstream(autostart / "tasksmack.desktop") << "[Desktop Entry]\nExec=\"/x\"\nX-GNOME-Autostart-enabled=false\n";
    EXPECT_EQ(entry.read()->command, std::nullopt);
    std::ofstream(autostart / "tasksmack.desktop") << "[Desktop Entry]\nName=TaskSmack\n";
    EXPECT_EQ(entry.read()->command, std::optional<std::string>(""));
    std::ofstream(autostart / "tasksmack.desktop") << "[Desktop Entry]\nExec=\"/never/closed\n";
    EXPECT_EQ(entry.read()->command, std::optional<std::string>(""));
    std::ofstream(autostart / "tasksmack.desktop") << "not a desktop entry\n";
    EXPECT_EQ(entry.read()->command, std::nullopt);

    // Remove takes only its own file, and succeeds when it is already gone.
    ASSERT_TRUE(entry.remove().has_value());
    EXPECT_FALSE(std::filesystem::exists(autostart / "tasksmack.desktop"));
    EXPECT_TRUE(std::filesystem::exists(neighbour));
    EXPECT_TRUE(entry.remove().has_value());

    LinuxStartAtLogin installed(autostart, "/usr/bin/TaskSmack");
    EXPECT_TRUE(installed.read()->installed);
    EXPECT_TRUE(LinuxStartAtLogin(autostart, "/opt/tasksmack/TaskSmack").read()->installed);
}

TEST(LinuxStartAtLoginTest, NoHomeOrAnUnwritableFolderSaysWhy)
{
    LinuxStartAtLogin nowhere({}, "/usr/bin/TaskSmack");
    EXPECT_FALSE(nowhere.read().has_value());
    EXPECT_FALSE(nowhere.write(R"("/usr/bin/TaskSmack" --minimized)").has_value());
    EXPECT_FALSE(nowhere.remove().has_value());

    const TestSupport::ScopedTempDir home("tasksmack_start_at_login_bad");
    LinuxStartAtLogin entry(home.path / "autostart", "/usr/bin/TaskSmack");
    EXPECT_FALSE(entry.write("\"/a\nb\"").has_value()) << "a command an Exec can't hold";
    // A file where the autostart folder should be: it can't be made.
    std::ofstream(home.path / "autostart") << "in the way\n";
    const auto blocked = entry.write(R"("/usr/bin/TaskSmack" --minimized)");
    ASSERT_FALSE(blocked.has_value());
    EXPECT_TRUE(blocked.error().contains("couldn't be made")) << blocked.error();
    if (::geteuid() == 0)
    {
        GTEST_SKIP() << "root reads and writes anything";
    }
    std::filesystem::remove(home.path / "autostart");
    std::filesystem::create_directories(home.path / "autostart");
    std::ofstream(home.path / "autostart" / "tasksmack.desktop") << "[Desktop Entry]\nExec=x\n";
    std::filesystem::permissions(home.path / "autostart" / "tasksmack.desktop", std::filesystem::perms::none);
    EXPECT_FALSE(entry.read().has_value()) << "unreadable";
    std::filesystem::permissions(home.path / "autostart", std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec);
    EXPECT_FALSE(entry.write(R"("/usr/bin/TaskSmack" --minimized)").has_value()) << "a read-only folder";
    EXPECT_FALSE(entry.remove().has_value());
    std::filesystem::permissions(home.path / "autostart", std::filesystem::perms::owner_all);
    std::filesystem::permissions(home.path / "autostart" / "tasksmack.desktop", std::filesystem::perms::owner_all);
}

} // namespace
} // namespace Platform
