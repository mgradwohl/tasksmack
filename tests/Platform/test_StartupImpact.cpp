/// @file test_StartupImpact.cpp
/// @brief A startup entry's share of a sign-in's measurement (#3073): matching its program's path to
/// the recorded processes (case, separators, quotes, long-path and UNC prefixes) and adding up its
/// process tree, each process once, PIDs told apart by their start times.

#include "Platform/IStartupProbe.h"
#include "Platform/StartupImpact.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>

namespace Platform
{
namespace
{

TEST(StartupImpactPathTest, ComparesPathsAsWindowsDoes)
{
    EXPECT_TRUE(sameProgramPath(R"(C:\Program Files\A\a.exe)", R"(c:\program files\a\A.EXE)"));
    EXPECT_TRUE(sameProgramPath(R"(C:\Program Files\A\a.exe)", "C:/Program Files/A/a.exe"));
    EXPECT_TRUE(sameProgramPath(R"("C:\Program Files\A\a.exe")", R"( C:\Program Files\A\a.exe )"));
    EXPECT_TRUE(sameProgramPath(R"(\\?\C:\Program Files\A\a.exe)", R"(C:\Program Files\A\a.exe)"));
    EXPECT_TRUE(sameProgramPath(R"(\??\C:\Windows\System32\conhost.exe)", R"(C:\Windows\System32\conhost.exe)"));
    EXPECT_TRUE(sameProgramPath(R"(\\?\UNC\server\share\a.exe)", R"(\\server\share\a.exe)"));
    EXPECT_TRUE(sameProgramPath(R"(\\server\share\a.exe)", "//server/share/a.exe"));

    EXPECT_FALSE(sameProgramPath(R"(C:\Program Files\A\a.exe)", R"(C:\Program Files\A\b.exe)"));
    EXPECT_FALSE(sameProgramPath(R"(C:\Program Files\A\a.exe)", R"(C:\Program Files (x86)\A\a.exe)"));
    EXPECT_FALSE(sameProgramPath(R"(C:\PROGRA~1\A\a.exe)", R"(C:\Program Files\A\a.exe)")) << "short names aren't resolved";
    EXPECT_FALSE(sameProgramPath("", ""));
    EXPECT_FALSE(sameProgramPath(R"("")", R"("")"));
    // Only ASCII letters fold: "É" and "é" are different bytes.
    EXPECT_FALSE(sameProgramPath("C:\\\xC3\x89.exe", "C:\\\xC3\xA9.exe"));

    EXPECT_EQ(comparableProgramPath(R"(\\?\UNC\server\x)"), R"(\server\x)");
    EXPECT_EQ(comparableProgramPath(R"(\\?\C:\x)"), R"(C:\x)");
    EXPECT_EQ(comparableProgramPath(R"("C:\x")"), R"(C:\x)");
}

/// A recorded process: its path, PID and start second, its parent's PID and start second, its cost.
[[nodiscard]] StartupImpactProcess recordedProcess(const char* path,
                                                   std::uint32_t pid,
                                                   std::uint64_t start,
                                                   std::uint32_t parentPid,
                                                   std::uint64_t parentStart,
                                                   std::uint64_t cpuUs,
                                                   std::uint64_t diskBytes)
{
    return {.path = path,
            .pid = pid,
            .parentPid = parentPid,
            .startTicks = start,
            .parentStartTicks = parentStart,
            .cpuUs = cpuUs,
            .diskBytes = diskBytes};
}

TEST(StartupImpactMeasureTest, AddsUpTheProgramsProcessesAndWhatTheyStarted)
{
    StartupImpactReading reading;
    reading.processes = {
        recordedProcess(R"(C:\Windows\explorer.exe)", 100, 1, 50, 0, 1, 1),
        recordedProcess(R"(C:\App\app.exe)", 200, 2, 100, 1, 1000, 10),    // started by explorer: the program
        recordedProcess(R"(C:\App\helper.exe)", 201, 3, 200, 2, 500, 5),   // its child
        recordedProcess(R"(C:\Tools\tool.exe)", 202, 4, 201, 3, 100, 1),   // its grandchild
        recordedProcess(R"(C:\App\app.exe)", 203, 5, 200, 2, 50, 2),       // a second app.exe, its own child: counted once
        recordedProcess(R"(C:\App\app.exe)", 300, 6, 100, 1, 7, 7),        // another instance, from explorer
        recordedProcess(R"(C:\Other\other.exe)", 201, 7, 999, 3, 9999, 9), // PID 201 again, another parent start: not its child
        recordedProcess(R"(C:\App\app.exe)", 200, 2, 100, 1, 1000, 10),    // recorded twice: counted once
    };
    const StartupImpactTotals app = measureStartupProgram(reading, R"(c:\app\APP.exe)");
    EXPECT_EQ(app.processCount, 5U);
    EXPECT_EQ(app.cpuUs, 1000U + 500U + 100U + 50U + 7U);
    EXPECT_EQ(app.diskBytes, 10U + 5U + 1U + 2U + 7U);

    const StartupImpactTotals tool = measureStartupProgram(reading, R"(C:\Tools\tool.exe)");
    EXPECT_EQ(tool.processCount, 1U);
    EXPECT_EQ(tool.cpuUs, 100U);

    const StartupImpactTotals missing = measureStartupProgram(reading, R"(C:\Nowhere\none.exe)");
    EXPECT_EQ(missing.processCount, 0U);
    EXPECT_EQ(missing.cpuUs, 0U);
    EXPECT_EQ(measureStartupProgram(reading, "").processCount, 0U) << "an entry whose program is unknown";
    EXPECT_EQ(measureStartupProgram(StartupImpactReading{}, R"(C:\App\app.exe)").processCount, 0U);
}

TEST(StartupImpactMeasureTest, APlatformWithoutMeasurementsGivesNone)
{
    UnsupportedStartupProbe probe;
    EXPECT_FALSE(probe.capabilities().hasImpact);
    const StartupImpactReading reading = probe.readImpact();
    EXPECT_TRUE(reading.processes.empty());
    EXPECT_TRUE(reading.unavailableReason.empty());
}

} // namespace
} // namespace Platform
