/// @file test_ProcMapsParser.cpp
/// @brief Platform::ProcMaps (#802): /proc/[pid]/maps lines and whole files as loaded modules. Pure
/// text, so built and run on every platform.

#include "Platform/IProcessModules.h"
#include "Platform/Linux/ProcMapsParser.h"

#include <gtest/gtest.h>

#include <initializer_list>
#include <optional>
#include <string_view>
#include <vector>

namespace Platform::ProcMaps
{
namespace
{

// A real-looking maps file: the executable, libc (four segments, one non-contiguous), a data-only
// mapping, anonymous and kernel mappings, a deleted library, and a path with spaces.
constexpr std::string_view MAPS = "55d0c1a00000-55d0c1a02000 r--p 00000000 08:01 1311 /usr/bin/cat\n"
                                  "55d0c1a02000-55d0c1a07000 r-xp 00002000 08:01 1311 /usr/bin/cat\n"
                                  "55d0c2b00000-55d0c2b21000 rw-p 00000000 00:00 0                          [heap]\n"
                                  "7f3a1c000000-7f3a1c028000 r--p 00000000 08:01 1835 /usr/lib/x86_64-linux-gnu/libc.so.6\n"
                                  "7f3a1c028000-7f3a1c1bd000 r-xp 00028000 08:01 1835 /usr/lib/x86_64-linux-gnu/libc.so.6\n"
                                  "7f3a1c1bd000-7f3a1c215000 r--p 001bd000 08:01 1835 /usr/lib/x86_64-linux-gnu/libc.so.6\n"
                                  "7f3a1c215000-7f3a1c219000 rw-p 00000000 00:00 0 \n"
                                  "7f3a1d000000-7f3a1d300000 r--p 00000000 08:01 2001 /usr/lib/locale/locale-archive\n"
                                  "7f3a1e000000-7f3a1e010000 r-xp 00000000 08:01 3003 /opt/app/libold.so (deleted)\n"
                                  "7f3a1e100000-7f3a1e104000 r-xp 00000000 08:01 3004 /opt/My App/lib plugin.so\n"
                                  "7f3a1c300000-7f3a1c304000 rw-p 00219000 08:01 1835 /usr/lib/x86_64-linux-gnu/libc.so.6\n"
                                  "7ffd5a1f0000-7ffd5a211000 rw-p 00000000 00:00 0                          [stack]\n"
                                  "7ffd5a3fb000-7ffd5a3fd000 r-xp 00000000 00:00 0                          [vdso]\n";

TEST(ProcMapsParserTest, ParsesALineWithItsPathname)
{
    const std::optional<MapsLine> parsed = parseMapsLine("7f3a1c028000-7f3a1c1bd000 r-xp 00028000 08:01 1835   /usr/lib/libc.so.6");
    ASSERT_TRUE(parsed.has_value());
    const MapsLine line = parsed.value_or(MapsLine{});
    EXPECT_EQ(line.start, 0x7f3a1c028000U);
    EXPECT_EQ(line.end, 0x7f3a1c1bd000U);
    EXPECT_TRUE(line.executable);
    EXPECT_EQ(line.inode, 1835U);
    EXPECT_EQ(line.pathname, "/usr/lib/libc.so.6");
}

TEST(ProcMapsParserTest, AnonymousLineHasNoPathname)
{
    const std::optional<MapsLine> parsed = parseMapsLine("7f3a1c215000-7f3a1c219000 rw-p 00000000 00:00 0");
    ASSERT_TRUE(parsed.has_value());
    const MapsLine line = parsed.value_or(MapsLine{});
    EXPECT_FALSE(line.executable);
    EXPECT_EQ(line.inode, 0U);
    EXPECT_TRUE(line.pathname.empty());
}

TEST(ProcMapsParserTest, RejectsTruncatedAndMalformedLines)
{
    for (const std::string_view bad : {"",
                                       "7f3a1c028000",
                                       "7f3a1c028000-",
                                       "7f3a1c028000-7f3a1c1bd000",
                                       "7f3a1c028000-7f3a1c1bd000 r-x",
                                       "7f3a1c028000-7f3a1c1bd000 r-xp 0002",
                                       "7f3a1c028000-7f3a1c1bd000 r-xp 00028000 0801 1835 /x",
                                       "7f3a1c028000-7f3a1c1bd000 r-xp 00028000 08:01",
                                       "7f3a1c028000-7f3a1c1bd000 r-xp 00028000 08:01 18z5 /x",
                                       "7f3a1c1bd000-7f3a1c028000 r-xp 00028000 08:01 1835 /x", // end before start
                                       "fffffffffffffffff-1 r-xp 0 08:01 1 /x"})                // overflows
    {
        EXPECT_FALSE(parseMapsLine(bad).has_value()) << bad;
    }
}

TEST(ProcMapsParserTest, GroupsExecutableFilesByPathname)
{
    const std::vector<ProcessModule> modules = parseProcMaps(MAPS);
    ASSERT_EQ(modules.size(), 4U);

    EXPECT_EQ(modules[0].path, "/usr/bin/cat");
    EXPECT_EQ(modules[0].baseAddress, 0x55d0c1a00000U);
    EXPECT_EQ(modules[0].sizeBytes, 0x7000U);
    EXPECT_FALSE(modules[0].version.has_value());

    // Four segments, one far from the others: the base is the lowest start, the size the sum of the spans.
    EXPECT_EQ(modules[1].path, "/usr/lib/x86_64-linux-gnu/libc.so.6");
    EXPECT_EQ(modules[1].baseAddress, 0x7f3a1c000000U);
    EXPECT_EQ(modules[1].sizeBytes, 0x215000U + 0x4000U);
    EXPECT_FALSE(modules[1].deleted);

    EXPECT_EQ(modules[2].path, "/opt/app/libold.so");
    EXPECT_TRUE(modules[2].deleted);

    EXPECT_EQ(modules[3].path, "/opt/My App/lib plugin.so");
    EXPECT_EQ(modules[3].sizeBytes, 0x4000U);
}

TEST(ProcMapsParserTest, DeletedFileAndItsReplacementAreSeparateModules)
{
    const std::vector<ProcessModule> modules = parseProcMaps("1000-2000 r-xp 0 08:01 7 /lib/a.so (deleted)\n"
                                                             "3000-4000 r-xp 0 08:01 8 /lib/a.so\n");
    ASSERT_EQ(modules.size(), 2U);
    EXPECT_TRUE(modules[0].deleted);
    EXPECT_FALSE(modules[1].deleted);
    EXPECT_EQ(modules[0].path, modules[1].path);
}

TEST(ProcMapsParserTest, SkipsATruncatedLastLine)
{
    // A read cut mid-line: the complete lines still count, the cut one does not.
    const std::vector<ProcessModule> modules = parseProcMaps("1000-2000 r-xp 0 08:01 7 /lib/a.so\n3000-40");
    ASSERT_EQ(modules.size(), 1U);
    EXPECT_EQ(modules[0].path, "/lib/a.so");
    EXPECT_TRUE(parseProcMaps("").empty());
    EXPECT_TRUE(parseProcMaps("\n\n").empty());
}

TEST(ProcMapsParserTest, SkipsNonFileMappings)
{
    // Named but not files: kernel areas, anonymous names and anon inodes, even when executable.
    const std::vector<ProcessModule> modules = parseProcMaps("1000-2000 r-xp 0 00:00 0 [vdso]\n"
                                                             "2000-3000 r-xp 0 00:00 0 [anon:jit]\n"
                                                             "3000-4000 r-xp 0 00:0e 99 anon_inode:[perf_event]\n");
    EXPECT_TRUE(modules.empty());
}

} // namespace
} // namespace Platform::ProcMaps
