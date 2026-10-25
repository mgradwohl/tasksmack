#include "Platform/ISystemInfoProbe.h"
#include "Platform/PartitionTable.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

// The partition layout parsers (#1632) on arbitrary bytes: as a DRIVE_LAYOUT_INFORMATION_EX buffer from
// IOCTL_DISK_GET_DRIVE_LAYOUT_EX, and as udev's ID_PART_ENTRY_TYPE text. Nothing may read past the input,
// and every partition read must lie in an entry the buffer holds.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) // NOLINT(readability-identifier-naming)
{
    const std::span<const std::byte> bytes = std::as_bytes(std::span<const std::uint8_t>(data, size));
    if (const std::optional<Platform::PartitionTable::DriveLayout> layout = Platform::PartitionTable::parseDriveLayout(bytes))
    {
        const std::size_t fit = (size - Platform::PartitionTable::LAYOUT_HEADER_BYTES) / Platform::PartitionTable::LAYOUT_ENTRY_BYTES;
        if (layout->partitions.size() > fit)
        {
            __builtin_trap();
        }
        for (const Platform::Partition& partition : layout->partitions)
        {
            if (partition.number == 0 || partition.sizeBytes == 0)
            {
                __builtin_trap();
            }
        }
    }
    if (bytes.size() >= 16)
    {
        static_cast<void>(Platform::PartitionTable::formatGuid(bytes.first<16>()));
    }
    const std::string_view text(reinterpret_cast<const char*>(data), size);
    Platform::Partition partition;
    Platform::PartitionTable::setType(partition, text);
    static_cast<void>(Platform::PartitionTable::gptTypeName(text));
    return 0;
}
