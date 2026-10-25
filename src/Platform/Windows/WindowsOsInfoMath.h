#pragma once

// The pure parts of WindowsSystemInfoProbe (#1512): the product name, display version and build
// strings from HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion, and the native architecture's
// name. Header-only and free of Windows headers, so their tests run on every platform.

#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace Platform::WindowsOsInfo
{

/// The first Windows 11 build. ProductName still says "Windows 10" on Windows 11.
inline constexpr std::uint32_t WINDOWS_11_FIRST_BUILD = 22000;

/// CurrentBuild ("26100") as a number; nullopt when it isn't one.
[[nodiscard]] inline std::optional<std::uint32_t> parseBuild(std::string_view text) noexcept
{
    std::uint32_t value = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || end != text.data() + text.size())
    {
        return std::nullopt;
    }
    return value;
}

/// The edition's name: ProductName with "Windows 10" made "Windows 11" from build 22000 on; without a
/// ProductName, "Windows <10|11> <EditionID>". Empty when neither is known.
[[nodiscard]] inline std::string productName(std::string_view productName, std::string_view editionId, std::optional<std::uint32_t> build)
{
    const bool eleven = build.has_value() && *build >= WINDOWS_11_FIRST_BUILD;
    if (productName.empty())
    {
        if (editionId.empty())
        {
            return {};
        }
        return std::string(eleven ? "Windows 11 " : "Windows 10 ") + std::string(editionId);
    }
    std::string name(productName);
    constexpr std::string_view TEN = "Windows 10";
    if (eleven && name.starts_with(TEN))
    {
        name.replace(0, TEN.size(), "Windows 11");
    }
    return name;
}

/// DisplayVersion ("25H2"), or ReleaseId ("2004") on builds before DisplayVersion existed.
[[nodiscard]] inline std::string displayVersion(std::string_view displayVersion, std::string_view releaseId)
{
    return std::string(displayVersion.empty() ? releaseId : displayVersion);
}

/// "CurrentBuild.UBR" ("26100.4652"), or CurrentBuild alone without a UBR.
[[nodiscard]] inline std::string buildString(std::string_view currentBuild, std::optional<std::uint32_t> ubr)
{
    if (currentBuild.empty() || !ubr.has_value())
    {
        return std::string(currentBuild);
    }
    return std::string(currentBuild) + "." + std::to_string(*ubr);
}

/// The name of an IMAGE_FILE_MACHINE_* value (IsWow64Process2's native machine); empty if unknown.
[[nodiscard]] constexpr std::string_view machineName(std::uint16_t machine) noexcept
{
    switch (machine)
    {
    case 0x8664: // IMAGE_FILE_MACHINE_AMD64
        return "x64";
    case 0xAA64: // IMAGE_FILE_MACHINE_ARM64
        return "ARM64";
    case 0x014C: // IMAGE_FILE_MACHINE_I386
        return "x86";
    case 0x01C4: // IMAGE_FILE_MACHINE_ARMNT
        return "ARM";
    default:
        return {};
    }
}

/// The name of a POWER_PLATFORM_ROLE (PowerDeterminePlatformRoleEx, #1513); empty for unspecified or
/// unknown.
[[nodiscard]] constexpr std::string_view platformRoleName(std::uint32_t role) noexcept
{
    switch (role)
    {
    case 1:
        return "Desktop";
    case 2:
        return "Mobile";
    case 3:
        return "Workstation";
    case 4:
        return "Enterprise server";
    case 5:
        return "Small office server";
    case 6:
        return "Appliance PC";
    case 7:
        return "Performance server";
    case 8:
        return "Slate";
    default:
        return {};
    }
}

} // namespace Platform::WindowsOsInfo
