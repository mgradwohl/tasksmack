#pragma once

// Pure, injectable-function-pointer logic extracted from ROCmGPUProbe.cpp so it can be
// unit-tested directly, without going through dlopen()/the ROCm SMI mock library. See
// CONTRIBUTING.md's "extract the pure decision logic into a small header" pattern.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>

namespace Platform::ROCmGPUProbeMath
{

// ROCm SMI's rsmi_frequencies_t changed layout in ROCm 6.0: it gained a leading
// `bool has_deep_sleep` and grew from 32 to 33 frequencies (RSMI_MAX_NUM_FREQUENCIES, 32 normal
// plus 1 deep-sleep). Passing the old 264-byte struct to a ROCm 6 library let it write 16 bytes
// past a stack object on every clock read (#1088). Both layouts are mirrored here, and the probe
// hands the library a buffer larger than either, then decodes by the library's version.

// NOLINTBEGIN(readability-identifier-naming,cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays) - must match AMD ROCm SMI API ABI

/// rsmi_frequencies_t up to ROCm 5.x (librocm_smi64 major version 5 and earlier).
struct RsmiFrequenciesV5
{
    std::uint32_t num_supported;
    std::uint32_t current;
    std::uint64_t frequency[32];
};

/// rsmi_frequencies_t from ROCm 6.0 onward (librocm_smi64 major version 6 and later).
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

/// The layout to decode for a ROCm SMI library whose rsmi_version_get() reported `libraryMajor`.
/// An unknown version (the optional symbol is missing) is decoded as the current layout: the
/// buffer is oversized either way, so a wrong guess can misread a clock but never overflow.
[[nodiscard]] constexpr FrequenciesLayout frequenciesLayoutFor(std::optional<std::uint32_t> libraryMajor) noexcept
{
    return (libraryMajor.has_value() && *libraryMajor < 6U) ? FrequenciesLayout::V5 : FrequenciesLayout::V6;
}

namespace Detail
{
template<typename Layout> [[nodiscard]] std::optional<std::uint64_t> currentFrequencyHz(const RsmiFrequenciesBuffer& buffer)
{
    Layout decoded{};
    std::memcpy(&decoded, buffer.bytes.data(), sizeof(decoded));
    const std::size_t capacity = std::size(decoded.frequency);
    if (decoded.current >= decoded.num_supported || decoded.current >= capacity)
    {
        return std::nullopt;
    }
    return decoded.frequency[decoded.current];
}
} // namespace Detail

/// The current frequency in Hz from a filled buffer, or nullopt when the reported current index
/// is out of range (of the supported count, or of the array itself).
[[nodiscard]] inline std::optional<std::uint64_t> currentFrequencyHz(const RsmiFrequenciesBuffer& buffer, FrequenciesLayout layout)
{
    return layout == FrequenciesLayout::V5 ? Detail::currentFrequencyHz<RsmiFrequenciesV5>(buffer)
                                           : Detail::currentFrequencyHz<RsmiFrequenciesV6>(buffer);
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

} // namespace Platform::ROCmGPUProbeMath
