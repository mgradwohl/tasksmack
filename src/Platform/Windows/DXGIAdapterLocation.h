#pragma once

#include "Platform/GPUTypes.h"
#include "Platform/Windows/DXGIGPUProbeMath.h"

#include <optional>

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winternl.h> // NTSTATUS, for d3dkmthk.h
#include <d3dkmthk.h>
// clang-format on

namespace Platform
{

/// The kernel-mode adapter calls adapterPciLocation() makes. Defaults to gdi32's exports; tests
/// substitute fakes, so the query can be checked without the hardware it describes (#1091).
struct D3DKMTAdapterFunctions
{
    decltype(&D3DKMTOpenAdapterFromLuid) openAdapterFromLuid = &D3DKMTOpenAdapterFromLuid;
    decltype(&D3DKMTQueryAdapterInfo) queryAdapterInfo = &D3DKMTQueryAdapterInfo;
    decltype(&D3DKMTCloseAdapter) closeAdapter = &D3DKMTCloseAdapter;
};

/// The adapter's PCI bus location, read from the kernel graphics adapter its LUID names, so the
/// Windows probe can match it to its NVML device exactly (#1091): DXGI_ADAPTER_DESC1 has no PCI
/// location, and DXGI and NVML enumerate in different orders. nullopt when the adapter cannot be
/// opened or reports no address (e.g. a software or remote adapter). An opened adapter is always
/// closed.
[[nodiscard]] inline std::optional<PciLocation> adapterPciLocation(const LUID& luid, const D3DKMTAdapterFunctions& fns = {})
{
    D3DKMT_OPENADAPTERFROMLUID open{};
    open.AdapterLuid = luid;
    if (fns.openAdapterFromLuid(&open) != 0) // STATUS_SUCCESS
    {
        return std::nullopt;
    }
    D3DKMT_ADAPTERADDRESS address{};
    D3DKMT_QUERYADAPTERINFO query{};
    query.hAdapter = open.hAdapter;
    query.Type = KMTQAITYPE_ADAPTERADDRESS;
    query.pPrivateDriverData = &address;
    query.PrivateDriverDataSize = sizeof(address);
    const NTSTATUS status = fns.queryAdapterInfo(&query);
    D3DKMT_CLOSEADAPTER close{};
    close.hAdapter = open.hAdapter;
    fns.closeAdapter(&close);
    if (status != 0)
    {
        return std::nullopt;
    }
    return PciLocation{.bus = address.BusNumber, .device = address.DeviceNumber};
}

// D3DKMT_ADAPTERTYPE::IndirectDisplayDevice exists only from the WDDM 1.3 interface on (#1251).
static_assert(DXGKDDI_INTERFACE_VERSION >= DXGKDDI_INTERFACE_VERSION_WDDM1_3,
              "adapterKind() needs D3DKMT_ADAPTERTYPE::IndirectDisplayDevice (WDDM 1.3)");

/// The kind of kernel graphics adapter the LUID names: render, display, software, indirect display
/// and so on. DXGI lists an indirect-display adapter (a DisplayLink dock, Miracast) under the name,
/// vendor and device id of the GPU it renders on, so this is what tells it apart from that GPU
/// (#1251). nullopt when the adapter cannot be opened or the query fails. An opened adapter is
/// always closed.
[[nodiscard]] inline std::optional<D3DKMT_ADAPTERTYPE> adapterKind(const LUID& luid, const D3DKMTAdapterFunctions& fns = {})
{
    D3DKMT_OPENADAPTERFROMLUID open{};
    open.AdapterLuid = luid;
    if (fns.openAdapterFromLuid(&open) != 0) // STATUS_SUCCESS
    {
        return std::nullopt;
    }
    D3DKMT_ADAPTERTYPE kind{};
    D3DKMT_QUERYADAPTERINFO query{};
    query.hAdapter = open.hAdapter;
    query.Type = KMTQAITYPE_ADAPTERTYPE;
    query.pPrivateDriverData = &kind;
    query.PrivateDriverDataSize = sizeof(kind);
    const NTSTATUS status = fns.queryAdapterInfo(&query);
    D3DKMT_CLOSEADAPTER close{};
    close.hAdapter = open.hAdapter;
    fns.closeAdapter(&close);
    if (status != 0)
    {
        return std::nullopt;
    }
    return kind;
}

/// The adapter-type bits shouldListAdapter() decides on, from a D3DKMT_ADAPTERTYPE (#1251).
[[nodiscard]] inline AdapterTypeBits adapterTypeBits(const D3DKMT_ADAPTERTYPE& kind) noexcept
{
    // NOLINTBEGIN(cppcoreguidelines-pro-type-union-access) - D3DKMT_ADAPTERTYPE is a union of its bitfields and Value
    return AdapterTypeBits{.softwareDevice = kind.SoftwareDevice != 0U, .indirectDisplayDevice = kind.IndirectDisplayDevice != 0U};
    // NOLINTEND(cppcoreguidelines-pro-type-union-access)
}

} // namespace Platform
