/// @file test_PartitionTable.cpp
/// @brief Platform::PartitionTable (#1632): GUID formatting, the GPT type GUID and MBR type byte names,
/// udev's type strings, and DRIVE_LAYOUT_INFORMATION_EX parsing from hand-built buffers (GPT, MBR with
/// unused slots and an extended container, raw, truncated, and every prefix of a valid buffer).

#include "PartitionLayoutTestData.h"
#include "Platform/ISystemInfoProbe.h"
#include "Platform/PartitionTable.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Platform::PartitionTable
{
namespace
{

using TestData::GIB;
using TestData::MIB;

TEST(PartitionTableTest, FormatsGuidsMixedEndian)
{
    const std::array<std::byte, 16> efi = TestData::guidBytes(TestData::EFI_SYSTEM);
    EXPECT_EQ(efi[0], std::byte{0x28}); // Data1 0xC12A7328, little endian
    EXPECT_EQ(efi[8], std::byte{0xBA}); // Data4 in order
    EXPECT_EQ(formatGuid(efi), "C12A7328-F81F-11D2-BA4B-00A0C93EC93B");
    EXPECT_EQ(formatGuid(std::array<std::byte, 16>{}), "00000000-0000-0000-0000-000000000000");
}

TEST(PartitionTableTest, NamesCommonGptTypes)
{
    EXPECT_EQ(gptTypeName("C12A7328-F81F-11D2-BA4B-00A0C93EC93B"), "EFI System");
    EXPECT_EQ(gptTypeName("E3C9E316-0B5C-4DB8-817D-F92DF00215AE"), "Microsoft reserved");
    EXPECT_EQ(gptTypeName("EBD0A0A2-B9E5-4433-87C0-68B6B72699C7"), "Basic data");
    EXPECT_EQ(gptTypeName("DE94BBA4-06D1-4D40-A16A-BFD50179D6AC"), "Recovery");
    EXPECT_EQ(gptTypeName("5808C8AA-7E8F-42E0-85D2-E1E90434CFB3"), "LDM metadata");
    EXPECT_EQ(gptTypeName("AF9B60A0-1431-4F62-BC68-3311714A69AD"), "LDM data");
    EXPECT_EQ(gptTypeName("0fc63daf-8483-4772-8e79-3d69d8477de4"), "Linux filesystem"); // udev's lower case
    EXPECT_EQ(gptTypeName("0657FD6D-A4AB-43C4-84E5-0933C84B4F4F"), "Linux swap");
    EXPECT_EQ(gptTypeName("E6D6D379-F507-44C2-A23C-238F2A3DF928"), "Linux LVM");
    EXPECT_EQ(gptTypeName("00000000-0000-0000-0000-000000000000"), "");
    EXPECT_EQ(gptTypeName(""), "");
    EXPECT_EQ(gptTypeName("not a guid"), "");
}

TEST(PartitionTableTest, NamesCommonMbrTypes)
{
    EXPECT_EQ(mbrTypeName(0x07), "NTFS/exFAT");
    EXPECT_EQ(mbrTypeName(0x0C), "FAT32");
    EXPECT_EQ(mbrTypeName(0x0F), "Extended");
    EXPECT_EQ(mbrTypeName(0x27), "Recovery");
    EXPECT_EQ(mbrTypeName(0x42), "LDM");
    EXPECT_EQ(mbrTypeName(0x82), "Linux swap");
    EXPECT_EQ(mbrTypeName(0x83), "Linux filesystem");
    EXPECT_EQ(mbrTypeName(0x8E), "Linux LVM");
    EXPECT_EQ(mbrTypeName(0xEE), "GPT protective");
    EXPECT_EQ(mbrTypeName(0xEF), "EFI System");
    EXPECT_EQ(mbrTypeName(0x00), ""); // an unused slot
    EXPECT_EQ(mbrTypeName(0x99), "");
    EXPECT_EQ(mbrTypeId(0x07), "0x07");
    EXPECT_EQ(mbrTypeId(0xEF), "0xEF");
}

TEST(PartitionTableTest, ReadsUdevTypeStrings)
{
    EXPECT_EQ(parseMbrType("0x83"), std::optional<std::uint8_t>(0x83));
    EXPECT_EQ(parseMbrType("0x7"), std::optional<std::uint8_t>(0x07));
    EXPECT_EQ(parseMbrType("0Xef"), std::optional<std::uint8_t>(0xEF));
    EXPECT_FALSE(parseMbrType("0x").has_value());
    EXPECT_FALSE(parseMbrType("0x123").has_value());
    EXPECT_FALSE(parseMbrType("0xzz").has_value());
    EXPECT_FALSE(parseMbrType("83").has_value());
    EXPECT_FALSE(parseMbrType("c12a7328-f81f-11d2-ba4b-00a0c93ec93b").has_value());

    Partition gpt;
    setType(gpt, "c12a7328-f81f-11d2-ba4b-00a0c93ec93b");
    EXPECT_EQ(gpt.typeId, "C12A7328-F81F-11D2-BA4B-00A0C93EC93B");
    EXPECT_EQ(gpt.typeName, "EFI System");
    Partition mbr;
    setType(mbr, "0x83");
    EXPECT_EQ(mbr.typeId, "0x83");
    EXPECT_EQ(mbr.typeName, "Linux filesystem");
    Partition unknown;
    setType(unknown, "01234567-89ab-cdef-0123-456789abcdef");
    EXPECT_EQ(unknown.typeId, "01234567-89AB-CDEF-0123-456789ABCDEF");
    EXPECT_EQ(unknown.typeName, "");
    Partition none;
    setType(none, "");
    EXPECT_EQ(none.typeId, "");
    EXPECT_EQ(none.typeName, "");
}

TEST(PartitionTableTest, ParsesGptLayout)
{
    const std::optional<DriveLayout> parsed = parseDriveLayout(TestData::windowsGptLayout());
    ASSERT_TRUE(parsed.has_value());
    const DriveLayout layout = parsed.value_or(DriveLayout{});
    EXPECT_EQ(layout.style, PartitionStyle::Gpt);
    ASSERT_EQ(layout.partitions.size(), 4U);
    EXPECT_EQ(layout.partitions[0].number, 1U);
    EXPECT_EQ(layout.partitions[0].typeId, TestData::EFI_SYSTEM);
    EXPECT_EQ(layout.partitions[0].typeName, "EFI System");
    EXPECT_EQ(layout.partitions[0].offsetBytes, MIB);
    EXPECT_EQ(layout.partitions[0].sizeBytes, 260 * MIB);
    EXPECT_EQ(layout.partitions[1].typeName, "Microsoft reserved");
    EXPECT_EQ(layout.partitions[2].typeName, "Basic data");
    EXPECT_EQ(layout.partitions[2].sizeBytes, 930 * GIB);
    EXPECT_EQ(layout.partitions[3].typeName, "Recovery");
    EXPECT_EQ(layout.partitions[3].number, 4U);
    EXPECT_TRUE(layout.partitions[3].mountPoint.empty());
    EXPECT_TRUE(layout.partitions[3].device.empty());
}

TEST(PartitionTableTest, ParsesMbrLayoutSkippingUnusedSlots)
{
    const std::optional<DriveLayout> parsed = parseDriveLayout(TestData::mbrLayout());
    ASSERT_TRUE(parsed.has_value());
    const DriveLayout layout = parsed.value_or(DriveLayout{});
    EXPECT_EQ(layout.style, PartitionStyle::Mbr);
    ASSERT_EQ(layout.partitions.size(), 2U); // the unused slot and the extended container are left out
    EXPECT_EQ(layout.partitions[0].number, 1U);
    EXPECT_EQ(layout.partitions[0].typeId, "0x07");
    EXPECT_EQ(layout.partitions[0].typeName, "NTFS/exFAT");
    EXPECT_EQ(layout.partitions[0].sizeBytes, 100 * GIB);
    EXPECT_EQ(layout.partitions[1].number, 2U);
    EXPECT_EQ(layout.partitions[1].typeName, "Linux filesystem");
    EXPECT_EQ(layout.partitions[1].offsetBytes, 111 * GIB);

    // A numbered slot of type 0 (PARTITION_ENTRY_UNUSED) with a length is still unused.
    const std::vector<std::byte> unused =
        TestData::driveLayout(STYLE_MBR, {{.offset = MIB, .length = GIB, .number = 1, .mbrType = 0x00, .gptType = {}}});
    ASSERT_TRUE(parseDriveLayout(unused).has_value());
    EXPECT_TRUE(parseDriveLayout(unused).value_or(DriveLayout{}).partitions.empty());

    // An unknown type keeps its byte and has no name.
    const std::vector<std::byte> odd =
        TestData::driveLayout(STYLE_MBR, {{.offset = MIB, .length = GIB, .number = 1, .mbrType = 0x99, .gptType = {}}});
    ASSERT_EQ(parseDriveLayout(odd).value_or(DriveLayout{}).partitions.size(), 1U);
    EXPECT_EQ(parseDriveLayout(odd).value_or(DriveLayout{}).partitions[0].typeId, "0x99");
    EXPECT_EQ(parseDriveLayout(odd).value_or(DriveLayout{}).partitions[0].typeName, "");
}

TEST(PartitionTableTest, RawShortAndTruncatedLayouts)
{
    const std::optional<DriveLayout> raw = parseDriveLayout(TestData::driveLayout(STYLE_RAW, {}));
    ASSERT_TRUE(raw.has_value());
    EXPECT_EQ(raw.value_or(DriveLayout{}).style, PartitionStyle::Raw);
    EXPECT_TRUE(raw.value_or(DriveLayout{}).partitions.empty());

    const std::optional<DriveLayout> unknownStyle = parseDriveLayout(TestData::driveLayout(7, {}));
    ASSERT_TRUE(unknownStyle.has_value());
    EXPECT_EQ(unknownStyle.value_or(DriveLayout{.style = PartitionStyle::Raw, .partitions = {}}).style, PartitionStyle::Unknown);

    EXPECT_FALSE(parseDriveLayout({}).has_value());
    const std::vector<std::byte> gpt = TestData::windowsGptLayout();
    EXPECT_FALSE(parseDriveLayout(std::span(gpt).first(LAYOUT_HEADER_BYTES - 1)).has_value());

    // A count larger than the buffer: only the entries that fit are read.
    std::vector<std::byte> lying = gpt;
    TestData::putLittleEndian(lying, 4, 1000, 4);
    EXPECT_EQ(parseDriveLayout(lying).value_or(DriveLayout{}).partitions.size(), 4U);
    EXPECT_EQ(parseDriveLayout(std::span(gpt).first(gpt.size() - 1)).value_or(DriveLayout{}).partitions.size(), 3U);

    // A negative offset or an empty partition is left out.
    const std::vector<std::byte> bad =
        TestData::driveLayout(STYLE_GPT,
                              {
                                  {.offset = ~std::uint64_t{0}, .length = GIB, .number = 1, .mbrType = 0, .gptType = TestData::BASIC_DATA},
                                  {.offset = MIB, .length = 0, .number = 2, .mbrType = 0, .gptType = TestData::BASIC_DATA},
                              });
    EXPECT_TRUE(parseDriveLayout(bad).value_or(DriveLayout{}).partitions.empty());

    // Every prefix parses without reading past it.
    std::size_t size = 0;
    while (size <= gpt.size())
    {
        static_cast<void>(parseDriveLayout(std::span(gpt).first(size)));
        ++size;
    }
}

} // namespace
} // namespace Platform::PartitionTable
