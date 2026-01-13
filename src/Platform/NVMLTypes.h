#pragma once

/// @file NVMLTypes.h
/// @brief Shared NVML type definitions for dynamic loading without requiring nvml.h
///
/// These definitions match the NVIDIA NVML API and allow TaskSmack to work with
/// NVIDIA GPUs without compile-time dependency on the CUDA toolkit.
/// Both Linux and Windows NVMLGPUProbe implementations use these types.

#include <cstdint>

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

/// NVML process information structure
/// Using the v3 structure with MIG fields for compatibility with newer drivers.
/// Older drivers may not fill gpuInstanceId/computeInstanceId but the structure
/// size remains compatible.
struct nvmlProcessInfo_t
{
    unsigned int pid;
    std::uint64_t usedGpuMemory;
    unsigned int gpuInstanceId;     // For MIG (Multi-Instance GPU) support
    unsigned int computeInstanceId; // For MIG support
};

// NOLINTEND(readability-identifier-naming)

} // namespace Platform::NVML
