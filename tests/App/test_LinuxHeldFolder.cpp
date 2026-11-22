/// @file test_LinuxHeldFolder.cpp
/// @brief App::LinuxHeldFolder (#4167, #4257): a session run as root for a user reads and writes that user's config
/// folder only relative to the folder held, and reads only the owner's own plain files there. These tests run
/// unprivileged, as the folder's owner; giving what root makes to the user is UserConfigRootTest's (CI's root pass).

#ifdef __linux__

#include "App/InstanceLock.h"
#include "App/LinuxHeldFolder.h"
#include "Platform/ScopedTempDir.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace App
{
namespace
{

class LinuxHeldFolderTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        auto held = LinuxHeldFolder::hold(m_Dir.path / "cfg", ::geteuid());
        ASSERT_TRUE(held.has_value()) << held.error();
        m_Held = std::move(*held);
    }

    [[nodiscard]] std::filesystem::path in(const std::filesystem::path& name) const
    {
        return m_Dir.path / "cfg" / name;
    }

    Platform::TestSupport::ScopedTempDir m_Dir{"ts_linux_held_folder"};
    std::shared_ptr<LinuxHeldFolder> m_Held;
};

TEST_F(LinuxHeldFolderTest, HoldMakesTheFolderAndKnowsItsOwnerFromTheDescriptor)
{
    EXPECT_TRUE(std::filesystem::is_directory(m_Dir.path / "cfg"));
    EXPECT_EQ(m_Held->path(), m_Dir.path / "cfg");
    EXPECT_EQ(m_Held->owner().uid, ::geteuid());
    EXPECT_GE(m_Held->posixFolderFd(), 0);
}

TEST_F(LinuxHeldFolderTest, ReadsTheOwnersPlainFilesAndNothingThatIsntThere)
{
    std::ofstream(in("config.toml")) << "[ui]\nfont_size = 12\n";
    const auto text = m_Held->readFile("config.toml");
    ASSERT_TRUE(text.has_value()) << text.error();
    EXPECT_EQ(text->value_or(""), "[ui]\nfont_size = 12\n");

    const auto missing = m_Held->readFile("absent.toml");
    ASSERT_TRUE(missing.has_value());
    EXPECT_FALSE(missing->has_value());
    const auto noFolder = m_Held->readFile("themes/dark.toml");
    ASSERT_TRUE(noFolder.has_value()) << "a folder on the way that isn't there is a missing file, not an error";
    EXPECT_FALSE(noFolder->has_value());
}

// #4257: what the user plants is refused, not followed: a link to a file of root's, a hard link to another file, a FIFO
// (which would block the read for ever) and a file too large to be settings.
TEST_F(LinuxHeldFolderTest, RefusesALinkAHardLinkAFifoAndAnOversizedFile)
{
    const std::filesystem::path elsewhere = m_Dir.path / "elsewhere.toml";
    std::ofstream(elsewhere) << "secret = 1\n";

    std::filesystem::create_symlink(elsewhere, in("link.toml"));
    EXPECT_FALSE(m_Held->readFile("link.toml").has_value()) << "a link";

    std::filesystem::create_hard_link(elsewhere, in("hard.toml"));
    EXPECT_FALSE(m_Held->readFile("hard.toml").has_value()) << "a second name for another file";

    ASSERT_EQ(::mkfifo(in("fifo.toml").c_str(), S_IRUSR | S_IWUSR), 0);
    EXPECT_FALSE(m_Held->readFile("fifo.toml").has_value()) << "a FIFO is refused at once, not read";

    {
        std::ofstream big(in("big.toml"));
    }
    std::filesystem::resize_file(in("big.toml"), LinuxHeldFolder::MAX_FILE_BYTES + 1);
    EXPECT_FALSE(m_Held->readFile("big.toml").has_value()) << "too large";

    std::filesystem::create_directories(in("folder.toml"));
    EXPECT_FALSE(m_Held->readFile("folder.toml").has_value()) << "a folder";
}

// A link on the way inside the held folder is refused too: "themes" swapped for a link to another folder is never entered.
TEST_F(LinuxHeldFolderTest, ALinkedFolderOnTheWayIsNeverEntered)
{
    const std::filesystem::path other = m_Dir.path / "other";
    std::filesystem::create_directories(other);
    std::ofstream(other / "dark.toml") << "name = \"dark\"\n";
    std::filesystem::create_directory_symlink(other, in("themes"));

    EXPECT_FALSE(m_Held->readFile("themes/dark.toml").has_value());
    EXPECT_FALSE(m_Held->listFiles("themes").has_value());
}

TEST_F(LinuxHeldFolderTest, NamesThatClimbOrAreAbsoluteAreRefused)
{
    for (const char* name : {"../escape.toml", "/etc/passwd", "", "./config.toml", "themes/../config.toml"})
    {
        SCOPED_TRACE(name);
        EXPECT_FALSE(m_Held->readFile(name).has_value());
        EXPECT_FALSE(m_Held->replaceFile(name, "x").has_value());
    }
}

TEST_F(LinuxHeldFolderTest, ListsOnlyPlainFilesSorted)
{
    ASSERT_TRUE(m_Held->createFolder("themes").has_value());
    std::ofstream(in("themes/b.toml")) << "";
    std::ofstream(in("themes/a.toml")) << "";
    std::filesystem::create_directories(in("themes/sub"));
    std::filesystem::create_symlink(in("themes/a.toml"), in("themes/c.toml"));

    const auto names = m_Held->listFiles("themes");
    ASSERT_TRUE(names.has_value()) << names.error();
    EXPECT_EQ(*names, (std::vector<std::filesystem::path>{"a.toml", "b.toml"}));

    const auto none = m_Held->listFiles("nothing-here");
    ASSERT_TRUE(none.has_value());
    EXPECT_TRUE(none->empty());
}

TEST_F(LinuxHeldFolderTest, CreateFolderIsIdempotentAndRefusesAFileOrLinkInTheWay)
{
    ASSERT_TRUE(m_Held->createFolder("themes").has_value());
    EXPECT_TRUE(std::filesystem::is_directory(in("themes")));
    EXPECT_TRUE(m_Held->createFolder("themes").has_value()) << "already there";

    std::ofstream(in("afile")) << "";
    EXPECT_FALSE(m_Held->createFolder("afile").has_value());
    std::filesystem::create_directory_symlink(m_Dir.path, in("alink"));
    EXPECT_FALSE(m_Held->createFolder("alink").has_value());
}

TEST_F(LinuxHeldFolderTest, ReplaceFileWritesThroughTheFolderAndKeepsTheOldPermissions)
{
    ASSERT_TRUE(m_Held->replaceFile("config.toml", "first\n").has_value());
    EXPECT_EQ(m_Held->readFile("config.toml")->value_or(""), "first\n");

    std::filesystem::permissions(in("config.toml"), std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
    ASSERT_TRUE(m_Held->replaceFile("config.toml", "second\n").has_value());
    EXPECT_EQ(m_Held->readFile("config.toml")->value_or(""), "second\n");
    EXPECT_EQ(std::filesystem::status(in("config.toml")).permissions(),
              std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);

    // A link where the file goes is not replaced through, and its target is untouched.
    const std::filesystem::path target = m_Dir.path / "target";
    std::ofstream(target) << "keep";
    std::filesystem::create_symlink(target, in("linked.toml"));
    EXPECT_FALSE(m_Held->replaceFile("linked.toml", "x").has_value());
    std::ifstream kept(target);
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(kept), {}), "keep");
}

// The instance lock is opened through the folder held, and a second lock on the same folder is refused.
TEST_F(LinuxHeldFolderTest, TheInstanceLockIsTakenThroughTheFolderHeld)
{
    const InstanceLock first(*m_Held, ".tasksmack.lock");
    ASSERT_EQ(first.status(), InstanceLock::Status::Acquired) << first.error();
    EXPECT_TRUE(std::filesystem::is_regular_file(in(".tasksmack.lock")));
    const InstanceLock second(*m_Held, ".tasksmack.lock");
    EXPECT_EQ(second.status(), InstanceLock::Status::HeldByAnotherInstance);

    std::filesystem::create_symlink(m_Dir.path / "elsewhere.lock", in("linked.lock"));
    const InstanceLock linked(*m_Held, "linked.lock");
    EXPECT_EQ(linked.status(), InstanceLock::Status::Unavailable) << "a link where the lock goes is never followed";
    EXPECT_FALSE(std::filesystem::exists(m_Dir.path / "elsewhere.lock"));
}

// Held on a path that has a link of the user's on the way, the hold is refused as root's walk refuses it: openDirectoryForUser
// is asked for the less-privileged side's rule.
TEST(LinuxHeldFolderHoldTest, AFolderThatIsALinkIsNotHeld)
{
    const Platform::TestSupport::ScopedTempDir dir("ts_linux_held_link");
    std::filesystem::create_directories(dir.path / "real");
    std::filesystem::create_directory_symlink(dir.path / "real", dir.path / "cfg");
    const auto held = LinuxHeldFolder::hold(dir.path / "cfg", ::geteuid());
    EXPECT_FALSE(held.has_value()) << "a link of the user's in place of the folder";
}

// #4167, as root (CI's root pass, tools/common.sh's root_test_filter): a folder that is another account's gets what
// root makes in it (a folder, a replaced file, the lock), and a file of root's planted there is never read as theirs.
TEST(LinuxHeldFolderRootTest, WhatRootMakesIsTheOwnersAndRootsOwnFilesAreNotRead)
{
    if (::geteuid() != 0)
    {
        GTEST_SKIP() << "needs root: CI's root pass runs it";
    }
    constexpr uid_t NOBODY = 65534;
    constexpr gid_t NOGROUP = 65534;
    const Platform::TestSupport::ScopedTempDir dir("ts_linux_held_root");
    const std::filesystem::path cfg = dir.path / "cfg";
    std::filesystem::create_directories(cfg);
    ASSERT_EQ(::chown(cfg.c_str(), NOBODY, NOGROUP), 0);
    auto held = LinuxHeldFolder::hold(cfg, 0);
    ASSERT_TRUE(held.has_value()) << held.error();
    EXPECT_EQ((*held)->owner().uid, NOBODY);

    const auto ownerOf = [](const std::filesystem::path& path)
    {
        struct stat info = {};
        return ::lstat(path.c_str(), &info) == 0 ? info.st_uid : static_cast<uid_t>(-1);
    };
    ASSERT_TRUE((*held)->createFolder("themes").has_value());
    EXPECT_EQ(ownerOf(cfg / "themes"), NOBODY);
    ASSERT_TRUE((*held)->replaceFile("config.toml", "a = 1\n").has_value());
    EXPECT_EQ(ownerOf(cfg / "config.toml"), NOBODY);
    EXPECT_EQ((*held)->readFile("config.toml")->value_or(""), "a = 1\n");
    {
        const InstanceLock lock(**held, ".tasksmack.lock");
        ASSERT_EQ(lock.status(), InstanceLock::Status::Acquired) << lock.error();
        EXPECT_EQ(ownerOf(cfg / ".tasksmack.lock"), NOBODY);
    }

    std::ofstream(cfg / "roots.toml") << "secret = 1\n"; // root's, in their folder
    EXPECT_FALSE((*held)->readFile("roots.toml").has_value()) << "not the folder owner's";
}

// #4805, as root: a third account's file renamed in as the lock is never locked or handed to the folder's owner, by the
// held folder or the path-based lock; root's own one (an older root run's) still is.
TEST(LinuxHeldFolderRootTest, ALockFileOfAnotherAccountIsNeverHandedOver)
{
    if (::geteuid() != 0)
    {
        GTEST_SKIP() << "needs root: CI's root pass runs it";
    }
    constexpr uid_t NOBODY = 65534;
    constexpr gid_t NOGROUP = 65534;
    constexpr uid_t THIRD = 1; // daemon: neither the folder's owner nor root
    const Platform::TestSupport::ScopedTempDir dir("ts_linux_held_third");
    const std::filesystem::path cfg = dir.path / "cfg";
    std::filesystem::create_directories(cfg);
    ASSERT_EQ(::chown(cfg.c_str(), NOBODY, NOGROUP), 0);
    const auto ownerOf = [](const std::filesystem::path& path)
    {
        struct stat info = {};
        return ::lstat(path.c_str(), &info) == 0 ? info.st_uid : static_cast<uid_t>(-1);
    };

    std::ofstream(cfg / "tasksmack.lock") << "the third account's\n";
    ASSERT_EQ(::chown((cfg / "tasksmack.lock").c_str(), THIRD, THIRD), 0);
    auto held = LinuxHeldFolder::hold(cfg, 0);
    ASSERT_TRUE(held.has_value()) << held.error();
    {
        const InstanceLock lock(**held, "tasksmack.lock");
        EXPECT_NE(lock.status(), InstanceLock::Status::Acquired);
        EXPECT_TRUE(lock.error().contains("belongs to another account")) << lock.error();
    }
    EXPECT_EQ(ownerOf(cfg / "tasksmack.lock"), THIRD) << "not handed to the folder's owner";
    {
        const InstanceLock lock(cfg / "tasksmack.lock");
        EXPECT_NE(lock.status(), InstanceLock::Status::Acquired);
        EXPECT_TRUE(lock.error().contains("belongs to another account")) << lock.error();
    }
    EXPECT_EQ(ownerOf(cfg / "tasksmack.lock"), THIRD) << "nor by the path-based lock";

    // Root's own, as an older root run left it: still reclaimed for the folder's owner (#2437).
    ASSERT_EQ(::chown((cfg / "tasksmack.lock").c_str(), 0, 0), 0);
    {
        const InstanceLock lock(**held, "tasksmack.lock");
        ASSERT_EQ(lock.status(), InstanceLock::Status::Acquired) << lock.error();
    }
    EXPECT_EQ(ownerOf(cfg / "tasksmack.lock"), NOBODY);
}

} // namespace
} // namespace App

#endif // __linux__
