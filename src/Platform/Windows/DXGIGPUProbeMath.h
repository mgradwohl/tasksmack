#pragma once

#include "Platform/GPUTypes.h"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

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
    case 0x5143: // Adreno on Windows on Arm (#1263)
        return "Qualcomm";
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

/// The LUID's high and low parts back out of luidToPdhFormat()'s "GPU_0x{HighPart}_0x{LowPart}"
/// (GPUInfo::luidId), or nullopt for a string of any other shape. The Windows probe opens an
/// adapter's kernel adapter by its LUID after enumeration, when its GPUInfo is all it has (#2586).
[[nodiscard]] inline std::optional<std::pair<std::uint32_t, std::uint32_t>> luidFromPdhFormat(std::string_view luidId)
{
    constexpr std::string_view PREFIX{"GPU_0x"};
    constexpr std::string_view SEPARATOR{"_0x"};
    constexpr std::size_t HEX_DIGITS = 8;
    if (luidId.size() != PREFIX.size() + HEX_DIGITS + SEPARATOR.size() + HEX_DIGITS || !luidId.starts_with(PREFIX) ||
        luidId.substr(PREFIX.size() + HEX_DIGITS, SEPARATOR.size()) != SEPARATOR)
    {
        return std::nullopt;
    }
    const auto parseHex = [](std::string_view digits) -> std::optional<std::uint32_t>
    {
        std::uint32_t value = 0;
        const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), value, 16);
        if (error != std::errc{} || end != digits.data() + digits.size())
        {
            return std::nullopt;
        }
        return value;
    };
    const auto high = parseHex(luidId.substr(PREFIX.size(), HEX_DIGITS));
    const auto low = parseHex(luidId.substr(PREFIX.size() + HEX_DIGITS + SEPARATOR.size(), HEX_DIGITS));
    if (!high.has_value() || !low.has_value())
    {
        return std::nullopt;
    }
    return std::pair{*high, *low};
}

/// The id of an adapter at this PCI location: "PCI_{bus}:{device}.{function}_{vendor id}:{device id}",
/// in hex (the function decimal, as PCI addresses print it). Where the adapter sits on the bus doesn't
/// change while it is present, or across a driver reset that gives it a new LUID (#1317). The id
/// names a slot and a model, not a physical card: an identical card swapped into the same slot gets
/// the same id and continues the old one's history, as a reconnect would, while a different model in
/// that slot, or the same model in another slot, gets a new id. The function number keeps two display functions of one multi-function
/// device (same vendor and device ids, same bus and device) from colliding, which sent the second to its LUID id; a location without one
/// (D3DKMT always reports it) is named by bus and device alone.
[[nodiscard]] inline std::string adapterPciId(const PciLocation& location, uint32_t vendorId, uint32_t deviceId)
{
    if (location.function.has_value())
    {
        return std::format("PCI_{:02X}:{:02X}.{:X}_{:04X}:{:04X}",
                           location.bus,
                           location.device,
                           *location.function,
                           vendorId & 0xFFFFU,
                           deviceId & 0xFFFFU);
    }
    return std::format("PCI_{:02X}:{:02X}_{:04X}:{:04X}", location.bus, location.device, vendorId & 0xFFFFU, deviceId & 0xFFFFU);
}

/// The id of an adapter named by its LUID alone: "LUID_0x{HighPart}_0x{LowPart}". Unique among the
/// adapters present, but a driver reset gives the adapter a new LUID, so this is the fallback.
[[nodiscard]] inline std::string adapterLuidId(uint32_t luidHighPart, uint32_t luidLowPart)
{
    return std::format("LUID_0x{:08X}_0x{:08X}", luidHighPart, luidLowPart);
}

/// The id DXGIGPUProbe gives an adapter, which keys its history, its GPU tab state and its counters
/// (#1317). It used to be the adapter's position in DXGI's list ("GPU{index}"), so removing an
/// adapter renumbered every one listed after it -- moving their history to the wrong card -- and a
/// later adapter took a removed one's number. Now it is the adapter's PCI location and model (see
/// adapterPciId()) where it reports one, and its LUID otherwise (see adapterLuidId()): an adapter
/// with no bus address, or a second adapter at a location whose id is already in @p takenIds, so two
/// adapters present at once never share one. Over time the id follows the slot and model, so an
/// identical card later fitted in the same slot continues the old one's history. Enumeration order is
/// kept only as display order.
/// @param location The adapter's PCI location, or nullopt when it reports none
/// @param vendorId DXGI_ADAPTER_DESC1::VendorId
/// @param deviceId DXGI_ADAPTER_DESC1::DeviceId
/// @param luidHighPart DXGI_ADAPTER_DESC1::AdapterLuid.HighPart, as unsigned
/// @param luidLowPart DXGI_ADAPTER_DESC1::AdapterLuid.LowPart
/// @param takenIds The ids other adapters already have
[[nodiscard]] inline std::string stableAdapterId(const std::optional<PciLocation>& location,
                                                 uint32_t vendorId,
                                                 uint32_t deviceId,
                                                 uint32_t luidHighPart,
                                                 uint32_t luidLowPart,
                                                 const std::unordered_set<std::string>& takenIds)
{
    if (location.has_value())
    {
        std::string pciId = adapterPciId(*location, vendorId, deviceId);
        if (!takenIds.contains(pciId))
        {
            return pciId;
        }
    }
    return adapterLuidId(luidHighPart, luidLowPart);
}

/// The descriptor heuristic classifyIntegrated() falls back on when DXCore can't say, taking the
/// relevant DXGI_ADAPTER_DESC1 fields directly so the vendor/VRAM-threshold branches can be unit
/// tested without a real (or COM-mocked) IDXGIAdapter1. A guess: an AMD APU with a carve-out of
/// 1 GiB or more reads as discrete, and a small dGPU as integrated (#1263).
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

    // Qualcomm's Adreno is only ever the SoC's own GPU (#1263).
    if (vendorId == 0x5143)
    {
        return true;
    }

    // NVIDIA doesn't make consumer integrated GPUs (Tegra is different architecture).
    // Assume discrete for NVIDIA.
    return false;
}

/// Whether an adapter is integrated (shares system memory) rather than discrete. DXCore's
/// DXCoreAdapterProperty::IsIntegrated is the driver's own answer, so it wins whenever it could be
/// read; only without it (Windows 10 before DXCore, or a failed query) does the descriptor
/// heuristic guess from vendor and dedicated memory, which got AMD APUs with a 1 GiB+ carve-out
/// and small dGPUs wrong and didn't know Qualcomm (#1263). A software adapter is never integrated.
/// @param dxcoreIsIntegrated DXCore's IsIntegrated for this adapter, or nullopt when unavailable
/// @param vendorId DXGI_ADAPTER_DESC1::VendorId
/// @param flags DXGI_ADAPTER_DESC1::Flags (DXGI_ADAPTER_FLAG_SOFTWARE = 0x2)
/// @param dedicatedVideoMemory DXGI_ADAPTER_DESC1::DedicatedVideoMemory
[[nodiscard]] inline bool
classifyIntegrated(std::optional<bool> dxcoreIsIntegrated, uint32_t vendorId, uint32_t flags, uint64_t dedicatedVideoMemory)
{
    constexpr uint32_t SOFTWARE_FLAG = 2;
    if ((flags & SOFTWARE_FLAG) != 0)
    {
        return false;
    }
    if (dxcoreIsIntegrated.has_value())
    {
        return *dxcoreIsIntegrated;
    }
    return isIntegratedGPUFromDesc(vendorId, flags, dedicatedVideoMemory);
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

/// Whether an adapter's memory figures (size, and the PDH used bytes WindowsGPUProbe assigns) count
/// its shared segment: an integrated GPU's memory is shared system memory, a discrete GPU's is its
/// dedicated VRAM. Published as GPUInfo::memoryIsShared so per-process memory counts the same
/// segment, a 0 shared reading included (#1164).
[[nodiscard]] constexpr bool adapterMemoryIsShared(bool isIntegrated)
{
    return isIntegrated;
}

/// An adapter's memory size, from DXGI_ADAPTER_DESC1: the shared system memory an integrated GPU
/// may use, or a discrete GPU's dedicated VRAM. Fixed for the adapter's lifetime, unlike the
/// per-process budget QueryVideoMemoryInfo reports, which moved over time and made the GPU tab's
/// total disagree with its header (16 GB vs 17.2 GB on an Arc 140T, #1029).
[[nodiscard]] constexpr uint64_t adapterMemoryTotalBytes(bool isIntegrated, uint64_t dedicatedVideoMemory, uint64_t sharedSystemMemory)
{
    return adapterMemoryIsShared(isIntegrated) ? sharedSystemMemory : dedicatedVideoMemory;
}

/// The counters DXGIGPUProbe reads for one adapter: its id and memory size. DXGI reads neither
/// utilization nor the adapter's memory in use, so both start unread; NVML or PDH marks them
/// available when it supplies a reading. Defaulting to available published DXGI's placeholder 0%
/// and 0 B as real samples whenever PDH was warming up or unavailable (#1245).
[[nodiscard]] inline GPUCounters makeDXGIAdapterCounters(std::string gpuId, uint64_t memoryTotalBytes)
{
    GPUCounters counter{};
    counter.gpuId = std::move(gpuId);
    counter.memoryTotalBytes = memoryTotalBytes;
    counter.memoryUsedBytes = 0;
    counter.utilizationAvailable = false;
    counter.memoryAvailable = false;
    return counter;
}

/// An adapter LUID as one 64-bit key: the high part above the low part.
[[nodiscard]] constexpr uint64_t adapterLuidKey(uint32_t luidHighPart, uint32_t luidLowPart) noexcept
{
    return (static_cast<uint64_t>(luidHighPart) << 32U) | luidLowPart;
}

/// One adapter from a DXCore adapter list, as DXGIGPUProbe reads it to find the NPUs and other
/// compute accelerators DXGI never lists (#1799). Plain values, so the selection below can be tested
/// without DXCore.
struct DXCoreAdapterRecord
{
    uint32_t luidHighPart = 0;
    uint32_t luidLowPart = 0;
    bool npu = false;                ///< Has DXCORE_HARDWARE_TYPE_ATTRIBUTE_NPU
    bool computeAccelerator = false; ///< Has DXCORE_HARDWARE_TYPE_ATTRIBUTE_COMPUTE_ACCELERATOR
    bool isHardware = true;          ///< DXCoreAdapterProperty::IsHardware
    bool isIntegrated = false;       ///< DXCoreAdapterProperty::IsIntegrated (false when unread)
    std::string description;         ///< DXCoreAdapterProperty::DriverDescription
    uint32_t vendorId = 0;           ///< DXCoreAdapterProperty::HardwareID
    uint32_t deviceId = 0;
    uint64_t dedicatedAdapterMemory = 0;
    uint64_t sharedSystemMemory = 0;
};

/// The kind of compute accelerator @p record is: an NPU where DXCore says so, else a compute
/// accelerator, else nullopt -- a GPU or anything else is not one (#1799).
[[nodiscard]] constexpr std::optional<DeviceKind> acceleratorKind(const DXCoreAdapterRecord& record) noexcept
{
    if (record.npu)
    {
        return DeviceKind::Npu;
    }
    if (record.computeAccelerator)
    {
        return DeviceKind::ComputeAccelerator;
    }
    return std::nullopt;
}

/// The compute accelerators DXGIGPUProbe lists beside DXGI's GPUs (#1799): every hardware adapter
/// DXCore reports as an NPU or compute accelerator (MCDM devices, which DXGI never enumerates) whose
/// LUID DXGI doesn't already list. An adapter DXGI lists is handled as DXGI's, whatever DXCore calls
/// it, so nothing is listed twice; a software adapter isn't listed at all. An adapter found in more
/// than one DXCore list (one per hardware type) is listed once, with the attributes of all of them,
/// in the order first found.
/// @param records Every adapter of DXCore's NPU and compute-accelerator lists
/// @param dxgiLuids adapterLuidKey() of every adapter DXGI enumerates
[[nodiscard]] inline std::vector<DXCoreAdapterRecord> selectAcceleratorAdapters(const std::vector<DXCoreAdapterRecord>& records,
                                                                                const std::unordered_set<uint64_t>& dxgiLuids)
{
    std::vector<DXCoreAdapterRecord> selected;
    std::size_t index = 0;
    while (index < records.size())
    {
        const DXCoreAdapterRecord& record = records[index];
        ++index;
        const uint64_t key = adapterLuidKey(record.luidHighPart, record.luidLowPart);
        if (!record.isHardware || !acceleratorKind(record).has_value() || dxgiLuids.contains(key))
        {
            continue;
        }
        std::size_t existing = 0;
        while (existing < selected.size() && adapterLuidKey(selected[existing].luidHighPart, selected[existing].luidLowPart) != key)
        {
            ++existing;
        }
        if (existing < selected.size())
        {
            selected[existing].npu = selected[existing].npu || record.npu;
            selected[existing].computeAccelerator = selected[existing].computeAccelerator || record.computeAccelerator;
            continue;
        }
        selected.push_back(record);
    }
    return selected;
}

/// Whether a compute accelerator's memory figures count its shared segment: one with no dedicated
/// memory of its own (an NPU such as Intel AI Boost, which maps system memory) counts shared, one
/// with dedicated memory counts that, as a discrete GPU does (#1164, #1799).
[[nodiscard]] constexpr bool acceleratorMemoryIsShared(uint64_t dedicatedAdapterMemory) noexcept
{
    return dedicatedAdapterMemory == 0;
}

/// A compute accelerator's memory size: its dedicated memory, or without any the shared system
/// memory it may map (see acceleratorMemoryIsShared()).
[[nodiscard]] constexpr uint64_t acceleratorMemoryTotalBytes(uint64_t dedicatedAdapterMemory, uint64_t sharedSystemMemory) noexcept
{
    return acceleratorMemoryIsShared(dedicatedAdapterMemory) ? sharedSystemMemory : dedicatedAdapterMemory;
}

} // namespace Platform
