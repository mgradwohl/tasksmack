#pragma once

/// @file NVMLTypes.h
/// @brief Shared NVML type definitions for dynamic loading without requiring nvml.h
///
/// These definitions match the NVIDIA NVML API and allow TaskSmack to work with
/// NVIDIA GPUs without compile-time dependency on the CUDA toolkit.
/// Both Linux and Windows NVMLGPUProbe implementations use these types.

#include <cstdint>
#include <cstring>
#include <iterator>
#include <optional>
#include <string_view>

namespace Platform::NVML
{

// NVML device handle (opaque pointer)
// NOLINTBEGIN(readability-identifier-naming) - these types mirror NVIDIA NVML C API naming
using nvmlDevice_t = void*;
// NOLINTEND(readability-identifier-naming)

// NVML return codes
// These enums must match NVML's ABI exactly (C-style enums, unsigned int).
// Using enum class would break dynamic loading compatibility. The
// performance-enum-size suppression is intentional to keep the ABI-aligned
// underlying size that NVML expects.
// NOLINTBEGIN(performance-enum-size,cppcoreguidelines-use-enum-class,readability-identifier-naming)
enum nvmlReturn_t : unsigned int
{
    NVML_SUCCESS = 0,
    NVML_ERROR_UNINITIALIZED = 1,
    NVML_ERROR_INVALID_ARGUMENT = 2,
    NVML_ERROR_NOT_SUPPORTED = 3,
    NVML_ERROR_NO_PERMISSION = 4,
    NVML_ERROR_ALREADY_INITIALIZED = 5,
    NVML_ERROR_NOT_FOUND = 6,
    NVML_ERROR_INSUFFICIENT_SIZE = 7,
    NVML_ERROR_INSUFFICIENT_POWER = 8,
    NVML_ERROR_DRIVER_NOT_LOADED = 9,
    NVML_ERROR_TIMEOUT = 10,
    NVML_ERROR_IRQ_ISSUE = 11,
    NVML_ERROR_LIBRARY_NOT_FOUND = 12,
    NVML_ERROR_FUNCTION_NOT_FOUND = 13,
    NVML_ERROR_CORRUPTED_INFOROM = 14,
    NVML_ERROR_GPU_IS_LOST = 15,
    NVML_ERROR_RESET_REQUIRED = 16,
    NVML_ERROR_OPERATING_SYSTEM = 17,
    NVML_ERROR_LIB_RM_VERSION_MISMATCH = 18,
    NVML_ERROR_IN_USE = 19,
    NVML_ERROR_MEMORY = 20,
    NVML_ERROR_NO_DATA = 21,
    NVML_ERROR_VGPU_ECC_NOT_SUPPORTED = 22,
    NVML_ERROR_INSUFFICIENT_RESOURCES = 23,
    NVML_ERROR_UNKNOWN = 999
};

// NVML temperature sensor types
enum nvmlTemperatureSensors_t : unsigned int
{
    NVML_TEMPERATURE_GPU = 0
};

// NVML clock types
enum nvmlClockType_t : unsigned int
{
    NVML_CLOCK_GRAPHICS = 0,
    NVML_CLOCK_SM = 1,
    NVML_CLOCK_MEM = 2,
    NVML_CLOCK_VIDEO = 3
};

// NVML PCIe utilization counter types
enum nvmlPcieUtilCounter_t : unsigned int
{
    NVML_PCIE_UTIL_TX_BYTES = 0,
    NVML_PCIE_UTIL_RX_BYTES = 1,
    NVML_PCIE_UTIL_COUNT = 2
};
// NOLINTEND(performance-enum-size,cppcoreguidelines-use-enum-class,readability-identifier-naming)

// NVML buffer size constants
inline constexpr unsigned int NVML_DEVICE_NAME_BUFFER_SIZE = 64;
inline constexpr unsigned int NVML_DEVICE_UUID_BUFFER_SIZE = 80;
inline constexpr unsigned int NVML_SYSTEM_DRIVER_VERSION_BUFFER_SIZE = 80;
inline constexpr unsigned int NVML_DEVICE_VBIOS_VERSION_BUFFER_SIZE = 32;

// NOLINTBEGIN(readability-identifier-naming) - these structs mirror NVIDIA NVML C API naming

/// NVML memory information structure
struct nvmlMemory_t
{
    std::uint64_t total;
    std::uint64_t free;
    std::uint64_t used;
};

/// NVML utilization rates structure
struct nvmlUtilization_t
{
    unsigned int gpu;
    unsigned int memory;
};

/// Opaque element type of the array nvmlDevice{Compute,Graphics}RunningProcesses{,_v2,_v3}
/// write (#1306). Which struct each entry is depends on the symbol: the legacy unversioned export
/// writes 16-byte nvmlProcessInfo_v1_t entries, _v2/_v3 write 24-byte nvmlProcessInfo_v2_t ones
/// (#1092, #1313), parsed by size in NVMLRunningProcesses.h. The library declares the parameter as
/// a pointer to that struct; any object pointer has the same ABI, so callers that size the entries
/// at run time pass their byte buffer through this one incomplete type. Every definition of these
/// entry points that a probe may call (the test mock included) must declare its
/// third parameter as nvmlProcessInfoEntries*, so the call matches the callee's declared type
/// (UBSan -fsanitize=function checks it).
struct nvmlProcessInfoEntries;

/// NVML PCI information (nvmlDeviceGetPciInfo_v3 / _v2): the device's PCI location and ids.
struct nvmlPciInfo_t
{
    char busIdLegacy[16]; // NOLINT(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays) - mirrors the C struct
    unsigned int domain;
    unsigned int bus;
    unsigned int device;
    unsigned int pciDeviceId; // (device ID << 16) | vendor ID
    unsigned int pciSubSystemId;
    char busId[32]; // NOLINT(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays) - mirrors the C struct
};

// NOLINTEND(readability-identifier-naming)

/// The PCI function number of the device @p pci describes. nvmlPciInfo_t has no field for it, so it
/// comes from busId's ".F" suffix ("00000000:01:00.0"); nullopt when busId is empty or doesn't end in
/// a function digit (0-7).
[[nodiscard]] inline std::optional<std::uint32_t> pciFunction(const nvmlPciInfo_t& pci)
{
    const std::string_view busId(std::data(pci.busId), ::strnlen(std::data(pci.busId), std::size(pci.busId)));
    const auto dot = busId.rfind('.');
    if (dot == std::string_view::npos || dot + 2 != busId.size())
    {
        return std::nullopt;
    }
    const char digit = busId[dot + 1];
    if (digit < '0' || digit > '7')
    {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(digit - '0');
}

} // namespace Platform::NVML
