#pragma once

#include <cstdint>
#include <format>
#include <optional>
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

/// The D3DKMT_ADAPTERTYPE bits shouldListAdapter() decides on, as plain bools so this header stays
/// free of the Windows kernel-mode headers.
struct AdapterTypeBits
{
    bool softwareDevice = false;
    bool indirectDisplayDevice = false;
};

/// Whether DXGIGPUProbe lists a DXGI adapter as a GPU. A software adapter (WARP, the Basic Render
/// Driver) is not one; nor is an indirect-display adapter (a DisplayLink dock, Miracast), which DXGI
/// reports under the name, vendor and device id of the GPU it renders on, so it showed as a
/// duplicate of that GPU and doubled the VRAM total (#1251). A failed adapter-type query keeps the
/// adapter: dropping a real GPU is worse than a possible duplicate.
/// @param softwareFlag DXGI_ADAPTER_DESC1::Flags has DXGI_ADAPTER_FLAG_SOFTWARE
/// @param adapterType The adapter's D3DKMT adapter type, or nullopt when it could not be read
[[nodiscard]] constexpr bool shouldListAdapter(bool softwareFlag, std::optional<AdapterTypeBits> adapterType)
{
    if (softwareFlag)
    {
        return false;
    }
    return !adapterType.has_value() || (!adapterType->softwareDevice && !adapterType->indirectDisplayDevice);
}

/// An adapter's memory size, from DXGI_ADAPTER_DESC1: the shared system memory an integrated GPU
/// may use, or a discrete GPU's dedicated VRAM. Fixed for the adapter's lifetime, unlike the
/// per-process budget QueryVideoMemoryInfo reports, which moved over time and made the GPU tab's
/// total disagree with its header (16 GB vs 17.2 GB on an Arc 140T, #1029).
[[nodiscard]] constexpr uint64_t adapterMemoryTotalBytes(bool isIntegrated, uint64_t dedicatedVideoMemory, uint64_t sharedSystemMemory)
{
    return isIntegrated ? sharedSystemMemory : dedicatedVideoMemory;
}

} // namespace Platform
