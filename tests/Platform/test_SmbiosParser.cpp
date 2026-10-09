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

/// Writes @p value little-endian over @p size bytes at @p offset of a structure's formatted area.
void putLittleEndian(ByteVector& structure, std::size_t offset, std::uint64_t value, std::size_t size)
{
    for (std::size_t i = 0; i < size; ++i)
    {
        structure.at(offset + i) = static_cast<std::uint8_t>((value >> (8U * i)) & 0xFFU);
    }
}

/// A type 16 (Physical Memory Array) of SMBIOS 2.7+ length.
[[nodiscard]] ByteVector
memoryArray(std::uint16_t handle, std::uint8_t use, std::uint32_t maxCapacityKib, std::uint16_t devices, std::uint64_t extendedBytes = 0)
{
    ByteVector bytes = makeStructure(TYPE_PHYSICAL_MEMORY_ARRAY, 0x17, {{0x04, 0x03}, {0x05, use}, {0x06, 0x03}}, {}, handle);
    putLittleEndian(bytes, 0x07, maxCapacityKib, 4);
    putLittleEndian(bytes, 0x0B, 0xFFFE, 2);
    putLittleEndian(bytes, 0x0D, devices, 2);
    putLittleEndian(bytes, 0x0F, extendedBytes, 8);
    return bytes;
}

/// What a type 17 (Memory Device) of SMBIOS 3.3+ length holds; strings are 1 locator, 2 bank,
/// 3 manufacturer, 4 serial, 5 part number.
struct DeviceSpec
{
    std::uint16_t arrayHandle = 0x1000;
    std::uint16_t size = 0x4000; // 16384 MiB
    std::uint32_t extendedSizeMib = 0;
    std::uint8_t formFactor = 0x09; // DIMM
    std::uint8_t type = 0x22;       // DDR5
    std::uint16_t speed = 6400;
    std::uint16_t configuredSpeed = 5600;
    std::uint32_t extendedSpeed = 0;
    std::uint32_t extendedConfiguredSpeed = 0;
    std::string_view locator = "DIMM A1";
    std::string_view manufacturer = "Samsung";
    std::string_view partNumber = "M323R2GA3BB0-CQKOD    ";
    std::uint8_t length = 0x5C;
};

[[nodiscard]] ByteVector memoryDevice(const DeviceSpec& spec, std::uint16_t handle = 0x1100)
{
    ByteVector bytes = makeStructure(TYPE_MEMORY_DEVICE,
                                     0x5C,
                                     {{0x0E, spec.formFactor}, {0x10, 1}, {0x11, 2}, {0x12, spec.type}, {0x17, 3}, {0x18, 4}, {0x1A, 5}},
                                     {spec.locator, "BANK 0", spec.manufacturer, "12345678", spec.partNumber},
                                     handle);
    putLittleEndian(bytes, 0x04, spec.arrayHandle, 2);
    putLittleEndian(bytes, 0x0C, spec.size, 2);
    putLittleEndian(bytes, 0x15, spec.speed, 2);
    putLittleEndian(bytes, 0x1C, spec.extendedSizeMib, 4);
    putLittleEndian(bytes, 0x20, spec.configuredSpeed, 2);
    putLittleEndian(bytes, 0x54, spec.extendedSpeed, 4);
    putLittleEndian(bytes, 0x58, spec.extendedConfiguredSpeed, 4);
    if (spec.length < 0x5C)
    {
        // An older, shorter structure: drop the formatted bytes past its length, keep the strings.
        bytes.erase(bytes.begin() + spec.length, bytes.begin() + 0x5C);
        bytes.at(1) = spec.length;
    }
    return bytes;
}

constexpr std::uint64_t MIB = std::uint64_t{1024} * 1024;
constexpr std::uint64_t GIB = MIB * 1024;

TEST(SmbiosParserTest, DecodesATypicalMemoryDevice)
{
    const ByteVector bytes = memoryDevice(DeviceSpec{});
    const auto structures = parseStructures(bytes);
    ASSERT_EQ(structures.size(), 1U);
    const MemoryDeviceFields fields = decodeMemoryDevice(structures.front());
    EXPECT_EQ(fields.arrayHandle, 0x1000);
    EXPECT_TRUE(fields.installed);
    EXPECT_EQ(fields.sizeBytes, 16 * GIB);
    EXPECT_EQ(memoryTypeName(fields.memoryType), "DDR5");
    EXPECT_EQ(memoryFormFactorName(fields.formFactor), "DIMM");
    EXPECT_EQ(fields.locator, "DIMM A1");
    EXPECT_EQ(fields.bankLocator, "BANK 0");
    EXPECT_EQ(fields.speedMts, 6400U);
    EXPECT_EQ(fields.configuredSpeedMts, 5600U);
    EXPECT_EQ(fields.manufacturer, "Samsung");
    EXPECT_EQ(fields.partNumber, "M323R2GA3BB0-CQKOD"); // trailing padding trimmed
}

TEST(SmbiosParserTest, MemoryDeviceSizes)
{
    const auto decode = [](const DeviceSpec& spec)
    {
        const ByteVector bytes = memoryDevice(spec);
        return decodeMemoryDevice(parseStructures(bytes).front());
    };
    // An empty slot.
    EXPECT_FALSE(decode({.size = 0, .manufacturer = "Unknown", .partNumber = "Not Specified"}).installed);
    // Extended size: 0x7FFF says the size is the DWORD at 0x1C, in MiB (bit 31 reserved).
    const MemoryDeviceFields extended = decode({.size = 0x7FFF, .extendedSizeMib = 0x8001'0000});
    EXPECT_EQ(extended.sizeBytes, 64 * GIB);
    // Bit 15 set: KiB granularity.
    EXPECT_EQ(decode({.size = 0x8200}).sizeBytes, 512U * 1024); // 0x8000 | 512
    // 0xFFFF: installed, size unknown.
    const MemoryDeviceFields unknown = decode({.size = 0xFFFF});
    EXPECT_TRUE(unknown.installed);
    EXPECT_EQ(unknown.sizeBytes, 0U);
}

TEST(SmbiosParserTest, MemoryDeviceExtendedSpeedsAndPlaceholders)
{
    const ByteVector bytes = memoryDevice({
        .formFactor = 0x0B,
        .type = 0x23,
        .speed = 0xFFFF,
        .configuredSpeed = 0xFFFF,
        .extendedSpeed = 8533,
        .extendedConfiguredSpeed = 7500,
        .manufacturer = "NOT SPECIFIED",
        .partNumber = "Unknown",
    });
    const MemoryDeviceFields fields = decodeMemoryDevice(parseStructures(bytes).front());
    EXPECT_EQ(memoryTypeName(fields.memoryType), "LPDDR5");
    EXPECT_EQ(memoryFormFactorName(fields.formFactor), "Row of chips");
    EXPECT_EQ(fields.speedMts, 8533U);
    EXPECT_EQ(fields.configuredSpeedMts, 7500U);
    EXPECT_EQ(fields.manufacturer, "");
    EXPECT_EQ(fields.partNumber, "");
}

TEST(SmbiosParserTest, TruncatedMemoryDevicesReadOnlyWhatIsThere)
{
    // SMBIOS 2.3 length (0x1B): no extended size, no configured speed.
    const ByteVector v23 = memoryDevice({.size = 0x7FFF, .extendedSizeMib = 0x10000, .length = 0x1B});
    const MemoryDeviceFields old = decodeMemoryDevice(parseStructures(v23).front());
    EXPECT_TRUE(old.installed);
    EXPECT_EQ(old.sizeBytes, 0U); // the extended size it points at isn't there
    EXPECT_EQ(old.speedMts, 6400U);
    EXPECT_EQ(old.configuredSpeedMts, 0U);
    EXPECT_EQ(old.partNumber, "M323R2GA3BB0-CQKOD");

    // 0xFFFF speeds in a structure too short for the extended fields read as unknown.
    const ByteVector v27 = memoryDevice({.speed = 0xFFFF, .configuredSpeed = 0xFFFF, .extendedSpeed = 9000, .length = 0x28});
    const MemoryDeviceFields mid = decodeMemoryDevice(parseStructures(v27).front());
    EXPECT_EQ(mid.speedMts, 0U);
    EXPECT_EQ(mid.configuredSpeedMts, 0U);
    EXPECT_EQ(mid.sizeBytes, 16 * GIB);

    // Too short to hold Size: counted as installed with an unknown size; strings past it are empty.
    const ByteVector header = makeStructure(TYPE_MEMORY_DEVICE, 0x08, {}, {"DIMM"});
    const MemoryDeviceFields bare = decodeMemoryDevice(parseStructures(header).front());
    EXPECT_TRUE(bare.installed);
    EXPECT_EQ(bare.sizeBytes, 0U);
    EXPECT_EQ(bare.locator, "");
    EXPECT_EQ(memoryTypeName(bare.memoryType), "");
}

TEST(SmbiosParserTest, MemoryArrays)
{
    const ByteVector plain = memoryArray(0x1000, MEMORY_ARRAY_USE_SYSTEM, 64 * 1024 * 1024, 4);
    const MemoryArrayFields fields = decodeMemoryArray(parseStructures(plain).front());
    EXPECT_EQ(fields.handle, 0x1000);
    EXPECT_EQ(fields.use, MEMORY_ARRAY_USE_SYSTEM);
    EXPECT_EQ(fields.maxCapacityBytes, 64 * GIB);
    EXPECT_EQ(fields.deviceCount, 4);

    const ByteVector extended = memoryArray(0x1000, MEMORY_ARRAY_USE_SYSTEM, MEMORY_ARRAY_EXTENDED_CAPACITY, 8, 4096 * GIB);
    EXPECT_EQ(decodeMemoryArray(parseStructures(extended).front()).maxCapacityBytes, 4096 * GIB);

    // SMBIOS 2.1 length (0x0F): no extended capacity to defer to.
    ByteVector old = memoryArray(0x1000, MEMORY_ARRAY_USE_SYSTEM, MEMORY_ARRAY_EXTENDED_CAPACITY, 2);
    old.erase(old.begin() + 0x0F, old.begin() + 0x17);
    old.at(1) = 0x0F;
    const MemoryArrayFields short21 = decodeMemoryArray(parseStructures(old).front());
    EXPECT_EQ(short21.maxCapacityBytes, 0U);
    EXPECT_EQ(short21.deviceCount, 2);
}

[[nodiscard]] ByteVector memoryTable()
{
    return concat({
        biosStructure(),
        memoryArray(0x1000, MEMORY_ARRAY_USE_SYSTEM, MEMORY_ARRAY_EXTENDED_CAPACITY, 4, 128 * GIB),
        memoryArray(0x2000, 0x05, 1024, 1), // flash: not RAM
        memoryDevice({.locator = "DIMM A1"}, 0x1100),
        memoryDevice({.size = 0, .locator = "DIMM A2", .manufacturer = "NO DIMM", .partNumber = "NO DIMM"}, 0x1101),
        memoryDevice({.size = 0x7FFF, .extendedSizeMib = 0x10000, .locator = "DIMM B1"}, 0x1102),
        memoryDevice({.size = 0, .locator = "DIMM B2"}, 0x1103),
        memoryDevice({.arrayHandle = 0x2000, .size = 1, .locator = "FLASH"}, 0x1104),
        makeStructure(TYPE_END_OF_TABLE, 4, {}, {}),
    });
}

TEST(SmbiosParserTest, DecodeMemoryModulesListsPopulatedSystemMemory)
{
    MemoryModulesInfo info;
    ASSERT_TRUE(decodeMemoryModules(makeRaw(3, 4, memoryTable()), info));
    EXPECT_TRUE(info.tableRead);
    EXPECT_EQ(info.slotCount, 4U);
    EXPECT_EQ(info.maxCapacityBytes, 128 * GIB);
    ASSERT_EQ(info.modules.size(), 2U);
    EXPECT_EQ(info.modules.at(0).locator, "DIMM A1");
    EXPECT_EQ(info.modules.at(0).sizeBytes, 16 * GIB);
    EXPECT_EQ(info.modules.at(0).type, "DDR5");
    EXPECT_EQ(info.modules.at(0).formFactor, "DIMM");
    EXPECT_EQ(info.modules.at(0).configuredSpeedMts, 5600U);
    EXPECT_EQ(info.modules.at(0).speedMts, 6400U);
    EXPECT_EQ(info.modules.at(1).locator, "DIMM B1");
    EXPECT_EQ(info.modules.at(1).sizeBytes, 64 * GIB);
    // decodeMemoryModules() fills only the table's facts.
    EXPECT_EQ(info.installedBytes, 0U);
    EXPECT_EQ(info.usableBytes, 0U);

    MemoryModulesInfo none;
    EXPECT_FALSE(decodeMemoryModules(ByteVector{0, 3, 4}, none));
    EXPECT_FALSE(none.tableRead);
}

TEST(SmbiosParserTest, MemorySlotsWithoutAnArray)
{
    // No type 16: the slots are the memory devices, and devices of an unknown array count.
    MemoryModulesInfo info;
    decodeMemoryTable(concat({memoryDevice({.arrayHandle = 0x4242}), memoryDevice({.size = 0, .locator = "DIMM 1"}, 0x1101)}), info);
    EXPECT_TRUE(info.tableRead);
    EXPECT_EQ(info.slotCount, 2U);
    EXPECT_EQ(info.maxCapacityBytes, 0U);
    EXPECT_EQ(info.modules.size(), 1U);

    // An empty table: read, nothing in it.
    MemoryModulesInfo empty;
    decodeMemoryTable({}, empty);
    EXPECT_TRUE(empty.tableRead);
    EXPECT_EQ(empty.slotCount, 0U);
    EXPECT_TRUE(empty.modules.empty());
}

TEST(SmbiosParserTest, EveryPrefixOfAMemoryTableDecodesSafely)
{
    const ByteVector table = memoryTable();
    for (std::size_t size = 0; size <= table.size(); ++size)
    {
        MemoryModulesInfo info;
        decodeMemoryTable(std::span(table).first(size), info);
        EXPECT_LE(info.modules.size(), 2U) << size;
    }
}

TEST(SmbiosParserTest, MemoryTypeAndFormFactorNames)
{
    EXPECT_EQ(memoryTypeName(0x1A), "DDR4");
    EXPECT_EQ(memoryTypeName(0x18), "DDR3");
    EXPECT_EQ(memoryTypeName(0x1E), "LPDDR4");
    EXPECT_EQ(memoryTypeName(0x22), "DDR5");
    EXPECT_EQ(memoryTypeName(0x23), "LPDDR5");
    EXPECT_EQ(memoryTypeName(0x24), "HBM3");
    EXPECT_EQ(memoryTypeName(0x02), ""); // Unknown
    EXPECT_EQ(memoryTypeName(0x15), ""); // reserved
    EXPECT_EQ(memoryTypeName(0xFF), "");
    EXPECT_EQ(memoryFormFactorName(0x0D), "SODIMM");
    EXPECT_EQ(memoryFormFactorName(0x11), "CAMM");
    EXPECT_EQ(memoryFormFactorName(0x02), "");
    EXPECT_EQ(memoryFormFactorName(0x40), "");

    EXPECT_TRUE(isPlaceholderString("Not Specified"));
    EXPECT_TRUE(isPlaceholderString("UNKNOWN"));
    EXPECT_TRUE(isPlaceholderString("To Be Filled By O.E.M."));
    EXPECT_FALSE(isPlaceholderString("Kingston"));
    EXPECT_FALSE(isPlaceholderString(""));
}

} // namespace
} // namespace Platform::Smbios
