/// @file test_ProcessMemoryMap.cpp
/// @brief Domain::MemoryMap (#3089): a memory map's totals overall, by kind and by file, the kind labels and
/// the protection text, for both platforms' rows (Linux's measured sizes, Windows' absent ones).

#include "Domain/ProcessMemoryMap.h"
#include "Platform/IProcessMemoryMap.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Domain::MemoryMap
{
namespace
{

using Platform::MemoryMapping;
using Platform::MemoryMappingKind;

/// A Linux-style mapping of @p bytes, @p pss of it proportional, all private dirty.
MemoryMapping measured(MemoryMappingKind kind, std::string path, std::uint64_t start, std::uint64_t bytes, std::uint64_t pss)
{
    MemoryMapping mapping;
    mapping.start = start;
    mapping.end = start + bytes;
    mapping.kind = kind;
    mapping.path = std::move(path);
    mapping.residentBytes = pss;
    mapping.pssBytes = pss;
    mapping.privateCleanBytes = 0;
    mapping.privateDirtyBytes = pss;
    mapping.sharedCleanBytes = 0;
    mapping.sharedDirtyBytes = 0;
    mapping.swapBytes = 0;
    return mapping;
}

TEST(ProcessMemoryMapTest, SumsEveryMappingByKindAndByFileLargestFirst)
{
    std::vector<MemoryMapping> mappings{
        measured(MemoryMappingKind::Image, "/usr/lib/libc.so.6", 0x1000, 0x4000, 100),
        measured(MemoryMappingKind::Image, "/usr/lib/libc.so.6", 0x5000, 0x1000, 50),
        measured(MemoryMappingKind::Heap, "[heap]", 0x10000, 0x8000, 4000),
        measured(MemoryMappingKind::File, "/usr/lib/locale/locale-archive", 0x20000, 0x10000, 10),
        measured(MemoryMappingKind::Anonymous, "", 0x40000, 0x2000, 500),
    };
    mappings[1].sharedCleanBytes = 30;
    mappings[1].swapBytes = 7;

    const Summary summary = summarize(mappings);
    EXPECT_EQ(summary.all.mappings, 5U);
    EXPECT_EQ(summary.all.sizeBytes, 0x4000U + 0x1000U + 0x8000U + 0x10000U + 0x2000U);
    EXPECT_EQ(summary.all.reservedBytes, 0U);
    EXPECT_EQ(summary.all.pssBytes, 4660U);
    EXPECT_EQ(summary.all.privateBytes, 4660U);
    EXPECT_EQ(summary.all.sharedBytes, 30U);
    EXPECT_EQ(summary.all.swapBytes, 7U);

    // Kinds by PSS: the heap, the anonymous memory, libc's image, then the data file.
    ASSERT_EQ(summary.byKind.size(), 4U);
    EXPECT_EQ(summary.byKind[0].kind, MemoryMappingKind::Heap);
    EXPECT_EQ(summary.byKind[1].kind, MemoryMappingKind::Anonymous);
    EXPECT_EQ(summary.byKind[2].kind, MemoryMappingKind::Image);
    EXPECT_EQ(summary.byKind[2].totals.mappings, 2U);
    EXPECT_EQ(summary.byKind[2].totals.pssBytes, 150U);
    EXPECT_EQ(summary.byKind[3].kind, MemoryMappingKind::File);

    // Files: Image and File mappings only, one row per file.
    ASSERT_EQ(summary.byFile.size(), 2U);
    EXPECT_EQ(summary.byFile[0].path, "/usr/lib/libc.so.6");
    EXPECT_EQ(summary.byFile[0].totals.mappings, 2U);
    EXPECT_EQ(summary.byFile[0].totals.sizeBytes, 0x5000U);
    EXPECT_EQ(summary.byFile[1].path, "/usr/lib/locale/locale-archive");
}

TEST(ProcessMemoryMapTest, ADeletedFileIsItsOwnRow)
{
    std::vector<MemoryMapping> mappings{
        measured(MemoryMappingKind::Image, "/usr/lib/libfoo.so", 0x1000, 0x1000, 5),
        measured(MemoryMappingKind::File, "/usr/lib/libfoo.so", 0x2000, 0x1000, 5),
    };
    mappings[0].deleted = true;
    const Summary summary = summarize(mappings);
    ASSERT_EQ(summary.byFile.size(), 2U);
    // Equal weights keep path, then deleted, order: the file now there, then the deleted one.
    EXPECT_FALSE(summary.byFile[0].deleted);
    EXPECT_TRUE(summary.byFile[1].deleted);
    EXPECT_EQ(summary.byFile[1].path, "/usr/lib/libfoo.so");
}

// Windows' VirtualQueryEx rows carry no smaps sizes: their totals stay absent rather than read as 0, the
// reserved address space is counted apart, and the order falls back to address space.
TEST(ProcessMemoryMapTest, UnmeasuredSizesStayAbsentAndReservedSpaceIsApart)
{
    MemoryMapping image{.start = 0x10000, .end = 0x30000, .kind = MemoryMappingKind::Image, .path = R"(C:\Windows\System32\ntdll.dll)"};
    MemoryMapping committed{.start = 0x40000, .end = 0x41000, .kind = MemoryMappingKind::Anonymous};
    MemoryMapping reserved{.start = 0x50000, .end = 0x150000, .reserved = true, .kind = MemoryMappingKind::Anonymous};
    const std::vector<MemoryMapping> mappings{image, committed, reserved};

    const Summary summary = summarize(mappings);
    EXPECT_EQ(summary.all.mappings, 3U);
    EXPECT_EQ(summary.all.sizeBytes, 0x21000U);
    EXPECT_EQ(summary.all.reservedBytes, 0x100000U);
    EXPECT_FALSE(summary.all.residentBytes.has_value());
    EXPECT_FALSE(summary.all.pssBytes.has_value());
    EXPECT_FALSE(summary.all.privateBytes.has_value());
    EXPECT_FALSE(summary.all.sharedBytes.has_value());
    EXPECT_FALSE(summary.all.swapBytes.has_value());
    ASSERT_EQ(summary.byKind.size(), 2U);
    EXPECT_EQ(summary.byKind[0].kind, MemoryMappingKind::Image) << "0x20000 committed beats 0x1000";
    EXPECT_EQ(summary.byKind[1].totals.reservedBytes, 0x100000U);

    // One measured mapping makes the total present: what Windows' lane gets if it fills residentBytes.
    MemoryMapping resident = committed;
    resident.residentBytes = 4096;
    const Summary some = summarize(std::vector<MemoryMapping>{image, resident});
    EXPECT_EQ(some.all.residentBytes, 4096U);
}

TEST(ProcessMemoryMapTest, TotalsSaturateRatherThanWrap)
{
    constexpr std::uint64_t MAX = std::numeric_limits<std::uint64_t>::max();
    std::vector<MemoryMapping> mappings{
        measured(MemoryMappingKind::Anonymous, "", 0, MAX, MAX),
        measured(MemoryMappingKind::Anonymous, "", 0, 10, 10),
    };
    const Summary summary = summarize(mappings);
    EXPECT_EQ(summary.all.sizeBytes, MAX);
    EXPECT_EQ(summary.all.pssBytes, MAX);
}

TEST(ProcessMemoryMapTest, AnEmptyMapHasNoRows)
{
    const Summary summary = summarize({});
    EXPECT_EQ(summary.all, Totals{});
    EXPECT_TRUE(summary.byKind.empty());
    EXPECT_TRUE(summary.byFile.empty());
}

TEST(ProcessMemoryMapTest, KindLabelsAndProtectionText)
{
    EXPECT_EQ(kindLabel(MemoryMappingKind::Image), "Image");
    EXPECT_EQ(kindLabel(MemoryMappingKind::File), "Mapped file");
    EXPECT_EQ(kindLabel(MemoryMappingKind::Anonymous), "Private");
    EXPECT_EQ(kindLabel(MemoryMappingKind::Heap), "Heap");
    EXPECT_EQ(kindLabel(MemoryMappingKind::Stack), "Stack");
    EXPECT_EQ(kindLabel(MemoryMappingKind::System), "System");
    EXPECT_EQ(kindLabel(MemoryMappingKind::Other), "Other");

    EXPECT_EQ(protectionText(MemoryMapping{.readable = true, .executable = true}), "r-xp");
    EXPECT_EQ(protectionText(MemoryMapping{.readable = true, .writable = true, .shared = true}), "rw-s");
    EXPECT_EQ(protectionText(MemoryMapping{}), "---p");
    EXPECT_EQ(protectionText(MemoryMapping{.readable = true, .writable = true, .guard = true}), "rw-pg");
}

TEST(ProcessMemoryMapTest, AMalformedMappingSpansNothing)
{
    EXPECT_EQ((MemoryMapping{.start = 10, .end = 5}).sizeBytes(), 0U);
}

} // namespace
} // namespace Domain::MemoryMap
