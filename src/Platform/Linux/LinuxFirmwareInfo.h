#pragma once

// The Linux Firmware & board facts (#1513), read under an injected root ("/" in the app, a fixture tree
// in tests): /sys/class/dmi/id, UEFI from /sys/firmware/efi, and the SMBIOS version from the entry
// point. The serials, the UUID and the entry point are root-only on most systems. Standard library only,
// so the fixture tests build and run everywhere.

#include "Platform/ISystemInfoProbe.h"
#include "Platform/Linux/LinuxOsInfo.h"
#include "Platform/SmbiosParser.h"

#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace Platform::LinuxFirmwareInfo
{

/// Whether a file can be opened for reading; tests inject a stand-in for a root-only file.
using ReadablePredicate = std::function<bool(const std::filesystem::path&)>;

[[nodiscard]] inline bool canOpen(const std::filesystem::path& path)
{
    return std::ifstream(path, std::ios::binary).is_open();
}

/// A chassis_type file's number ("10"); 0 when it isn't one.
[[nodiscard]] inline std::uint8_t parseChassisType(std::string_view text) noexcept
{
    unsigned value = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    return ec == std::errc{} && value <= 0xFF ? static_cast<std::uint8_t>(value) : std::uint8_t{0};
}

/// The facts under @p root into @p info.
inline void readFirmwareFacts(const std::filesystem::path& root, FirmwareInfo& info, const ReadablePredicate& readable = canOpen)
{
    const std::filesystem::path dmi = root / "sys/class/dmi/id";
    std::error_code ec;
    // A file that exists but can't be opened is root-only (0400 in sysfs).
    const auto read = [&](const char* name, bool* needsAdmin = nullptr)
    {
        const std::filesystem::path path = dmi / name;
        if (!readable(path))
        {
            if (needsAdmin != nullptr && std::filesystem::exists(path, ec))
            {
                *needsAdmin = true;
            }
            return std::string{};
        }
        return LinuxOsInfo::readLine(path);
    };

    info.available = true;
    info.systemManufacturer = read("sys_vendor");
    info.systemModel = read("product_name");
    info.systemVersion = read("product_version");
    info.systemSku = read("product_sku");
    info.systemFamily = read("product_family");
    info.systemSerial = read("product_serial", &info.identifiersNeedAdmin);
    info.systemUuid = read("product_uuid", &info.identifiersNeedAdmin);
    info.biosVendor = read("bios_vendor");
    info.biosVersion = read("bios_version");
    info.biosReleaseDate = read("bios_date");
    info.embeddedControllerVersion = read("ec_firmware_release");
    info.boardManufacturer = read("board_vendor");
    info.boardProduct = read("board_name");
    info.boardVersion = read("board_version");
    info.boardSerial = read("board_serial", &info.identifiersNeedAdmin);
    info.chassisManufacturer = read("chassis_vendor");
    const std::uint8_t chassis = parseChassisType(read("chassis_type"));
    info.chassisType = std::string(Smbios::chassisTypeName(chassis));
    info.platformRole = std::string(Smbios::platformRoleFromChassis(chassis));

    // /sys/firmware/efi exists only when the kernel was booted by UEFI.
    if (std::filesystem::exists(root / "sys/firmware/efi", ec))
    {
        info.firmwareMode = FirmwareMode::Uefi;
    }
    else if (std::filesystem::exists(root / "sys/firmware", ec))
    {
        info.firmwareMode = FirmwareMode::Legacy;
    }

    const std::filesystem::path entryPoint = root / "sys/firmware/dmi/tables/smbios_entry_point";
    if (readable(entryPoint))
    {
        const std::string text = LinuxOsInfo::readFile(entryPoint);
        const std::vector<std::uint8_t> bytes(text.begin(), text.end());
        info.smbiosVersion = Smbios::entryPointVersion(bytes);
    }
    else
    {
        info.smbiosVersionNeedsAdmin = std::filesystem::exists(entryPoint, ec);
    }
}

} // namespace Platform::LinuxFirmwareInfo
