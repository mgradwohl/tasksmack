#pragma once

#include <cstdint>
#include <format>
#include <string>

namespace Platform
{

/// Convert a DXGI vendor ID to a vendor name string.
[[nodiscard]] inline std::string vendorIdToName(uint32_t vendorId)
{
    switch (vendorId)
    {
    case 0x10DE:
        return "NVIDIA";
    case 0x1002:
    case 0x1022:
        return "AMD";
    case 0x8086:
    case 0x8087:
        return "Intel";
    default:
        return "Unknown";
    }
}

/// Convert a LUID's High/Low parts to PDH-compatible format: "GPU_0x{HighPart}_0x{LowPart}".
/// PDH instance names use this format for GPU identification; taking the parts as plain
/// integers (rather than the Win32 LUID struct) keeps this header includable from tests
/// without pulling in windows.h.
[[nodiscard]] inline std::string luidToPdhFormat(uint32_t luidHighPart, uint32_t luidLowPart)
{
    return std::format("GPU_0x{:08X}_0x{:08X}", luidHighPart, luidLowPart);
}

/// Pure decision logic behind DXGIGPUProbe::isIntegratedGPU, taking the relevant
/// DXGI_ADAPTER_DESC1 fields directly so the vendor/VRAM-threshold branches can be unit
/// tested without a real (or COM-mocked) IDXGIAdapter1.
/// @param vendorId DXGI_ADAPTER_DESC1::VendorId
/// @param flags DXGI_ADAPTER_DESC1::Flags (DXGI_ADAPTER_FLAG_SOFTWARE = 0x2)
/// @param dedicatedVideoMemory DXGI_ADAPTER_DESC1::DedicatedVideoMemory
[[nodiscard]] inline bool isIntegratedGPUFromDesc(uint32_t vendorId, uint32_t flags, uint64_t dedicatedVideoMemory)
{
    // DXGI_ADAPTER_FLAG_SOFTWARE = 0x2
    constexpr uint32_t SOFTWARE_FLAG = 2;
    if ((flags & SOFTWARE_FLAG) != 0)
    {
        return false; // Software adapters are not "integrated" in the discrete/integrated sense
    }

    // Intel integrated GPUs typically have vendor ID 0x8086.
    // Intel UHD/Iris integrated graphics have lower dedicated video memory.
    if (vendorId == 0x8086)
    {
        // Intel GPUs with < 512MB dedicated VRAM are likely integrated
        return dedicatedVideoMemory < (512ULL * 1024 * 1024);
    }

    // AMD APUs (integrated) have vendor ID 0x1002 but lower dedicated memory.
    if (vendorId == 0x1002)
    {
        // AMD integrated GPUs typically have < 1GB dedicated VRAM
        return dedicatedVideoMemory < (1024ULL * 1024 * 1024);
    }

    // NVIDIA doesn't make consumer integrated GPUs (Tegra is different architecture).
    // Assume discrete for NVIDIA.
    return false;
}

} // namespace Platform
