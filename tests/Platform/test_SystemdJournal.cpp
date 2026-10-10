/// @file test_SystemdJournal.cpp
/// @brief Platform::SystemdJournal (#1674) against this machine: with libsystemd and a readable journal,
/// the systemd-coredump entries read (there may be none), each naming a process; otherwise the read says
/// why and the test is skipped.

#include "Platform/Linux/LinuxCoredumps.h"
#include "Platform/Linux/SystemdJournal.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <optional>
#include <string>

namespace Platform
{
namespace
{

TEST(SystemdJournalTest, ReadsThisMachinesCoredumpEntriesOrSaysWhyNot)
{
    const LinuxCoredumps::JournalReader reader = SystemdJournal::makeCoredumpJournalReader();
    ASSERT_TRUE(reader);
    constexpr std::size_t MAX = 5;
    const LinuxCoredumps::JournalRead read = reader(0, MAX);
    if (!read.opened)
    {
        EXPECT_FALSE(read.error.empty());
        GTEST_SKIP() << "The journal isn't readable here: " << read.error;
    }
    EXPECT_LE(read.entries.size(), MAX);
    for (const LinuxCoredumps::JournalFields& fields : read.entries)
    {
        std::string coreFile;
        // Every systemd-coredump entry names its process.
        EXPECT_TRUE(LinuxCoredumps::crashFromJournal(fields, coreFile).has_value());
    }
}

} // namespace
} // namespace Platform
