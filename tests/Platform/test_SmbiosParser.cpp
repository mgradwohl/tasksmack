/// @file test_SmbiosParser.cpp
/// @brief Platform::Smbios (#1513) on hand-built tables: the RawSMBIOSData header, the structure walk
/// (string sets, the double NUL, truncated and garbage input), string indices, types 0-3, chassis types
/// with the lock bit, platform roles, entry-point versions, and every prefix of a valid table.

#include "Platform/ISystemInfoProbe.h"
#include "Platform/SmbiosParser.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Platform::Smbios
{
namespace
{

using ByteVector = std::vector<std::uint8_t>;

/// A structure of @p length formatted bytes (header included) with @p fields set at their offsets,
/// then its string set and the double NUL.
[[nodiscard]] ByteVector makeStructure(std::uint8_t type,
                                       std::uint8_t length,
                                       std::initializer_list<std::pair<std::size_t, std::uint8_t>> fields,
                                       std::initializer_list<std::string_view> strings,
                                       std::uint16_t handle = 0x0100)
{
    ByteVector bytes(length, 0);
    bytes.at(0) = type;
    bytes.at(1) = length;
    bytes.at(2) = static_cast<std::uint8_t>(handle & 0xFFU);
    bytes.at(3) = static_cast<std::uint8_t>(handle >> 8U);
    for (const auto& [offset, value] : fields)
    {
        bytes.at(offset) = value;
    }
    for (const std::string_view text : strings)
    {
        bytes.insert(bytes.end(), text.begin(), text.end());
        bytes.push_back(0);
    }
    bytes.push_back(0);
    if (std::empty(strings)) // initializer_list has no empty() member
    {
        bytes.push_back(0);
    }
    return bytes;
}

[[nodiscard]] ByteVector concat(std::initializer_list<ByteVector> parts)
{
    ByteVector all;
    for (const ByteVector& part : parts)
    {
        all.insert(all.end(), part.begin(), part.end());
    }
    return all;
}

/// A RawSMBIOSData blob around @p table; @p declaredLength overrides its Length.
[[nodiscard]] ByteVector makeRaw(std::uint8_t major, std::uint8_t minor, const ByteVector& table, std::uint32_t declaredLength = 0xFFFFFFFF)
{
    const std::uint32_t length = declaredLength == 0xFFFFFFFF ? static_cast<std::uint32_t>(table.size()) : declaredLength;
    ByteVector raw{0, major, minor, 0};
    for (unsigned shift = 0; shift < 32; shift += 8)
    {
        raw.push_back(static_cast<std::uint8_t>((length >> shift) & 0xFFU));
    }
    raw.insert(raw.end(), table.begin(), table.end());
    return raw;
}

[[nodiscard]] ByteVector biosStructure(std::uint8_t ecMajor = 0x01, std::uint8_t ecMinor = 0x17)
{
    return makeStructure(TYPE_BIOS,
                         0x18,
                         {{0x04, 1}, {0x05, 2}, {0x08, 3}, {0x16, ecMajor}, {0x17, ecMinor}},
                         {"American Megatrends", "1.2.3 ", "05/14/2024"});
}

[[nodiscard]] ByteVector systemStructure()
{
    const std::initializer_list<std::pair<std::size_t, std::uint8_t>> fields{
        {0x04, 1},    {0x05, 2},    {0x06, 3},    {0x07, 4},    {0x08, 0x33}, {0x09, 0x22}, {0x0A, 0x11}, {0x0B, 0x00},
        {0x0C, 0x55}, {0x0D, 0x44}, {0x0E, 0x77}, {0x0F, 0x66}, {0x10, 0x88}, {0x11, 0x99}, {0x12, 0xAA}, {0x13, 0xBB},
        {0x14, 0xCC}, {0x15, 0xDD}, {0x16, 0xEE}, {0x17, 0xFF}, {0x19, 5},    {0x1A, 6},
    };
    return makeStructure(TYPE_SYSTEM, 0x1B, fields, {"Contoso", "Surface Pro", "1.0", "SN-123", "SKU-9", "Surface"});
}

[[nodiscard]] ByteVector fullTable()
{
    return concat({
        biosStructure(),
        systemStructure(),
        makeStructure(TYPE_BASEBOARD, 0x0F, {{0x04, 1}, {0x05, 2}, {0x06, 3}, {0x07, 4}}, {"Contoso", "Board X", "Rev A", "BSN-1"}),
        makeStructure(TYPE_CHASSIS, 0x15, {{0x04, 1}, {0x05, 0x8A}, {0x06, 0}, {0x07, 2}}, {"Contoso", "CSN-1"}),
        makeStructure(TYPE_END_OF_TABLE, 4, {}, {}),
    });
}

TEST(SmbiosParserTest, RawHeaderGivesVersionAndClampsLength)
{
    const ByteVector table = fullTable();
    const auto parsed = parseRawSmbiosData(makeRaw(3, 4, table));
    ASSERT_TRUE(parsed.has_value());
    const Table header = parsed.value_or(Table{});
    EXPECT_EQ(header.majorVersion, 3);
    EXPECT_EQ(header.minorVersion, 4);
    EXPECT_EQ(header.data.size(), table.size());

    // A Length past the bytes there is clamped; a shorter one is honoured.
    EXPECT_EQ(parseRawSmbiosData(makeRaw(3, 4, table, 1'000'000)).value_or(Table{}).data.size(), table.size());
    EXPECT_EQ(parseRawSmbiosData(makeRaw(3, 4, table, 10)).value_or(Table{}).data.size(), 10U);
    // Shorter than the header.
    EXPECT_FALSE(parseRawSmbiosData(ByteVector{0, 3, 4, 0, 0, 0, 0}).has_value());
    EXPECT_FALSE(parseRawSmbiosData(ByteVector{}).has_value());
}

TEST(SmbiosParserTest, WalksStructuresAndStopsAtEndOfTable)
{
    ByteVector table = fullTable();
    const ByteVector after = makeStructure(TYPE_MEMORY_DEVICE, 0x28, {}, {"DIMM"});
    table.insert(table.end(), after.begin(), after.end()); // past type 127: ignored
    const auto structures = parseStructures(table);
    ASSERT_EQ(structures.size(), 5U);
    EXPECT_EQ(structures[0].type, TYPE_BIOS);
    EXPECT_EQ(structures[0].handle, 0x0100);
    EXPECT_EQ(structures[0].formatted.size(), 0x18U);
    EXPECT_EQ(structures[3].type, TYPE_CHASSIS);
    EXPECT_EQ(structures[4].type, TYPE_END_OF_TABLE);
    EXPECT_TRUE(structures[4].strings.empty());
    EXPECT_EQ(findFirst(structures, TYPE_MEMORY_DEVICE), nullptr);
    ASSERT_NE(findFirst(structures, TYPE_BASEBOARD), nullptr);
    EXPECT_EQ(findFirst(structures, TYPE_BASEBOARD)->string(2), "Board X");
}

TEST(SmbiosParserTest, StringIndices)
{
    const ByteVector structuresBytes = makeStructure(TYPE_BASEBOARD,
                                                     8,
                                                     {{0x04, 2}, {0x05, 0}, {0x06, 9}, {0x07, 1}},
                                                     {"  padded  ", "Second\tTab\x01"}); // the structures view these bytes
    const auto structures = parseStructures(structuresBytes);
    ASSERT_EQ(structures.size(), 1U);
    const Structure& board = structures[0];
    EXPECT_EQ(board.string(0), "");           // 0 = none
    EXPECT_EQ(board.string(1), "padded");     // trimmed
    EXPECT_EQ(board.string(2), "Second Tab"); // control characters made spaces, then trimmed
    EXPECT_EQ(board.string(3), "");           // out of range
    EXPECT_EQ(board.string(255), "");
    EXPECT_EQ(board.stringAt(0x04), "Second Tab");
    EXPECT_EQ(board.stringAt(0x05), "");
    EXPECT_EQ(board.stringAt(0x06), "");
    EXPECT_EQ(board.stringAt(0x08), ""); // past the formatted area
    EXPECT_EQ(board.stringAt(1'000), "");

    // A structure without strings has an empty set, and the next one parses after its double NUL.
    const ByteVector twoBytes = concat(
        {makeStructure(TYPE_CHASSIS, 6, {}, {}), makeStructure(TYPE_BASEBOARD, 5, {{0x04, 1}}, {"B"})}); // the structures view these bytes
    const auto two = parseStructures(twoBytes);
    ASSERT_EQ(two.size(), 2U);
    EXPECT_TRUE(two[0].strings.empty());
    EXPECT_EQ(two[0].string(1), "");
    EXPECT_EQ(two[1].stringAt(0x04), "B");
}

TEST(SmbiosParserTest, FieldReadsStayInsideTheFormattedArea)
{
    const ByteVector structuresBytes = makeStructure(
        TYPE_MEMORY_DEVICE, 8, {{0x04, 0x34}, {0x05, 0x12}, {0x06, 0x78}, {0x07, 0x56}}, {}); // the structures view these bytes
    const auto structures = parseStructures(structuresBytes);
    ASSERT_EQ(structures.size(), 1U);
    const Structure& s = structures[0];
    EXPECT_EQ(s.byte(7), 0x56);
    EXPECT_FALSE(s.byte(8).has_value());
    EXPECT_EQ(s.word(4), 0x1234);
    EXPECT_EQ(s.word(6), 0x5678);
    EXPECT_FALSE(s.word(7).has_value());
    EXPECT_EQ(s.dword(4), 0x56781234U);
    EXPECT_FALSE(s.dword(5).has_value());
    EXPECT_FALSE(s.dword(static_cast<std::size_t>(-1)).has_value());
}

TEST(SmbiosParserTest, MalformedInputStopsTheWalkAndKeepsWhatCameBefore)
{
    EXPECT_TRUE(parseStructures({}).empty()); // zero-length table
    EXPECT_TRUE(parseStructures(ByteVector{0, 4, 0}).empty());

    const ByteVector good = biosStructure();
    // A length under the header size.
    EXPECT_EQ(parseStructures(concat({good, ByteVector{1, 3, 0, 0, 0, 0}})).size(), 1U);
    // A length past the buffer: a truncated structure.
    EXPECT_EQ(parseStructures(concat({good, ByteVector{1, 0x1B, 0, 0, 1, 2}})).size(), 1U);
    // A string set without its double NUL.
    EXPECT_EQ(parseStructures(concat({good, ByteVector{2, 4, 0, 0, 'a', 0, 'b'}})).size(), 1U);
    EXPECT_EQ(parseStructures(concat({good, ByteVector{2, 4, 0, 0, 0}})).size(), 1U);
    EXPECT_EQ(parseStructures(concat({good, ByteVector{2, 4, 0, 0}})).size(), 1U);
}

TEST(SmbiosParserTest, EveryPrefixOfAValidTableParsesSafely)
{
    // Under the sanitizers this is the bounds check: no prefix may read past its end.
    const ByteVector raw = makeRaw(3, 4, fullTable());
    for (std::size_t size = 0; size <= raw.size(); ++size)
    {
        const ByteVector prefix(raw.begin(), raw.begin() + static_cast<std::ptrdiff_t>(size));
        FirmwareInfo info;
        static_cast<void>(decodeFirmware(prefix, info));
        // Garbage: every byte value as a type and length.
        ByteVector garbage(prefix);
        for (std::uint8_t& b : garbage)
        {
            b = static_cast<std::uint8_t>(b ^ 0x5AU);
        }
        static_cast<void>(decodeFirmware(garbage, info));
    }
    SUCCEED();
}

TEST(SmbiosParserTest, DecodesBios)
{
    const ByteVector structuresBytes = biosStructure(); // the structures view these bytes
    const auto structures = parseStructures(structuresBytes);
    ASSERT_EQ(structures.size(), 1U);
    const BiosFields bios = decodeBios(structures[0]);
    EXPECT_EQ(bios.vendor, "American Megatrends");
    EXPECT_EQ(bios.version, "1.2.3");
    EXPECT_EQ(bios.releaseDate, "05/14/2024");
    EXPECT_EQ(bios.embeddedControllerVersion, "1.23");

    EXPECT_EQ(decodeBios(parseStructures(biosStructure(0xFF, 0xFF))[0]).embeddedControllerVersion, ""); // no EC
    // An SMBIOS 2.3 structure ends before the EC bytes.
    const ByteVector oldBytes = makeStructure(TYPE_BIOS, 0x12, {{0x04, 1}}, {"Phoenix"}); // the structures view these bytes
    const auto old = parseStructures(oldBytes);
    EXPECT_EQ(decodeBios(old[0]).vendor, "Phoenix");
    EXPECT_EQ(decodeBios(old[0]).embeddedControllerVersion, "");
}

TEST(SmbiosParserTest, DecodesSystemAndTheUuidByteOrder)
{
    const ByteVector structuresBytes = systemStructure(); // the structures view these bytes
    const auto structures = parseStructures(structuresBytes);
    ASSERT_EQ(structures.size(), 1U);
    const SystemFields system = decodeSystem(structures[0], Table{.majorVersion = 3, .minorVersion = 4, .data = {}});
    EXPECT_EQ(system.manufacturer, "Contoso");
    EXPECT_EQ(system.productName, "Surface Pro");
    EXPECT_EQ(system.version, "1.0");
    EXPECT_EQ(system.serialNumber, "SN-123");
    EXPECT_EQ(system.skuNumber, "SKU-9");
    EXPECT_EQ(system.family, "Surface");
    // From 2.6 the first three fields are little-endian; before it, as stored.
    EXPECT_EQ(system.uuid, "00112233-4455-6677-8899-AABBCCDDEEFF");
    EXPECT_EQ(decodeSystem(structures[0], Table{.majorVersion = 2, .minorVersion = 5, .data = {}}).uuid,
              "33221100-5544-7766-8899-AABBCCDDEEFF");

    EXPECT_EQ(formatUuid(ByteVector(16, 0x00), true), "");
    EXPECT_EQ(formatUuid(ByteVector(16, 0xFF), true), "");
    EXPECT_EQ(formatUuid(ByteVector(15, 0x12), true), "");

    // A 2.0 structure (length 8) has no UUID, SKU or family.
    const ByteVector oldBytes = makeStructure(TYPE_SYSTEM, 8, {{0x04, 1}}, {"Acme"}); // the structures view these bytes
    const auto old = parseStructures(oldBytes);
    const SystemFields short20 = decodeSystem(old[0], Table{.majorVersion = 2, .minorVersion = 0, .data = {}});
    EXPECT_EQ(short20.manufacturer, "Acme");
    EXPECT_EQ(short20.uuid, "");
    EXPECT_EQ(short20.skuNumber, "");
    EXPECT_EQ(short20.family, "");
}

TEST(SmbiosParserTest, DecodesBaseboardAndChassis)
{
    const ByteVector structuresBytes = fullTable(); // the structures view these bytes
    const auto structures = parseStructures(structuresBytes);
    const BaseboardFields board = decodeBaseboard(*findFirst(structures, TYPE_BASEBOARD));
    EXPECT_EQ(board.manufacturer, "Contoso");
    EXPECT_EQ(board.product, "Board X");
    EXPECT_EQ(board.version, "Rev A");
    EXPECT_EQ(board.serialNumber, "BSN-1");

    const ChassisFields chassis = decodeChassis(*findFirst(structures, TYPE_CHASSIS));
    EXPECT_EQ(chassis.manufacturer, "Contoso");
    EXPECT_EQ(chassis.type, 0x0A); // 0x8A with the lock bit cleared
    EXPECT_TRUE(chassis.hasLock);
    EXPECT_EQ(chassis.version, "");
    EXPECT_EQ(chassis.serialNumber, "CSN-1");
}

TEST(SmbiosParserTest, ChassisTypesAndPlatformRoles)
{
    EXPECT_EQ(chassisTypeName(0x03), "Desktop");
    EXPECT_EQ(chassisTypeName(0x09), "Laptop");
    EXPECT_EQ(chassisTypeName(0x0A), "Notebook");
    EXPECT_EQ(chassisTypeName(0x8A), "Notebook"); // the lock bit is ignored
    EXPECT_EQ(chassisTypeName(0x1E), "Tablet");
    EXPECT_EQ(chassisTypeName(0x1F), "Convertible");
    EXPECT_EQ(chassisTypeName(0x23), "Mini PC");
    EXPECT_EQ(chassisTypeName(0x24), "Stick PC");
    EXPECT_EQ(chassisTypeName(0x00), "");
    EXPECT_EQ(chassisTypeName(0x25), ""); // past the table
    EXPECT_EQ(chassisTypeName(0x7F), "");
    EXPECT_EQ(chassisTypeName(0xFF), "");

    EXPECT_EQ(platformRoleFromChassis(0x07), "Desktop");
    EXPECT_EQ(platformRoleFromChassis(0x8A), "Mobile");
    EXPECT_EQ(platformRoleFromChassis(0x1F), "Mobile");
    EXPECT_EQ(platformRoleFromChassis(0x17), "Server");
    EXPECT_EQ(platformRoleFromChassis(0x02), "");
}

TEST(SmbiosParserTest, EntryPointVersions)
{
    ByteVector sm3(0x18, 0);
    sm3.at(0) = '_';
    sm3.at(1) = 'S';
    sm3.at(2) = 'M';
    sm3.at(3) = '3';
    sm3.at(4) = '_';
    sm3.at(7) = 3;
    sm3.at(8) = 5;
    EXPECT_EQ(entryPointVersion(sm3), "3.5");
    sm3.pop_back();
    EXPECT_EQ(entryPointVersion(sm3), ""); // truncated

    ByteVector sm(0x1F, 0);
    sm.at(0) = '_';
    sm.at(1) = 'S';
    sm.at(2) = 'M';
    sm.at(3) = '_';
    sm.at(6) = 2;
    sm.at(7) = 8;
    EXPECT_EQ(entryPointVersion(sm), "2.8");
    EXPECT_EQ(entryPointVersion(ByteVector{'_', 'S', 'M'}), "");
    EXPECT_EQ(entryPointVersion({}), "");
}

TEST(SmbiosParserTest, DecodeFirmwareFillsTheSection)
{
    FirmwareInfo info;
    ASSERT_TRUE(decodeFirmware(makeRaw(3, 4, fullTable()), info));
    EXPECT_EQ(info.smbiosVersion, "3.4");
    EXPECT_EQ(info.biosVendor, "American Megatrends");
    EXPECT_EQ(info.embeddedControllerVersion, "1.23");
    EXPECT_EQ(info.systemManufacturer, "Contoso");
    EXPECT_EQ(info.systemModel, "Surface Pro");
    EXPECT_EQ(info.systemSerial, "SN-123");
    EXPECT_EQ(info.boardProduct, "Board X");
    EXPECT_EQ(info.chassisType, "Notebook");
    EXPECT_EQ(info.platformRole, "Mobile");

    FirmwareInfo none;
    EXPECT_FALSE(decodeFirmware(ByteVector{0, 3}, none));
    EXPECT_TRUE(none.smbiosVersion.empty());
}

} // namespace
} // namespace Platform::Smbios
