#pragma once

// Whether an AMD GPU is an APU's integrated graphics (#1266, #1344), shared by the ROCm SMI and DRM
// probes so both class an AMD GPU by the same rule. Everything read here is a sysfs attribute amdgpu
// (or the PCI core) cached when it probed the device, so it never wakes a runtime-suspended GPU (#1117).

#include "Platform/Linux/PciDisplayDevices.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace Platform::AmdApu
{

/// A graphics-core (GC) IP version, as amdgpu publishes it under
/// <pci device>/ip_discovery/die/0/GC/0/{major,minor,revision} on ASICs it sets up from the IP
/// discovery table (Linux 5.17+).
struct GcIpVersion
{
    std::uint32_t major = 0;
    std::uint32_t minor = 0;
    std::uint32_t revision = 0;

    friend constexpr bool operator==(const GcIpVersion&, const GcIpVersion&) = default;
};

/// The GC IP versions amdgpu itself flags AMD_IS_APU for (amdgpu_discovery_set_ip_blocks()): Raven,
/// Raven2/Picasso, Renoir/Cezanne, Cyan Skillfish, Van Gogh, Rembrandt, Raphael, Mendocino,
/// Phoenix/Hawk Point, Phoenix2, Strix Point, Strix Halo, Krackan Point and the later GC 11.5.x/11.7.x
/// parts. Mirrors the AMD_IS_APU switch in the kernel's drivers/gpu/drm/amd/amdgpu/amdgpu_discovery.c
/// (checked against torvalds/linux master, 2026-10-06). A newer APU generation needs adding here
/// (until then it reads as discrete, as every AMD GPU did before #1266).
inline constexpr std::array<GcIpVersion, 19> APU_GC_IP_VERSIONS{{
    {.major = 9, .minor = 1, .revision = 0},  {.major = 9, .minor = 2, .revision = 2},  {.major = 9, .minor = 3, .revision = 0},
    {.major = 10, .minor = 1, .revision = 3}, {.major = 10, .minor = 1, .revision = 4}, {.major = 10, .minor = 3, .revision = 1},
    {.major = 10, .minor = 3, .revision = 3}, {.major = 10, .minor = 3, .revision = 6}, {.major = 10, .minor = 3, .revision = 7},
    {.major = 11, .minor = 0, .revision = 1}, {.major = 11, .minor = 0, .revision = 4}, {.major = 11, .minor = 5, .revision = 0},
    {.major = 11, .minor = 5, .revision = 1}, {.major = 11, .minor = 5, .revision = 2}, {.major = 11, .minor = 5, .revision = 3},
    {.major = 11, .minor = 5, .revision = 4}, {.major = 11, .minor = 5, .revision = 6}, {.major = 11, .minor = 7, .revision = 0},
    {.major = 11, .minor = 7, .revision = 1},
}};

/// An inclusive range of AMD PCI device ids.
struct PciDeviceIdRange
{
    std::uint16_t first = 0;
    std::uint16_t last = 0;
};

/// AMD APU PCI device ids, for a kernel that doesn't publish ip_discovery (older kernels, and the
/// pre-IP-discovery ASICs amdgpu flags AMD_IS_APU by device id in its pciidlist: Kaveri, Kabini,
/// Mullins, Carrizo, Stoney, Raven, Renoir, Van Gogh, Yellow Carp, Cyan Skillfish), plus the
/// IP-discovery APUs' ids (Raphael, Mendocino, Phoenix, Phoenix2, Strix Point, Strix Halo, Krackan).
/// The pre-IP-discovery entries mirror the AMD_IS_APU rows of the kernel's amdgpu_drv.c pciidlist
/// (checked against torvalds/linux master, 2026-10-06); Kaveri's ids have gaps, so it is listed in runs.
inline constexpr std::array<PciDeviceIdRange, 34> APU_PCI_DEVICE_IDS{{
    {.first = 0x1304, .last = 0x1307}, // Kaveri
    {.first = 0x1309, .last = 0x1313}, // Kaveri
    {.first = 0x1315, .last = 0x1318}, // Kaveri
    {.first = 0x131B, .last = 0x131D}, // Kaveri
    {.first = 0x9830, .last = 0x983F}, // Kabini
    {.first = 0x9850, .last = 0x985F}, // Mullins
    {.first = 0x9870, .last = 0x9870}, // Carrizo
    {.first = 0x9874, .last = 0x9877}, // Carrizo
    {.first = 0x98E4, .last = 0x98E4}, // Stoney
    {.first = 0x15D8, .last = 0x15D8}, // Picasso / Raven2
    {.first = 0x15DD, .last = 0x15DD}, // Raven
    {.first = 0x15E7, .last = 0x15E7}, // Barcelo
    {.first = 0x1636, .last = 0x1636}, // Renoir
    {.first = 0x1638, .last = 0x1638}, // Cezanne
    {.first = 0x164C, .last = 0x164C}, // Lucienne
    {.first = 0x163F, .last = 0x163F}, // Van Gogh
    {.first = 0x164D, .last = 0x164D}, // Yellow Carp
    {.first = 0x1681, .last = 0x1681}, // Rembrandt
    {.first = 0x13DB, .last = 0x13DB}, // Cyan Skillfish
    {.first = 0x13F9, .last = 0x13FC}, // Cyan Skillfish
    {.first = 0x13FE, .last = 0x13FE}, // Cyan Skillfish
    {.first = 0x143F, .last = 0x143F}, // Cyan Skillfish
    {.first = 0x164E, .last = 0x164E}, // Raphael
    {.first = 0x1506, .last = 0x1506}, // Mendocino
    {.first = 0x15BF, .last = 0x15BF}, // Phoenix
    {.first = 0x15C8, .last = 0x15C8}, // Phoenix2
    {.first = 0x150E, .last = 0x150E}, // Strix Point
    {.first = 0x1586, .last = 0x1586}, // Strix Halo
    // IP-discovery APUs the kernel identifies by GC version rather than a pciidlist row; ids checked
    // against the PCI ID Repository (pci-ids.ucw.cz, 2026-10-06).
    {.first = 0x1435, .last = 0x1435}, // Sephiroth (Van Gogh, Steam Deck OLED)
    {.first = 0x13C0, .last = 0x13C0}, // Granite Ridge
    {.first = 0x1900, .last = 0x1901}, // Hawk Point 1 / 2
    {.first = 0x1114, .last = 0x1114}, // Krackan
    {.first = 0x1902, .last = 0x1902}, // Krackan 2
}};

/// Whether an AMD GPU is an APU's integrated graphics (#1266). The GC IP version decides when
/// amdgpu publishes it, since that is the signal the kernel's own AMD_IS_APU flag comes from; a
/// discrete GC version is discrete whatever the device id. Without it the PCI device id decides.
/// With neither, the GPU is taken as discrete.
[[nodiscard]] constexpr bool isAmdApu(std::optional<GcIpVersion> gcVersion, std::optional<std::uint16_t> pciDeviceId) noexcept
{
    if (gcVersion.has_value())
    {
        return std::ranges::find(APU_GC_IP_VERSIONS, *gcVersion) != APU_GC_IP_VERSIONS.end();
    }
    if (pciDeviceId.has_value())
    {
        const std::uint16_t id = *pciDeviceId;
        return std::ranges::any_of(APU_PCI_DEVICE_IDS,
                                   [id](const PciDeviceIdRange& range) { return id >= range.first && id <= range.last; });
    }
    return false;
}

/// Reads a sysfs decimal attribute ("11\n"); nullopt if it can't be read or isn't a number.
[[nodiscard]] inline std::optional<std::uint32_t> readDecimalAttribute(const std::filesystem::path& path)
{
    std::ifstream file(path);
    std::string text;
    if (!file.is_open() || !std::getline(file, text))
    {
        return std::nullopt;
    }
    std::string_view digits(text);
    while (!digits.empty() && (digits.back() == '\r' || digits.back() == ' '))
    {
        digits.remove_suffix(1);
    }
    std::uint32_t value = 0;
    const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), value);
    if (error != std::errc{} || end != digits.data() + digits.size() || digits.empty())
    {
        return std::nullopt;
    }
    return value;
}

/// The graphics core's IP version from amdgpu's ip_discovery sysfs tree under the PCI device
/// directory `devicePath` (#1266). The IP is listed both by name (GC) and by hardware id (11);
/// nullopt on kernels or ASICs without the tree.
[[nodiscard]] inline std::optional<GcIpVersion> readGcIpVersion(const std::string& devicePath)
{
    for (const char* gcDir : {"/ip_discovery/die/0/GC/0", "/ip_discovery/die/0/11/0"})
    {
        const std::filesystem::path dir = devicePath + gcDir;
        const auto major = readDecimalAttribute(dir / "major");
        const auto minor = readDecimalAttribute(dir / "minor");
        const auto revision = readDecimalAttribute(dir / "revision");
        if (major.has_value() && minor.has_value() && revision.has_value())
        {
            return GcIpVersion{.major = *major, .minor = *minor, .revision = *revision};
        }
    }
    return std::nullopt;
}

/// The PCI device id from the PCI device directory's `device` attribute ("0x15bf"), or nullopt if
/// it can't be read or isn't a 16-bit, non-zero id.
[[nodiscard]] inline std::optional<std::uint16_t> readPciDeviceId(const std::string& devicePath)
{
    constexpr std::uint32_t MAX_PCI_DEVICE_ID = 0xFFFFU;
    if (std::uint32_t id = 0; PciDisplayDevices::readHexAttribute(devicePath + "/device", id) && id != 0 && id <= MAX_PCI_DEVICE_ID)
    {
        return static_cast<std::uint16_t>(id); // Range-checked just above
    }
    return std::nullopt;
}

/// isAmdApu() for the AMD GPU whose PCI device directory is `devicePath`, from its sysfs attributes.
[[nodiscard]] inline bool isAmdApuDevice(const std::string& devicePath)
{
    return isAmdApu(readGcIpVersion(devicePath), readPciDeviceId(devicePath));
}

} // namespace Platform::AmdApu
