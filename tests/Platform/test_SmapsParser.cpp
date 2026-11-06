/// @file test_SmapsParser.cpp
/// @brief Platform::Smaps::parse() (#3089): /proc/[pid]/smaps text as memory mappings, from fixture text.
/// Pure, so it runs on every platform.

#include "Platform/IProcessMemoryMap.h"
#include "Platform/Linux/SmapsParser.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Platform
{
namespace
{

/// One smaps entry: @p header, then each size line with @p kb for every size.
std::string entry(std::string_view header, std::uint64_t kb)
{
    std::string text(header);
    text += '\n';
    const std::string value = std::to_string(kb) + " kB\n";
    for (const char* key : {"Size",
                            "KernelPageSize",
                            "MMUPageSize",
                            "Rss",
                            "Pss",
                            "Pss_Dirty",
                            "Shared_Clean",
                            "Shared_Dirty",
                            "Private_Clean",
                            "Private_Dirty",
                            "Referenced",
                            "Anonymous",
                            "LazyFree",
                            "AnonHugePages",
                            "Swap",
                            "SwapPss",
                            "Locked"})
    {
        text += std::string(key) + ":" + std::string(16 - std::string_view(key).size(), ' ') + value;
    }
    text += "THPeligible:    0\nVmFlags: rd ex mr mw me\n";
    return text;
}

constexpr std::string_view LIBC_TEXT = "7f3a1c028000-7f3a1c1bd000 r-xp 00028000 08:01 1835   /usr/lib/libc.so.6";

TEST(SmapsParserTest, ReadsAMappingsRangeProtectionOffsetPathAndSizes)
{
    const std::string text = std::string(LIBC_TEXT) + "\n" +
                             "Size:               1620 kB\n"
                             "Rss:                1200 kB\n"
                             "Pss:                  40 kB\n"
                             "Shared_Clean:       1196 kB\n"
                             "Shared_Dirty:          0 kB\n"
                             "Private_Clean:         4 kB\n"
                             "Private_Dirty:         0 kB\n"
                             "Swap:                  8 kB\n"
                             "SwapPss:               2 kB\n"
                             "VmFlags: rd ex mr mw me sd\n";
    const std::vector<MemoryMapping> mappings = Smaps::parse(text);
    ASSERT_EQ(mappings.size(), 1U);
    const MemoryMapping& libc = mappings[0];
    EXPECT_EQ(libc.start, 0x7f3a1c028000U);
    EXPECT_EQ(libc.end, 0x7f3a1c1bd000U);
    EXPECT_EQ(libc.sizeBytes(), 0x195000U);
    EXPECT_TRUE(libc.readable);
    EXPECT_FALSE(libc.writable);
    EXPECT_TRUE(libc.executable);
    EXPECT_FALSE(libc.shared);
    EXPECT_FALSE(libc.guard);
    EXPECT_FALSE(libc.reserved);
    EXPECT_EQ(libc.fileOffset, 0x28000U);
    EXPECT_EQ(libc.kind, MemoryMappingKind::Image);
    EXPECT_EQ(libc.path, "/usr/lib/libc.so.6");
    EXPECT_FALSE(libc.deleted);
    EXPECT_EQ(libc.residentBytes, 1200U * 1024U);
    EXPECT_EQ(libc.pssBytes, 40U * 1024U);
    EXPECT_EQ(libc.sharedCleanBytes, 1196U * 1024U);
    EXPECT_EQ(libc.sharedDirtyBytes, 0U);
    EXPECT_EQ(libc.privateCleanBytes, 4U * 1024U);
    EXPECT_EQ(libc.privateDirtyBytes, 0U);
    EXPECT_EQ(libc.swapBytes, 8U * 1024U);
    EXPECT_EQ(libc.swapPssBytes, 2U * 1024U);
}

TEST(SmapsParserTest, EachPathnameGivesItsKind)
{
    const std::string text = entry("55d0c1a00000-55d0c1a02000 r--p 00000000 08:01 1311 /usr/bin/server", 1) +
                             entry("55d0c1a02000-55d0c1a04000 r-xp 00002000 08:01 1311 /usr/bin/server", 1) +
                             entry("55d0c2000000-55d0c2021000 rw-p 00000000 00:00 0      [heap]", 1) +
                             entry("7f3a00000000-7f3a00021000 rw-p 00000000 00:00 0", 1) +
                             entry("7f3a10000000-7f3a10100000 r--p 00000000 08:01 77   /usr/lib/locale/locale-archive", 1) +
                             entry("7f3a20000000-7f3a20001000 rw-s 00000000 00:0f 1029 anon_inode:[io_uring]", 1) +
                             entry("7f3a30000000-7f3a30001000 rw-p 00000000 00:00 0    [anon:libc_malloc]", 1) +
                             entry("7ffd5a1f0000-7ffd5a211000 rw-p 00000000 00:00 0    [stack]", 1) +
                             entry("7ffd5a3f0000-7ffd5a3f4000 r--p 00000000 00:00 0    [vvar]", 1) +
                             entry("7ffd5a3f4000-7ffd5a3f6000 r-xp 00000000 00:00 0    [vdso]", 1) +
                             entry("ffffffffff600000-ffffffffff601000 --xp 00000000 00:00 0  [vsyscall]", 1);
    const std::vector<MemoryMapping> mappings = Smaps::parse(text);
    ASSERT_EQ(mappings.size(), 11U);
    // The data mapping of an executable file is part of its image, as the Modules section groups them.
    EXPECT_EQ(mappings[0].kind, MemoryMappingKind::Image);
    EXPECT_FALSE(mappings[0].executable);
    EXPECT_EQ(mappings[1].kind, MemoryMappingKind::Image);
    EXPECT_EQ(mappings[2].kind, MemoryMappingKind::Heap);
    EXPECT_EQ(mappings[2].path, "[heap]");
    EXPECT_EQ(mappings[3].kind, MemoryMappingKind::Anonymous);
    EXPECT_EQ(mappings[3].path, "");
    EXPECT_EQ(mappings[4].kind, MemoryMappingKind::File) << "mapped for data only";
    EXPECT_EQ(mappings[5].kind, MemoryMappingKind::Other);
    EXPECT_EQ(mappings[5].path, "anon_inode:[io_uring]");
    EXPECT_TRUE(mappings[5].shared);
    EXPECT_EQ(mappings[6].kind, MemoryMappingKind::Other);
    EXPECT_EQ(mappings[6].path, "[anon:libc_malloc]");
    EXPECT_EQ(mappings[7].kind, MemoryMappingKind::Stack);
    EXPECT_EQ(mappings[8].kind, MemoryMappingKind::System);
    EXPECT_EQ(mappings[9].kind, MemoryMappingKind::System);
    // [vsyscall]: the kernel's page in every process, at the top of the address space, execute-only.
    EXPECT_EQ(mappings[10].kind, MemoryMappingKind::System);
    EXPECT_EQ(mappings[10].start, 0xffffffffff600000U);
    EXPECT_FALSE(mappings[10].readable);
    EXPECT_TRUE(mappings[10].executable);
    EXPECT_EQ(mappings[10].residentBytes, 1024U);
}

TEST(SmapsParserTest, ADeletedFileKeepsItsPathWithoutTheSuffixAndStaysApartFromTheFileNowThere)
{
    const std::string text = entry("7f3a1c000000-7f3a1c001000 r-xp 00000000 08:01 1835 /usr/lib/libfoo.so (deleted)", 4) +
                             entry("7f3a1d000000-7f3a1d001000 r--p 00000000 08:01 1999 /usr/lib/libfoo.so", 4) +
                             entry("7f3a1e000000-7f3a1e001000 rw-s 00000000 00:01 2048 /memfd:jit (deleted)", 4);
    const std::vector<MemoryMapping> mappings = Smaps::parse(text);
    ASSERT_EQ(mappings.size(), 3U);
    EXPECT_EQ(mappings[0].path, "/usr/lib/libfoo.so");
    EXPECT_TRUE(mappings[0].deleted);
    EXPECT_EQ(mappings[0].kind, MemoryMappingKind::Image);
    // The file now at that path has no executable mapping of its own.
    EXPECT_EQ(mappings[1].path, "/usr/lib/libfoo.so");
    EXPECT_FALSE(mappings[1].deleted);
    EXPECT_EQ(mappings[1].kind, MemoryMappingKind::File);
    EXPECT_EQ(mappings[2].path, "/memfd:jit");
    EXPECT_TRUE(mappings[2].deleted);
    EXPECT_TRUE(mappings[2].shared);
}

TEST(SmapsParserTest, APathWithSpacesIsKeptWhole)
{
    const std::vector<MemoryMapping> mappings =
        Smaps::parse(entry("7f3a1c000000-7f3a1c001000 r--p 00001000 08:01 42   /home/al ice/My Files/data  file.bin", 1));
    ASSERT_EQ(mappings.size(), 1U);
    EXPECT_EQ(mappings[0].path, "/home/al ice/My Files/data  file.bin");
    EXPECT_EQ(mappings[0].fileOffset, 0x1000U);
    EXPECT_EQ(mappings[0].kind, MemoryMappingKind::File);
}

// SwapPss arrived in Linux 4.3, and a field the kernel doesn't write is unmeasured, not zero.
TEST(SmapsParserTest, AMissingSizeIsAbsentNotZero)
{
    const std::vector<MemoryMapping> mappings = Smaps::parse("7f3a1c000000-7f3a1c001000 rw-p 00000000 00:00 0\n"
                                                             "Size:                  4 kB\n"
                                                             "Rss:                   4 kB\n"
                                                             "Pss:                   4 kB\n"
                                                             "Swap:                  0 kB\n");
    ASSERT_EQ(mappings.size(), 1U);
    EXPECT_EQ(mappings[0].residentBytes, 4096U);
    EXPECT_EQ(mappings[0].swapBytes, 0U);
    EXPECT_FALSE(mappings[0].swapPssBytes.has_value());
    EXPECT_FALSE(mappings[0].privateCleanBytes.has_value());
    EXPECT_FALSE(mappings[0].sharedDirtyBytes.has_value());
}

TEST(SmapsParserTest, AHugeValueIsReadAndOneThatOverflowsInBytesIsAbsent)
{
    const std::uint64_t biggestKb = std::numeric_limits<std::uint64_t>::max() / 1024U;
    const std::string text = "0-ffffffffffff rw-p 00000000 00:00 0\n"
                             "Rss:       " +
                             std::to_string(biggestKb) +
                             " kB\n"
                             "Pss:       18446744073709551615 kB\n"
                             "Swap:      99999999999999999999999 kB\n"
                             "Private_Dirty: 12x kB\n"
                             "Private_Clean: 7\n";
    const std::vector<MemoryMapping> mappings = Smaps::parse(text);
    ASSERT_EQ(mappings.size(), 1U);
    EXPECT_EQ(mappings[0].sizeBytes(), 0xffffffffffffU);
    EXPECT_EQ(mappings[0].residentBytes, biggestKb * 1024U);
    EXPECT_FALSE(mappings[0].pssBytes.has_value()) << "past UINT64_MAX in bytes";
    EXPECT_FALSE(mappings[0].swapBytes.has_value()) << "past UINT64_MAX in kB";
    EXPECT_FALSE(mappings[0].privateDirtyBytes.has_value()) << "not a number";
    EXPECT_FALSE(mappings[0].privateCleanBytes.has_value()) << "no unit";
}

// A broken header's sizes are dropped with it, never given to the mapping before it; a size line before any
// header, a truncated last line and blank lines are skipped.
TEST(SmapsParserTest, AMalformedHeaderDropsItsSizesAndStrayLinesAreSkipped)
{
    const std::string text = "Rss:  99 kB\n"
                             "\n" +
                             entry("1000-2000 r--p 00000000 08:01 5 /a", 1) + entry("3000-zzzz r--p 00000000 08:01 5 /b", 7) +
                             entry("5000-4000 r--p 00000000 08:01 5 /c", 7) + entry("6000-7000 rw-p 00000000 00:00 0", 2) + "8000-90";
    const std::vector<MemoryMapping> mappings = Smaps::parse(text);
    ASSERT_EQ(mappings.size(), 2U);
    EXPECT_EQ(mappings[0].path, "/a");
    EXPECT_EQ(mappings[0].residentBytes, 1024U);
    EXPECT_EQ(mappings[1].start, 0x6000U);
    EXPECT_EQ(mappings[1].residentBytes, 2048U);
}

TEST(SmapsParserTest, AnEmptyFileHasNoMappings)
{
    EXPECT_TRUE(Smaps::parse("").empty());
}

TEST(SmapsParserTest, AFieldLineIsAWordThenAColon)
{
    EXPECT_TRUE(Smaps::Detail::isFieldLine("Rss:   4 kB"));
    EXPECT_TRUE(Smaps::Detail::isFieldLine("VmFlags: rd"));
    EXPECT_TRUE(Smaps::Detail::isFieldLine("Pss_Dirty: 0 kB"));
    EXPECT_FALSE(Smaps::Detail::isFieldLine("7f3a1c028000-7f3a1c1bd000 r-xp 00028000 08:01 1835 /lib"));
    EXPECT_FALSE(Smaps::Detail::isFieldLine("abcdef-abcdf0 r-xp 00000000 08:01 1 /x:y"));
    EXPECT_FALSE(Smaps::Detail::isFieldLine(":4 kB"));
    EXPECT_FALSE(Smaps::Detail::isFieldLine("no colon"));
}

} // namespace
} // namespace Platform
