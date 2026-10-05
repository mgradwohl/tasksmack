#include "App/InstanceLock.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <system_error>

#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace App
{
namespace
{

/// A fresh directory per test, so parallel test processes never share a lock file.
class InstanceLockTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        std::random_device random;
        m_Dir = std::filesystem::temp_directory_path() / ("tasksmack_lock_" + std::to_string(random()) + std::to_string(random()));
        std::error_code ec;
        ASSERT_TRUE(std::filesystem::create_directories(m_Dir, ec)) << ec.message();
    }

    void TearDown() override
    {
        std::error_code ec;
        std::filesystem::remove_all(m_Dir, ec);
    }

    std::filesystem::path m_Dir;
};

TEST_F(InstanceLockTest, FirstInstanceAcquiresTheLock)
{
    const InstanceLock lock(m_Dir / "tasksmack.lock");
    EXPECT_EQ(lock.status(), InstanceLock::Status::Acquired);
    EXPECT_TRUE(lock.error().empty());
    EXPECT_TRUE(std::filesystem::exists(m_Dir / "tasksmack.lock"));
}

TEST_F(InstanceLockTest, SecondInstanceIsToldTaskSmackIsAlreadyRunning)
{
    // #1230: two instances saving one config could undo each other's settings change; the second
    // one must not start. A second lock in the same process opens its own file description, so it
    // conflicts exactly as another process's would.
    const InstanceLock first(m_Dir / "tasksmack.lock");
    ASSERT_EQ(first.status(), InstanceLock::Status::Acquired);

    const InstanceLock second(m_Dir / "tasksmack.lock");
    EXPECT_EQ(second.status(), InstanceLock::Status::HeldByAnotherInstance);
    EXPECT_TRUE(second.error().empty());
}

TEST_F(InstanceLockTest, LockIsReleasedWhenTheInstanceEnds)
{
    {
        const InstanceLock first(m_Dir / "tasksmack.lock");
        ASSERT_EQ(first.status(), InstanceLock::Status::Acquired);
    }
    const InstanceLock next(m_Dir / "tasksmack.lock");
    EXPECT_EQ(next.status(), InstanceLock::Status::Acquired);
}

TEST_F(InstanceLockTest, CreatesAMissingConfigDirectory)
{
    // First launch: the config directory doesn't exist yet.
    const auto nested = m_Dir / "new" / "tasksmack" / "tasksmack.lock";
    const InstanceLock lock(nested);
    EXPECT_EQ(lock.status(), InstanceLock::Status::Acquired);
    EXPECT_TRUE(std::filesystem::exists(nested));
}

TEST_F(InstanceLockTest, UnusableLockPathIsUnavailableNotAlreadyRunning)
{
    // A lock that can't be taken at all (here its directory is a file) must not be mistaken for a
    // running instance: TaskSmack logs it and starts anyway.
    const auto blocker = m_Dir / "not-a-dir";
    std::ofstream(blocker) << "a file, not a directory\n";
    ASSERT_TRUE(std::filesystem::is_regular_file(blocker));
    const InstanceLock lock(blocker / "tasksmack.lock");
    EXPECT_EQ(lock.status(), InstanceLock::Status::Unavailable);
    EXPECT_FALSE(lock.error().empty());
}

#ifndef _WIN32
TEST_F(InstanceLockTest, AnotherProcessIsToldTaskSmackIsAlreadyRunning)
{
    // The real two-instance case (#1230): a separate process can't take the lock while this one
    // holds it, and can once it is released.
    const auto path = m_Dir / "tasksmack.lock";
    const auto statusInChild = [&path]() -> int
    {
        const auto pid = ::fork();
        if (pid == 0)
        {
            const InstanceLock child(path);
            ::_exit(static_cast<int>(child.status()));
        }
        int status = 0;
        // NOLINTNEXTLINE(misc-include-cleaner) - WIFEXITED from sys/wait.h, include-cleaner false positive
        if (pid < 0 || ::waitpid(pid, &status, 0) != pid || !WIFEXITED(status))
        {
            return -1;
        }
        return WEXITSTATUS(status); // NOLINT(misc-include-cleaner) - WEXITSTATUS from sys/wait.h
    };

    {
        const InstanceLock parent(path);
        ASSERT_EQ(parent.status(), InstanceLock::Status::Acquired);
        EXPECT_EQ(statusInChild(), static_cast<int>(InstanceLock::Status::HeldByAnotherInstance));
    }
    EXPECT_EQ(statusInChild(), static_cast<int>(InstanceLock::Status::Acquired));
}
#endif

} // namespace
} // namespace App
