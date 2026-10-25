#pragma once

// The root-only firmware files System Information reads through tasksmack-helper (#2310): the SMBIOS
// entry point and table, the serials and the UUID. Broker::FirmwareFile names each; this maps the name
// to its one path, so the helper reads only these. Standard library only, so the fixture tests build
// and run everywhere.

#include "Platform/BrokerProtocol.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>
#include <vector>

namespace Platform::Linux
{

/// The path of @p file under @p root ("/" in the helper, a fixture tree in tests).
[[nodiscard]] inline std::filesystem::path firmwareFilePath(const std::filesystem::path& root, Broker::FirmwareFile file)
{
    switch (file)
    {
    case Broker::FirmwareFile::SmbiosEntryPoint:
        return root / "sys/firmware/dmi/tables/smbios_entry_point";
    case Broker::FirmwareFile::SmbiosTable:
        return root / "sys/firmware/dmi/tables/DMI";
    case Broker::FirmwareFile::ProductSerial:
        return root / "sys/class/dmi/id/product_serial";
    case Broker::FirmwareFile::ProductUuid:
        return root / "sys/class/dmi/id/product_uuid";
    case Broker::FirmwareFile::BoardSerial:
        return root / "sys/class/dmi/id/board_serial";
    }
    return {};
}

/// @p file under @p root: its bytes, NotFound when it doesn't exist, or Unreadable when it can't be
/// read or is larger than Broker::MAX_FIRMWARE_FILE_BYTES.
[[nodiscard]] inline Broker::FirmwareFileReply readFirmwareFileAt(const std::filesystem::path& root, Broker::FirmwareFile file)
{
    const std::filesystem::path path = firmwareFilePath(root, file);
    std::error_code ec;
    if (path.empty() || !std::filesystem::exists(path, ec))
    {
        return {.status = Broker::FirmwareFileStatus::NotFound, .bytes = {}};
    }
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        return {.status = Broker::FirmwareFileStatus::Unreadable, .bytes = {}};
    }
    // sysfs reports a size of 4096 for every attribute, so read rather than trust file_size()
    std::vector<std::uint8_t> bytes;
    for (std::istreambuf_iterator<char> it(in), end; it != end; ++it)
    {
        if (bytes.size() == Broker::MAX_FIRMWARE_FILE_BYTES)
        {
            return {.status = Broker::FirmwareFileStatus::Unreadable, .bytes = {}};
        }
        bytes.push_back(static_cast<std::uint8_t>(*it));
    }
    if (in.bad())
    {
        return {.status = Broker::FirmwareFileStatus::Unreadable, .bytes = {}};
    }
    return {.status = Broker::FirmwareFileStatus::Ok, .bytes = std::move(bytes)};
}

} // namespace Platform::Linux
