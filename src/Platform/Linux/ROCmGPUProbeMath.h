#pragma once

// Pure, injectable-function-pointer logic extracted from ROCmGPUProbe.cpp so it can be
// unit-tested directly, without going through dlopen()/the ROCm SMI mock library. See
// CONTRIBUTING.md's "extract the pure decision logic into a small header" pattern.

#include "Platform/Linux/PciRuntimePm.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <type_traits>

namespace Platform::ROCmGPUProbeMath
{

// ROCm SMI's rsmi_frequencies_t changed layout in ROCm 6.0: it gained a leading
// `bool has_deep_sleep` and grew from 32 to 33 frequencies (RSMI_MAX_NUM_FREQUENCIES, 32 normal
// plus 1 deep-sleep). Passing the old 264-byte struct to a ROCm 6 library let it write 16 bytes
// past a stack object on every clock read (#1088). Both layouts are mirrored here, and the probe
// hands the library a buffer larger than either, then decodes it.

// NOLINTBEGIN(readability-identifier-naming,cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays) - must match AMD ROCm SMI API ABI

/// rsmi_frequencies_t up to ROCm 5.x.
struct RsmiFrequenciesV5
{
    std::uint32_t num_supported;
    std::uint32_t current;
    std::uint64_t frequency[32];
};

/// rsmi_frequencies_t from ROCm 6.0 onward.
struct RsmiFrequenciesV6
{
    bool has_deep_sleep;
    std::uint32_t num_supported;
    std::uint32_t current;
    std::uint64_t frequency[33];
};

// NOLINTEND(readability-identifier-naming,cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)

static_assert(sizeof(RsmiFrequenciesV5) == 264, "ROCm 5 rsmi_frequencies_t is 264 bytes");
static_assert(sizeof(RsmiFrequenciesV6) == 280, "ROCm 6 rsmi_frequencies_t is 280 bytes");

/// What the probe passes to rsmi_dev_gpu_clk_freq_get(): larger than every known layout, so a
/// library whose struct is bigger than the one we decode still cannot write past it.
struct alignas(alignof(std::uint64_t)) RsmiFrequenciesBuffer
{
    std::array<std::byte, 512> bytes{};
};

static_assert(sizeof(RsmiFrequenciesBuffer) >= sizeof(RsmiFrequenciesV6));

enum class FrequenciesLayout : std::uint8_t
{
    V5,
    V6
};

/// The layout to try first for a ROCm SMI library whose rsmi_version_get() reported
/// `libraryMajor`. Only a preference: a library built without its git-tag metadata reports a
/// generated version (1.0.0) whatever layout it writes, so currentFrequencyHz() validates the
/// preferred layout and falls back to the other.
[[nodiscard]] constexpr FrequenciesLayout frequenciesLayoutFor(std::optional<std::uint32_t> libraryMajor) noexcept
{
    return (libraryMajor.has_value() && *libraryMajor < 6U) ? FrequenciesLayout::V5 : FrequenciesLayout::V6;
}

namespace Detail
{

// Where each layout keeps its fields. Decoding reads them by offset rather than copying the buffer
// into the mirror struct: a V5 buffer's first byte is not a valid bool, so reading it as
// RsmiFrequenciesV6::has_deep_sleep would be undefined behaviour.
struct FieldOffsets
{
    std::size_t numSupported;
    std::size_t current;
    std::size_t frequency;
    std::size_t capacity;
};

inline constexpr FieldOffsets V5_OFFSETS{.numSupported = offsetof(RsmiFrequenciesV5, num_supported),
                                         .current = offsetof(RsmiFrequenciesV5, current),
                                         .frequency = offsetof(RsmiFrequenciesV5, frequency),
                                         .capacity = std::extent_v<decltype(RsmiFrequenciesV5::frequency)>};
inline constexpr FieldOffsets V6_OFFSETS{.numSupported = offsetof(RsmiFrequenciesV6, num_supported),
                                         .current = offsetof(RsmiFrequenciesV6, current),
                                         .frequency = offsetof(RsmiFrequenciesV6, frequency),
                                         .capacity = std::extent_v<decltype(RsmiFrequenciesV6::frequency)>};

/// Clocks above this are not a real reading (no GPU clock is near 100 GHz).
inline constexpr std::uint64_t MAX_PLAUSIBLE_HZ = 100'000'000'000ULL;

template<typename T> [[nodiscard]] T readAt(const RsmiFrequenciesBuffer& buffer, std::size_t offset)
{
    T value{};
    std::memcpy(&value, buffer.bytes.data() + offset, sizeof(value));
    return value;
}

/// The current frequency if the buffer is self-consistent under `offsets`: a supported count
/// that fits the array, a current index below it, and a non-zero, plausible frequency there.
[[nodiscard]] inline std::optional<std::uint64_t> plausibleFrequencyHz(const RsmiFrequenciesBuffer& buffer, const FieldOffsets& offsets)
{
    const auto numSupported = readAt<std::uint32_t>(buffer, offsets.numSupported);
    const auto current = readAt<std::uint32_t>(buffer, offsets.current);
    if (numSupported == 0 || numSupported > offsets.capacity || current >= numSupported)
    {
        return std::nullopt;
    }
    const auto hz = readAt<std::uint64_t>(buffer, offsets.frequency + (current * sizeof(std::uint64_t)));
    if (hz == 0 || hz > MAX_PLAUSIBLE_HZ)
    {
        return std::nullopt;
    }
    return hz;
}

} // namespace Detail

/// The current frequency in Hz from a filled buffer. The `preferred` layout is tried first and
/// the other second; each must be self-consistent (see plausibleFrequencyHz). The two layouts
/// put different fields at the same offsets, so a buffer written in one layout is not
/// self-consistent when read as the other. nullopt when neither fits.
[[nodiscard]] inline std::optional<std::uint64_t> currentFrequencyHz(const RsmiFrequenciesBuffer& buffer, FrequenciesLayout preferred)
{
    const auto& first = (preferred == FrequenciesLayout::V5) ? Detail::V5_OFFSETS : Detail::V6_OFFSETS;
    const auto& second = (preferred == FrequenciesLayout::V5) ? Detail::V6_OFFSETS : Detail::V5_OFFSETS;
    if (const auto hz = Detail::plausibleFrequencyHz(buffer, first))
    {
        return hz;
    }
    return Detail::plausibleFrequencyHz(buffer, second);
}

// Mirrors ROCm SMI's RSMI_STATUS_SUCCESS (0) without depending on ROCmGPUProbe.cpp's
// anonymous-namespace rsmi_status_t/RSMI_STATUS_SUCCESS definitions.
inline constexpr std::uint32_t kRsmiStatusSuccess = 0;

using StatusStringFn = const char* (*) (std::uint32_t);
using DeviceIdLookupFn = std::uint32_t (*)(std::uint32_t, std::uint64_t*);

/// Resolves a ROCm SMI status code to a human-readable string via the (possibly unresolved)
/// rsmi_status_string function pointer, falling back to "Unknown ROCm error N" when the
/// symbol failed to load or the library itself returns null.
[[nodiscard]] inline std::string resolveErrorString(std::uint32_t result, StatusStringFn statusStringFn)
{
    if (statusStringFn != nullptr)
    {
        const char* errStr = statusStringFn(result);
        if (errStr != nullptr)
        {
            return errStr;
        }
    }
    return "Unknown ROCm error " + std::to_string(result);
}

/// Derives a device identifier via the fallback chain: uniqueId -> pciId -> "amd_<index>".
/// Must match enumerateGPUs()'s chain exactly so GPUCounters::gpuId correlates to
/// GPUInfo::id in the domain layer. Either lookup function pointer may be null (optional
/// symbol failed to load) or may fail at call time (non-success status).
[[nodiscard]] inline std::string deriveDeviceId(std::uint32_t deviceIdx, DeviceIdLookupFn uniqueIdFn, DeviceIdLookupFn pciIdFn)
{
    std::uint64_t uniqueId = 0;
    if (uniqueIdFn != nullptr && uniqueIdFn(deviceIdx, &uniqueId) == kRsmiStatusSuccess)
    {
        return std::to_string(uniqueId);
    }

    std::uint64_t pciId = 0;
    if (pciIdFn != nullptr && pciIdFn(deviceIdx, &pciId) == kRsmiStatusSuccess)
    {
        return std::to_string(pciId);
    }

    return "amd_" + std::to_string(deviceIdx);
}

/// The sysfs name ("0000:03:00.0") of the PCI device whose rsmi_dev_pci_id_get() BDF id is `bdfId`,
/// for its power/runtime_status (#1117). ROCm SMI packs it as
/// (domain << 32) | (bus << 8) | (device << 3) | function.
[[nodiscard]] inline std::string sysfsPciAddress(std::uint64_t bdfId)
{
    constexpr unsigned DOMAIN_SHIFT = 32U;
    constexpr unsigned BUS_SHIFT = 8U;
    constexpr unsigned DEVICE_SHIFT = 3U;
    constexpr std::uint64_t BUS_MASK = 0xFFU;
    constexpr std::uint64_t DEVICE_MASK = 0x1FU;
    constexpr std::uint64_t FUNCTION_MASK = 0x7U;
    return PciRuntimePm::pciAddress(static_cast<std::uint32_t>(bdfId >> DOMAIN_SHIFT),
                                    static_cast<std::uint32_t>((bdfId >> BUS_SHIFT) & BUS_MASK),
                                    static_cast<std::uint32_t>((bdfId >> DEVICE_SHIFT) & DEVICE_MASK),
                                    static_cast<std::uint32_t>(bdfId & FUNCTION_MASK));
}

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
/// With neither, the GPU is taken as discrete, as ROCm SMI's devices are most often.
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

} // namespace Platform::ROCmGPUProbeMath
