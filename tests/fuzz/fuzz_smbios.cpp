#include "Platform/ISystemInfoProbe.h"
#include "Platform/SmbiosParser.h"

#include <cstddef>
#include <cstdint>
#include <span>

// The SMBIOS parser (#1513, memory #1515) on arbitrary bytes: as a RawSMBIOSData blob (header, then table), as an
// entry point, and as a bare structure table with every typed decoder run on every structure. Nothing
// may read past the input.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) // NOLINT(readability-identifier-naming)
{
    const std::span<const std::uint8_t> bytes(data, size);

    Platform::FirmwareInfo info;
    static_cast<void>(Platform::Smbios::decodeFirmware(bytes, info));
    static_cast<void>(Platform::Smbios::entryPointVersion(bytes));

    // The memory decoders (#1515), on the blob and on the bare table.
    Platform::MemoryModulesInfo memory;
    static_cast<void>(Platform::Smbios::decodeMemoryModules(bytes, memory));
    Platform::Smbios::decodeMemoryTable(bytes, memory);

    const Platform::Smbios::Table table{.majorVersion = 3, .minorVersion = 4, .data = bytes};
    for (const Platform::Smbios::Structure& structure : Platform::Smbios::parseStructures(bytes))
    {
        static_cast<void>(Platform::Smbios::decodeBios(structure));
        static_cast<void>(Platform::Smbios::decodeSystem(structure, table));
        static_cast<void>(Platform::Smbios::decodeBaseboard(structure));
        static_cast<void>(Platform::Smbios::decodeChassis(structure));
        static_cast<void>(Platform::Smbios::decodeMemoryArray(structure));
        static_cast<void>(Platform::Smbios::decodeMemoryDevice(structure));
        static_cast<void>(structure.qword(0x0F));
        static_cast<void>(structure.dword(0x0C));
        static_cast<void>(structure.string(255));
    }
    return 0;
}
