#pragma once

// GPU temperature, clock and fan for every adapter on Windows, NVIDIA or not, from the kernel graphics
// adapter's performance data (#2586): D3DKMTQueryAdapterInfo with KMTQAITYPE_ADAPTERPERFDATA (the
// figures Task Manager's GPU page draws), KMTQAITYPE_ADAPTERPERFDATA_CAPS for the fan's maximum and
// KMTQAITYPE_NODEPERFDATA for the 3D engine's clock. The calls are gdi32's exports (linked, like the
// adapter-location query in DXGIAdapterLocation.h), or a test's fakes through D3DKMTAdapterFunctions.
// WindowsGPUProbe reads this on the GPU sampler thread for the adapters NVML doesn't cover.

#include "Platform/Windows/AdapterPerfDataMath.h"
#include "Platform/Windows/DXGIAdapterLocation.h"

#include <cstdint>
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

// The perf-data queries exist from the WDDM 2.4 interface on (Windows 10 1803).
static_assert(DXGKDDI_INTERFACE_VERSION >= DXGKDDI_INTERFACE_VERSION_WDDM2_4,
              "readAdapterPerfData() needs KMTQAITYPE_ADAPTERPERFDATA (WDDM 2.4)");

namespace Platform
{

/// The adapter's performance data, read from the kernel graphics adapter its LUID names. nullopt when
/// the adapter can't be opened or its driver doesn't answer the perf-data query (a software adapter,
/// a driver older than WDDM 2.4, a remote session): "no sensor", which the caller never logs per
/// sample. The fan maximum (the caps query) and the 3D engine's clock (node 0's perf data) are read
/// alongside; either failing leaves its fields 0, so that one sensor is missing rather than all of
/// them. An opened adapter is always closed. The query goes to the driver, so a sleeping adapter may
/// be woken by it: the caller asks DisplayDevicePower first and leaves a sleeping GPU alone (#1265).
[[nodiscard]] inline std::optional<AdapterPerfReading> readAdapterPerfData(const LUID& luid, const D3DKMTAdapterFunctions& fns = {})
{
    D3DKMT_OPENADAPTERFROMLUID open{};
    open.AdapterLuid = luid;
    if (fns.openAdapterFromLuid(&open) != 0) // STATUS_SUCCESS
    {
        return std::nullopt;
    }

    const auto query = [&fns, &open](KMTQUERYADAPTERINFOTYPE type, void* data, UINT size)
    {
        D3DKMT_QUERYADAPTERINFO request{};
        request.hAdapter = open.hAdapter;
        request.Type = type;
        request.pPrivateDriverData = data;
        request.PrivateDriverDataSize = size;
        return fns.queryAdapterInfo(&request) == 0;
    };

    std::optional<AdapterPerfReading> reading;
    // The adapter's first physical adapter: TaskSmack lists linked adapters as one, as DXGI does.
    D3DKMT_ADAPTER_PERFDATA perf{};
    perf.PhysicalAdapterIndex = 0;
    if (query(KMTQAITYPE_ADAPTERPERFDATA, &perf, sizeof(perf)))
    {
        reading = AdapterPerfReading{
            .temperatureDeciCelsius = perf.Temperature,
            .fanRpm = perf.FanRPM,
            .powerTenthsPercent = perf.Power,
            .memoryFrequencyHz = perf.MemoryFrequency,
            .maxMemoryFrequencyHz = perf.MaxMemoryFrequency,
        };
        D3DKMT_ADAPTER_PERFDATACAPS caps{};
        caps.PhysicalAdapterIndex = 0;
        if (query(KMTQAITYPE_ADAPTERPERFDATA_CAPS, &caps, sizeof(caps)))
        {
            reading->maxFanRpm = caps.MaxFanRPM;
        }
        // Node 0 is the 3D engine on every WDDM driver TaskSmack has met (PDH's "eng_0_engtype_3D").
        D3DKMT_NODE_PERFDATA node{};
        node.NodeOrdinal = 0;
        node.PhysicalAdapterIndex = 0;
        if (query(KMTQAITYPE_NODEPERFDATA, &node, sizeof(node)))
        {
            reading->engineFrequencyHz = node.Frequency;
            reading->maxEngineFrequencyHz = node.MaxFrequency;
        }
    }

    D3DKMT_CLOSEADAPTER close{};
    close.hAdapter = open.hAdapter;
    fns.closeAdapter(&close);
    return reading;
}

} // namespace Platform
