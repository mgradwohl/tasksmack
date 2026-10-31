// The Startup tab on Linux (#2541): XDG autostart parsing, the user-over-system precedence, the desktop
// filters, and switching an entry by a user copy -- over fixture trees in a temporary directory.

#include "Platform/IStartupActions.h"
#include "Platform/IStartupProbe.h"
#include "Platform/Linux/LinuxStartup.h"
#include "Platform/Linux/SystemdUnitFile.h"
#include "Platform/Linux/XdgAutostart.h"
#include "Platform/ScopedTempDir.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include <unistd.h>

namespace Platform
{
namespace
{

/// Where the tests' programs (sh) are found, rather than this machine's $PATH.
constexpr const char* SEARCH_PATH = "/usr/bin:/bin";

void writeText(const std::filesystem::path& path, const std::string& text)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
}

[[nodiscard]] std::string readText(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

/// @p text parsed, or a default entry (failing the test) when it doesn't parse.
[[nodiscard]] XdgAutostart::Entry parsed(const std::string& text)
{
    const std::optional<XdgAutostart::Entry> entry = XdgAutostart::parse(text);
    EXPECT_TRUE(entry.has_value()) << text;
    return entry.value_or(XdgAutostart::Entry{});
}

[[nodiscard]] const StartupEntry* find(const std::vector<StartupEntry>& entries, const std::string& fileName)
{
    const auto it = std::ranges::find(entries, fileName, &StartupEntry::registrationName);
    return it != entries.end() ? &*it : nullptr;
}

TEST(XdgAutostartTest, ParsesTheDesktopEntryGroupOnly)
{
    const XdgAutostart::Entry entry = parsed("# comment\n"
                                             "[Desktop Entry]\n"
                                             "Type=Application\n"
                                             "Name=Clipboard\n"
                                             "Name[de]=Zwischenablage\n"
                                             "Exec=clipd --daemon %U\n"
                                             "Icon=edit-paste\n"
                                             "OnlyShowIn=GNOME;Unity;\n"
                                             "X-GNOME-Autostart-enabled=false\n"
                                             "[Desktop Action New]\n"
                                             "Name=Other\n"
                                             "Hidden=true\n");
    EXPECT_EQ(entry.name, "Clipboard");
    EXPECT_EQ(entry.exec, "clipd --daemon %U");
    EXPECT_EQ(entry.icon, "edit-paste");
    EXPECT_EQ(entry.onlyShowIn, (std::vector<std::string>{"GNOME", "Unity"}));
    EXPECT_FALSE(entry.hidden) << "another group's Hidden";
    EXPECT_EQ(entry.gnomeEnabled, false);
    EXPECT_FALSE(entry.enabled());
    EXPECT_FALSE(XdgAutostart::parse("Name=No group\n").has_value());
}

TEST(XdgAutostartTest, TheDesktopFiltersFollowXdgCurrentDesktop)
{
    XdgAutostart::Entry entry;
    EXPECT_TRUE(XdgAutostart::startsIn(entry, "KDE"));
    entry.onlyShowIn = {"GNOME"};
    EXPECT_TRUE(XdgAutostart::startsIn(entry, "ubuntu:GNOME"));
    EXPECT_FALSE(XdgAutostart::startsIn(entry, "KDE"));
    EXPECT_FALSE(XdgAutostart::startsIn(entry, "")) << "no desktop named: an OnlyShowIn entry doesn't start";
    entry.onlyShowIn.clear();
    entry.notShowIn = {"KDE"};
    EXPECT_FALSE(XdgAutostart::startsIn(entry, "KDE"));
    EXPECT_TRUE(XdgAutostart::startsIn(entry, "XFCE"));
}

// The Desktop Entry Specification's rule: the desktops in order, the first one either list names decides.
TEST(XdgAutostartTest, TheFirstDesktopEitherListNamesDecides)
{
    XdgAutostart::Entry entry;
    entry.onlyShowIn = {"ubuntu"};
    entry.notShowIn = {"GNOME"};
    EXPECT_TRUE(XdgAutostart::startsIn(entry, "ubuntu:GNOME"));
    EXPECT_FALSE(XdgAutostart::startsIn(entry, "GNOME:ubuntu"));
}

TEST(XdgAutostartTest, AByteOrderMarkAndCrlfLineEndingsAreKept)
{
    const std::string text = "\xEF\xBB\xBF[Desktop Entry]\r\nName=Tool\r\nExec=tool\r\nX-GNOME-Autostart-enabled=true\r\n";
    EXPECT_EQ(parsed(text).name, "Tool");
    EXPECT_EQ(XdgAutostart::withEnabled(text, false).value_or(""),
              "\xEF\xBB\xBF[Desktop Entry]\r\nHidden=true\r\nName=Tool\r\nExec=tool\r\nX-GNOME-Autostart-enabled=false\r\n");
}

TEST(XdgAutostartTest, SessionComponentsAreMarked)
{
    EXPECT_TRUE(parsed("[Desktop Entry]\nExec=a\nX-GNOME-Autostart-Phase=Initialization\n").sessionComponent);
    EXPECT_TRUE(parsed("[Desktop Entry]\nExec=a\nX-GNOME-HiddenUnderSystemd=true\n").sessionComponent);
    EXPECT_TRUE(parsed("[Desktop Entry]\nExec=a\nNoDisplay=true\n").noDisplay);
    EXPECT_FALSE(parsed("[Desktop Entry]\nExec=a\n").sessionComponent);
}

TEST(XdgAutostartTest, TheDirectoriesFollowTheBaseDirectorySpecification)
{
    const XdgAutostart::Dirs defaults = XdgAutostart::dirs("/home/u", "", "");
    EXPECT_EQ(defaults.user, std::filesystem::path("/home/u/.config/autostart"));
    EXPECT_EQ(defaults.system, (std::vector<std::filesystem::path>{"/etc/xdg/autostart"}));
    const XdgAutostart::Dirs set = XdgAutostart::dirs("/home/u", "/cfg", "/a:relative:/b");
    EXPECT_EQ(set.user, std::filesystem::path("/cfg/autostart"));
    EXPECT_EQ(set.system, (std::vector<std::filesystem::path>{"/a/autostart", "/b/autostart"})) << "relative ones ignored";
    EXPECT_TRUE(XdgAutostart::dirs("", "relative", "").user.empty());
}

TEST(XdgAutostartTest, SwitchingSetsHiddenAndKeepsEveryOtherLine)
{
    const std::string text = "[Desktop Entry]\nName=Tool\nHidden=false\nX-GNOME-Autostart-enabled=true\nExec=tool\n[Other]\nHidden=x\n";
    const std::string off = XdgAutostart::withEnabled(text, false).value_or("");
    EXPECT_EQ(off, "[Desktop Entry]\nHidden=true\nName=Tool\nX-GNOME-Autostart-enabled=false\nExec=tool\n[Other]\nHidden=x\n");
    EXPECT_FALSE(parsed(off).enabled());
    const std::string on = XdgAutostart::withEnabled(off, true).value_or("");
    EXPECT_TRUE(parsed(on).enabled());
    EXPECT_FALSE(XdgAutostart::withEnabled("Name=x\n", true).has_value());
}

class LinuxStartupTest : public ::testing::Test
{
  protected:
    TestSupport::ScopedTempDir m_Root{"ts_linux_startup"};
    XdgAutostart::Dirs m_Dirs{.user = m_Root.path / "home/.config/autostart",
                              .system = {m_Root.path / "etc/xdg/autostart", m_Root.path / "usr/etc/autostart"}};
};

TEST_F(LinuxStartupTest, ListsEachFileNameOnceTheUsersWinning)
{
    writeText(m_Dirs.system[0] / "shared.desktop", "[Desktop Entry]\nType=Application\nName=System copy\nExec=/bin/sh\n");
    writeText(m_Dirs.system[1] / "shared.desktop", "[Desktop Entry]\nType=Application\nName=Later system copy\nExec=/bin/sh\n");
    writeText(m_Dirs.user / "shared.desktop", "[Desktop Entry]\nType=Application\nName=User copy\nExec=/bin/sh\nHidden=true\n");
    writeText(m_Dirs.system[1] / "later.desktop", "[Desktop Entry]\nName=Later\nExec=sh -c true\n");
    writeText(m_Dirs.user / "mine.desktop", "[Desktop Entry]\nExec=/nonexistent/tool\n");
    writeText(m_Dirs.user / "kde.desktop", "[Desktop Entry]\nName=KDE only\nExec=/bin/sh\nOnlyShowIn=KDE;\n");
    writeText(m_Dirs.user / "link.desktop", "[Desktop Entry]\nType=Link\nURL=https://example.org\n");
    writeText(m_Dirs.user / "notes.txt", "not an autostart file");

    LinuxStartupProbe probe(m_Dirs, "GNOME", SEARCH_PATH);
    EXPECT_TRUE(probe.capabilities().canEnumerate);
    const std::vector<StartupEntry> entries = probe.enumerate();
    EXPECT_EQ(entries.size(), 4U) << "the link and the .txt aren't listed";

    const StartupEntry* shared = find(entries, "shared.desktop");
    ASSERT_NE(shared, nullptr);
    EXPECT_EQ(shared->name, "User copy");
    EXPECT_FALSE(shared->enabled);
    EXPECT_EQ(shared->location, StartupLocation::XdgAutostartSystem) << "a system entry, switched by the user's copy";
    EXPECT_EQ(shared->scope, StartupScope::Machine);
    EXPECT_EQ(shared->sourcePath, (m_Dirs.user / "shared.desktop").string());
    EXPECT_EQ(shared->target, StartupTargetState::Present);

    const StartupEntry* later = find(entries, "later.desktop");
    ASSERT_NE(later, nullptr);
    EXPECT_TRUE(later->enabled);
    EXPECT_EQ(later->executablePath.front(), '/') << later->executablePath << ": sh found on $PATH";

    const StartupEntry* mine = find(entries, "mine.desktop");
    ASSERT_NE(mine, nullptr);
    EXPECT_EQ(mine->name, "mine") << "no Name: the file's";
    EXPECT_EQ(mine->location, StartupLocation::XdgAutostartUser);
    EXPECT_EQ(mine->scope, StartupScope::User);
    EXPECT_EQ(mine->target, StartupTargetState::Missing);

    const StartupEntry* kde = find(entries, "kde.desktop");
    ASSERT_NE(kde, nullptr);
    EXPECT_FALSE(kde->enabled);
    EXPECT_EQ(kde->notSwitchableReason, "Starts only in KDE");
    EXPECT_FALSE(hasSwitchableState(*kde));
}

TEST_F(LinuxStartupTest, DisablingASystemEntryWritesAUserCopyAndEnablingRemovesIt)
{
    const std::string systemText = "[Desktop Entry]\nType=Application\nName=Updater\nExec=/bin/sh\n";
    writeText(m_Dirs.system[0] / "updater.desktop", systemText);
    LinuxStartupProbe probe(m_Dirs, "GNOME", SEARCH_PATH);
    LinuxStartupActions actions(m_Dirs);
    EXPECT_TRUE(actions.capabilities().canSetEnabled);
    EXPECT_FALSE(actions.capabilities().machineEntriesNeedElevation);

    const std::vector<StartupEntry> before = probe.enumerate();
    ASSERT_NE(find(before, "updater.desktop"), nullptr);
    ASSERT_TRUE(actions.setEnabled(*find(before, "updater.desktop"), false).ok);
    EXPECT_EQ(readText(m_Dirs.system[0] / "updater.desktop"), systemText) << "the system file is never written";
    EXPECT_TRUE(parsed(readText(m_Dirs.user / "updater.desktop")).hidden);
    EXPECT_FALSE(find(probe.enumerate(), "updater.desktop")->enabled);

    ASSERT_TRUE(actions.setEnabled(*find(probe.enumerate(), "updater.desktop"), true).ok);
    EXPECT_FALSE(std::filesystem::exists(m_Dirs.user / "updater.desktop")) << "a copy that only switched it goes";
    EXPECT_TRUE(find(probe.enumerate(), "updater.desktop")->enabled);
}

TEST_F(LinuxStartupTest, EnablingAnEditedCopyKeepsItWithHiddenFalse)
{
    writeText(m_Dirs.system[0] / "chat.desktop", "[Desktop Entry]\nName=Chat\nExec=/bin/sh\n");
    writeText(m_Dirs.user / "chat.desktop", "[Desktop Entry]\nName=Chat\nExec=/bin/sh --minimized\nHidden=true\n");
    LinuxStartupProbe probe(m_Dirs, "", SEARCH_PATH);
    LinuxStartupActions actions(m_Dirs);
    ASSERT_TRUE(actions.setEnabled(*find(probe.enumerate(), "chat.desktop"), true).ok);
    const std::string text = readText(m_Dirs.user / "chat.desktop");
    EXPECT_TRUE(text.contains("--minimized")) << "the user's own change is kept";
    EXPECT_TRUE(parsed(text).enabled());
}

TEST_F(LinuxStartupTest, AUserEntryIsSwitchedInPlace)
{
    writeText(m_Dirs.user / "own.desktop", "[Desktop Entry]\nName=Own\nExec=/bin/sh\nX-GNOME-Autostart-enabled=false\n");
    LinuxStartupProbe probe(m_Dirs, "GNOME", SEARCH_PATH);
    LinuxStartupActions actions(m_Dirs);
    EXPECT_FALSE(find(probe.enumerate(), "own.desktop")->enabled);
    ASSERT_TRUE(actions.setEnabled(*find(probe.enumerate(), "own.desktop"), true).ok);
    EXPECT_TRUE(find(probe.enumerate(), "own.desktop")->enabled);
    EXPECT_TRUE(readText(m_Dirs.user / "own.desktop").contains("X-GNOME-Autostart-enabled=true"));
}

// The usual way to hide a system entry is a stub with no Exec. Enabling it makes a working copy (here,
// since the stub is all the user has, removed: the system file applies again).
TEST_F(LinuxStartupTest, EnablingAStubWithoutExecBringsTheSystemEntryBack)
{
    writeText(m_Dirs.system[0] / "tray.desktop", "[Desktop Entry]\nName=Tray\nExec=/bin/sh\n");
    writeText(m_Dirs.user / "tray.desktop", "[Desktop Entry]\nHidden=true\n");
    LinuxStartupProbe probe(m_Dirs, "GNOME", SEARCH_PATH);
    const std::vector<StartupEntry> entries = probe.enumerate();
    const StartupEntry* listed = find(entries, "tray.desktop");
    ASSERT_NE(listed, nullptr);
    EXPECT_EQ(listed->name, "Tray") << "the system file's name, not the stub's file name";
    EXPECT_FALSE(listed->enabled);
    LinuxStartupActions actions(m_Dirs);
    ASSERT_TRUE(actions.setEnabled(*listed, true).ok);
    EXPECT_FALSE(std::filesystem::exists(m_Dirs.user / "tray.desktop"));
    EXPECT_TRUE(find(probe.enumerate(), "tray.desktop")->enabled);

    // A stub that only says Hidden=false still hides it: the session drops a file with no Exec.
    writeText(m_Dirs.user / "tray.desktop", "[Desktop Entry]\nHidden=false\n");
    EXPECT_FALSE(find(probe.enumerate(), "tray.desktop")->enabled);
}

TEST_F(LinuxStartupTest, SessionComponentsCantBeSwitched)
{
    writeText(m_Dirs.system[0] / "at-spi.desktop",
              "[Desktop Entry]\nName=AT-SPI\nExec=/bin/sh\nNoDisplay=true\nX-GNOME-Autostart-Phase=Initialization\n");
    LinuxStartupProbe probe(m_Dirs, "GNOME", SEARCH_PATH);
    const std::vector<StartupEntry> entries = probe.enumerate();
    const StartupEntry* entry = find(entries, "at-spi.desktop");
    ASSERT_NE(entry, nullptr);
    EXPECT_TRUE(entry->enabled);
    EXPECT_EQ(entry->notSwitchableReason, "Part of the desktop session");
}

TEST_F(LinuxStartupTest, TheProgramSkipsAssignmentsAndEnv)
{
    writeText(m_Dirs.user / "a.desktop", "[Desktop Entry]\nExec=LD_LIBRARY_PATH=/opt/x/lib /bin/sh --flag\n");
    writeText(m_Dirs.user / "b.desktop", "[Desktop Entry]\nExec=env -u X FOO=1 sh -c true\n");
    LinuxStartupProbe probe(m_Dirs, "", SEARCH_PATH);
    const std::vector<StartupEntry> entries = probe.enumerate();
    EXPECT_EQ(find(entries, "a.desktop")->executablePath, "/bin/sh");
    EXPECT_TRUE(find(entries, "b.desktop")->executablePath.ends_with("/sh")) << find(entries, "b.desktop")->executablePath;
    EXPECT_TRUE(find(entries, "a.desktop")->iconPath.empty()) << "the icon comes from the program";
}

TEST_F(LinuxStartupTest, ASymlinkedUserFileIsntReplaced)
{
    writeText(m_Root.path / "dotfiles/link.desktop", "[Desktop Entry]\nExec=/bin/sh\n");
    std::filesystem::create_directories(m_Dirs.user);
    std::error_code ec;
    std::filesystem::create_symlink(m_Root.path / "dotfiles/link.desktop", m_Dirs.user / "link.desktop", ec);
    if (ec)
    {
        GTEST_SKIP() << "no symlinks here";
    }
    LinuxStartupProbe probe(m_Dirs, "", SEARCH_PATH);
    LinuxStartupActions actions(m_Dirs);
    const std::vector<StartupEntry> entries = probe.enumerate();
    ASSERT_NE(find(entries, "link.desktop"), nullptr);
    const StartupActionResult result = actions.setEnabled(*find(entries, "link.desktop"), false);
    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(result.message.contains("symbolic link")) << result.message;
    EXPECT_TRUE(std::filesystem::is_symlink(m_Dirs.user / "link.desktop"));
}

TEST_F(LinuxStartupTest, ANameThatIsntAnAutostartFileIsRefused)
{
    LinuxStartupActions actions(m_Dirs);
    StartupEntry entry;
    entry.registrationName = "../escape.desktop";
    EXPECT_FALSE(actions.setEnabled(entry, false).ok);
    entry.registrationName = "missing.desktop";
    EXPECT_FALSE(actions.setEnabled(entry, false).ok);
    EXPECT_FALSE(std::filesystem::exists(m_Dirs.user / "missing.desktop"));
    EXPECT_FALSE(LinuxStartupActions(XdgAutostart::Dirs{}).capabilities().canSetEnabled) << "no HOME: nowhere to write";
}

// #3082: a unit file's description, first ExecStart (prefixes dropped), a timer's schedule and unit.
TEST(SystemdUnitFileTest, ReadsWhatTheStartupTabShows)
{
    const SystemdUnitFile::Unit service =
        SystemdUnitFile::parse("# a comment\n[Unit]\nDescription=Sync daemon\nAfter=network.target\n\n[Service]\nType=simple\n"
                               "ExecStart=-/usr/bin/syncd --quiet\nExecStart=/usr/bin/other\n[Install]\nWantedBy=default.target\n");
    EXPECT_EQ(service.description, "Sync daemon");
    EXPECT_EQ(service.execStart, "/usr/bin/syncd --quiet");
    EXPECT_TRUE(service.schedule.empty());

    const SystemdUnitFile::Unit timer =
        SystemdUnitFile::parse("[Unit]\nDescription=Nightly backup\n[Timer]\nOnCalendar=daily\nPersistent=true\nUnit=backup-job.service\n");
    EXPECT_EQ(timer.schedule, "OnCalendar=daily");
    EXPECT_EQ(timer.timerUnit, "backup-job.service");
    EXPECT_TRUE(SystemdUnitFile::parse("").description.empty());
}

TEST(SystemdUnitFileTest, TheDirectoriesFollowXdgConfigHome)
{
    const SystemdUserDirs defaults = systemdUserDirs("/home/u", "");
    EXPECT_EQ(defaults.user, std::filesystem::path("/home/u/.config/systemd/user"));
    EXPECT_EQ(defaults.system, (std::vector<std::filesystem::path>{"/etc/systemd/user"}));
    EXPECT_EQ(defaults.unitPath.front(), defaults.user);
    EXPECT_EQ(systemdUserDirs("/home/u", "/cfg").user, std::filesystem::path("/cfg/systemd/user"));
    EXPECT_TRUE(systemdUserDirs("", "relative").user.empty());
}

// #3082: enabled user units (services and timers) are listed read-only beside the autostart files; the user's
// link to a unit wins over the same unit enabled for every user; a timer shows the command its service runs.
TEST_F(LinuxStartupTest, ListsTheEnabledSystemdUserUnitsReadOnly)
{
    const std::filesystem::path user = m_Root.path / "home/.config/systemd/user";
    const std::filesystem::path system = m_Root.path / "etc/systemd/user";
    const std::filesystem::path lib = m_Root.path / "usr/lib/systemd/user";
    writeText(lib / "syncd.service", "[Unit]\nDescription=Sync daemon\n[Service]\nExecStart=/bin/sh -c true\n");
    writeText(lib / "backup.timer", "[Unit]\nDescription=Nightly backup\n[Timer]\nOnCalendar=daily\n");
    writeText(lib / "backup.service", "[Service]\nExecStart=/nonexistent/backup --all\n");
    std::filesystem::create_directories(user / "default.target.wants");
    std::filesystem::create_directories(user / "timers.target.wants");
    std::filesystem::create_directories(system / "default.target.wants");
    std::filesystem::create_symlink(lib / "syncd.service", user / "default.target.wants/syncd.service");
    std::filesystem::create_symlink(lib / "backup.timer", user / "timers.target.wants/backup.timer");
    std::filesystem::create_symlink(lib / "syncd.service", system / "default.target.wants/syncd.service");
    writeText(system / "default.target.wants/agent.service", "[Service]\nExecStart=/bin/sh\n");

    LinuxStartupProbe probe(
        m_Dirs, "GNOME", SEARCH_PATH, SystemdUserDirs{.user = user, .system = {system}, .unitPath = {user, system, lib}});
    const std::vector<StartupEntry> entries = probe.enumerate();
    ASSERT_EQ(entries.size(), 3U);

    const StartupEntry* syncd = find(entries, "syncd.service");
    ASSERT_NE(syncd, nullptr);
    EXPECT_EQ(syncd->name, "Sync daemon");
    EXPECT_EQ(syncd->command, "/bin/sh -c true");
    EXPECT_EQ(syncd->executablePath, "/bin/sh");
    EXPECT_EQ(syncd->location, StartupLocation::SystemdUserUnit) << "the user's link wins";
    EXPECT_TRUE(syncd->enabled);
    EXPECT_FALSE(syncd->notSwitchableReason.empty()) << "read-only for now";
    EXPECT_EQ(syncd->target, StartupTargetState::Present);

    const StartupEntry* backup = find(entries, "backup.timer");
    ASSERT_NE(backup, nullptr);
    EXPECT_EQ(backup->name, "Nightly backup (timer, OnCalendar=daily)");
    EXPECT_EQ(backup->command, "/nonexistent/backup --all") << "the command its service runs";
    EXPECT_EQ(backup->target, StartupTargetState::Missing);

    const StartupEntry* agent = find(entries, "agent.service");
    ASSERT_NE(agent, nullptr);
    EXPECT_EQ(agent->name, "agent");
    EXPECT_EQ(agent->location, StartupLocation::SystemdSystemUnit);
    EXPECT_EQ(agent->scope, StartupScope::Machine);

    // The actions refuse a unit: it isn't an autostart file.
    LinuxStartupActions actions(m_Dirs);
    EXPECT_FALSE(actions.setEnabled(*syncd, false).ok);
}

/// An entry for @p fileName, as the probe would hand it to setEnabled().
[[nodiscard]] StartupEntry entryNamed(const std::string& fileName)
{
    StartupEntry entry;
    entry.registrationName = fileName;
    return entry;
}

// #3221: setEnabled()'s refusals: no autostart folder, a name that isn't an autostart file, nothing to read, a
// file without its group, and switching on an entry that names no program anywhere.
TEST_F(LinuxStartupTest, SwitchingRefusesWhatItCantReadOrWrite)
{
    LinuxStartupActions homeless(XdgAutostart::Dirs{.user = {}, .system = {}});
    EXPECT_FALSE(homeless.capabilities().canSetEnabled);
    EXPECT_TRUE(homeless.setEnabled(entryNamed("a.desktop"), false).message.contains("no autostart folder"));

    LinuxStartupActions actions(m_Dirs);
    for (const char* name : {"", "../x.desktop", "notes.txt"})
    {
        EXPECT_EQ(actions.setEnabled(entryNamed(name), false).message, "This isn't an autostart file") << name;
    }
    EXPECT_TRUE(actions.setEnabled(entryNamed("gone.desktop"), false).message.contains("couldn't be read"));

    writeText(m_Dirs.user / "nogroup.desktop", "Name=No group\nExec=/bin/sh\n");
    EXPECT_TRUE(actions.setEnabled(entryNamed("nogroup.desktop"), false).message.contains("no [Desktop Entry] group"));

    writeText(m_Dirs.user / "noexec.desktop", "[Desktop Entry]\nName=Stub\nHidden=true\n");
    EXPECT_TRUE(actions.setEnabled(entryNamed("noexec.desktop"), true).message.contains("no Exec"))
        << "a stub with no system entry behind it";
}

// #3221: a folder TaskSmack can't make or write: a file where it should be, a read-only folder, and a copy
// that can't be removed.
TEST_F(LinuxStartupTest, AFolderItCantMakeOrWriteSaysWhich)
{
    writeText(m_Dirs.system[0] / "tool.desktop", "[Desktop Entry]\nName=Tool\nExec=/bin/sh\n");
    // A file where the user's autostart folder should be.
    writeText(m_Dirs.user.parent_path() / "autostart", "in the way\n");
    LinuxStartupActions actions(m_Dirs);
    EXPECT_TRUE(actions.setEnabled(entryNamed("tool.desktop"), false).message.contains("couldn't be made"));
    std::filesystem::remove(m_Dirs.user);

    if (::geteuid() == 0)
    {
        GTEST_SKIP() << "root writes into a read-only folder";
    }
    std::filesystem::create_directories(m_Dirs.user);
    std::filesystem::permissions(m_Dirs.user, std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec);
    EXPECT_TRUE(actions.setEnabled(entryNamed("tool.desktop"), false).message.contains("couldn't be written"));
    std::filesystem::permissions(m_Dirs.user, std::filesystem::perms::owner_all);

    // A copy that only switched the system entry off, in a folder it can't be removed from.
    ASSERT_TRUE(actions.setEnabled(entryNamed("tool.desktop"), false).ok);
    std::filesystem::permissions(m_Dirs.user, std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec);
    EXPECT_TRUE(actions.setEnabled(entryNamed("tool.desktop"), true).message.contains("couldn't be removed"));
    std::filesystem::permissions(m_Dirs.user, std::filesystem::perms::owner_all);

    // An unreadable user copy.
    std::filesystem::permissions(m_Dirs.user / "tool.desktop", std::filesystem::perms::none);
    EXPECT_TRUE(actions.setEnabled(entryNamed("tool.desktop"), true).message.contains("couldn't be read"));
    std::filesystem::permissions(m_Dirs.user / "tool.desktop", std::filesystem::perms::owner_all);
}

// #3221: an Exec with no program is Unresolved; a bare name not on the search path stays as written, Missing.
TEST_F(LinuxStartupTest, AnExecWithNoProgramIsUnresolvedAndAnUnknownNameMissing)
{
    writeText(m_Dirs.user / "empty.desktop", "[Desktop Entry]\nName=Empty\nExec=\n");
    writeText(m_Dirs.user / "unknown.desktop", "[Desktop Entry]\nName=Unknown\nExec=tasksmack-no-such-tool-3221 --x\n");
    LinuxStartupProbe probe(m_Dirs, "", SEARCH_PATH);
    const std::vector<StartupEntry> entries = probe.enumerate();
    const StartupEntry* empty = find(entries, "empty.desktop");
    ASSERT_NE(empty, nullptr);
    EXPECT_EQ(empty->target, StartupTargetState::Unresolved);
    const StartupEntry* unknown = find(entries, "unknown.desktop");
    ASSERT_NE(unknown, nullptr);
    EXPECT_EQ(unknown->executablePath, "tasksmack-no-such-tool-3221");
    EXPECT_EQ(unknown->target, StartupTargetState::Missing);
}

} // namespace
} // namespace Platform
