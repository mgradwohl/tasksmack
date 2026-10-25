/// @file WindowsDXGIFake.h
/// @brief A fake DXGI factory and fake D3DKMT adapter calls for the Windows DXGIGPUProbe, and the
/// friend accessor that installs them (#1294).
///
/// A fake factory lists the adapters present when it was made, as a real one does, and stops being
/// current (IDXGIFactory1::IsCurrent()) once the adapter set changes, so a test can add or remove an
/// adapter and watch the probe pick it up. The fake D3DKMT calls answer each adapter's PCI location
/// by LUID. Everything is inline, since more than one test file uses it.

#pragma once

#ifdef _WIN32

#include "Platform/GPUTypes.h"
#include "Platform/Windows/ComPtr.h"
#include "Platform/Windows/DXGIAdapterLocation.h"
#include "Platform/Windows/DXGIGPUProbe.h"
#include "Platform/Windows/DXGIGPUProbeMath.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wlanguage-extension-token"
#include <dxgi.h>
#include <dxcore_interface.h>
#pragma clang diagnostic pop
// clang-format on

namespace Platform
{
namespace DXGIFake
{

/// What the fake D3DKMT answers an adapter's perf-data queries with (#2586): the adapter perf data
/// itself, the caps (the fan maximum) unless capsOk is false, and node 0's perf data (the engine
/// clock) unless nodeOk is false. An adapter without one fails the perf-data query, as a driver
/// older than WDDM 2.4 does.
struct FakeAdapterPerf
{
    std::uint32_t temperatureDeciCelsius = 0;
    std::uint32_t fanRpm = 0;
    std::uint32_t powerTenthsPercent = 0;
    std::uint64_t memoryFrequencyHz = 0;
    std::uint64_t maxMemoryFrequencyHz = 0;
    std::uint32_t maxFanRpm = 0;
    bool capsOk = true;
    std::uint64_t engineFrequencyHz = 0;
    std::uint64_t maxEngineFrequencyHz = 0;
    bool nodeOk = true;
};

/// One fake adapter: its descriptor, the PCI location the fake D3DKMT reports for its LUID, and the
/// perf data it answers with (none: the query fails).
struct FakeAdapter
{
    DXGI_ADAPTER_DESC1 desc{};
    std::optional<PciLocation> pciLocation;
    std::optional<FakeAdapterPerf> perf;
};

/// One fake DXCore adapter of the NPU or compute-accelerator lists (#1799), and the PCI location the
/// fake D3DKMT reports for its LUID.
struct FakeAccelerator
{
    DXCoreAdapterRecord record;
    std::optional<PciLocation> pciLocation;
};

struct FakeDXGIState
{
    std::vector<FakeAdapter> adapters;
    // What the fake DXCore lists: installed only by DXGIGPUProbeTestAccessor::useFakeAccelerators().
    std::vector<FakeAccelerator> accelerators;
    int acceleratorListings = 0;
    // Bumped whenever the adapter set changes: a factory made before is no longer current.
    int generation = 0;
    int factoriesCreated = 0;
    bool failFactoryCreation = false;
    // EnumAdapters1 succeeds at this index but hands back no adapter (a misbehaving driver stack).
    std::optional<UINT> nullAdapterAt;
    // Kernel adapters opened, so a test can see a per-LUID decision being made again.
    int adapterOpens = 0;
    // Perf-data queries (KMTQAITYPE_ADAPTERPERFDATA) answered or refused, so a test can see that a
    // sleeping adapter wasn't asked (#2586).
    int perfQueries = 0;
    // Open kernel adapter handles: handle - 1 indexes this, giving the LUID it was opened for.
    std::vector<LUID> openedLuids;
};

inline FakeDXGIState& dxgiState()
{
    static FakeDXGIState state;
    return state;
}

/// An adapter descriptor with this name, vendor, device id, LUID and dedicated memory.
inline FakeAdapter makeAdapter(const wchar_t* name,
                               std::uint32_t vendorId,
                               std::uint32_t deviceId,
                               std::uint32_t luidLowPart,
                               std::optional<PciLocation> pciLocation,
                               std::uint64_t dedicatedVideoMemory = 8ULL << 30U)
{
    FakeAdapter adapter;
    const std::wstring_view source(name);
    const std::size_t length = std::min(source.size(), std::size(adapter.desc.Description) - 1);
    std::ranges::copy(source.substr(0, length), std::begin(adapter.desc.Description));
    adapter.desc.VendorId = vendorId;
    adapter.desc.DeviceId = deviceId;
    adapter.desc.AdapterLuid.LowPart = luidLowPart;
    adapter.desc.AdapterLuid.HighPart = 0;
    adapter.desc.DedicatedVideoMemory = static_cast<SIZE_T>(dedicatedVideoMemory);
    adapter.desc.SharedSystemMemory = static_cast<SIZE_T>(16ULL << 30U);
    adapter.pciLocation = pciLocation;
    return adapter;
}

/// Replace the adapter set, as a hot-plug, removal or driver reset would: factories made before are
/// no longer current.
inline void setAdapters(std::vector<FakeAdapter> adapters)
{
    dxgiState().adapters = std::move(adapters);
    ++dxgiState().generation;
}

// NOLINTBEGIN(cppcoreguidelines-owning-memory,readability-identifier-naming,readability-convert-member-functions-to-static,cppcoreguidelines-special-member-functions,hicpp-special-member-functions,readability-non-const-parameter)
// - COM objects: self-deleting on the last Release(), with the interface's own method names.

class FakeDXGIAdapter final : public IDXGIAdapter1
{
  public:
    explicit FakeDXGIAdapter(const DXGI_ADAPTER_DESC1& desc) : m_Desc(desc)
    {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID /*riid*/, void** object) override
    {
        *object = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return ++m_RefCount;
    }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG count = --m_RefCount;
        if (count == 0)
        {
            delete this;
        }
        return count;
    }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID /*name*/, UINT /*size*/, const void* /*data*/) override
    {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID /*name*/, const IUnknown* /*unknown*/) override
    {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID /*name*/, UINT* /*size*/, void* /*data*/) override
    {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID /*riid*/, void** parent) override
    {
        *parent = nullptr;
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE EnumOutputs(UINT /*output*/, IDXGIOutput** output) override
    {
        *output = nullptr;
        return DXGI_ERROR_NOT_FOUND;
    }
    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_ADAPTER_DESC* /*desc*/) override
    {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE CheckInterfaceSupport(REFGUID /*name*/, LARGE_INTEGER* /*umdVersion*/) override
    {
        return DXGI_ERROR_UNSUPPORTED;
    }
    HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_ADAPTER_DESC1* desc) override
    {
        *desc = m_Desc;
        return S_OK;
    }

  private:
    ~FakeDXGIAdapter() = default;

    DXGI_ADAPTER_DESC1 m_Desc;
    ULONG m_RefCount = 1;
};

class FakeDXGIFactory final : public IDXGIFactory1
{
  public:
    FakeDXGIFactory() : m_Generation(dxgiState().generation)
    {
        for (const auto& adapter : dxgiState().adapters)
        {
            m_Adapters.push_back(adapter.desc);
        }
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID /*riid*/, void** object) override
    {
        *object = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return ++m_RefCount;
    }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG count = --m_RefCount;
        if (count == 0)
        {
            delete this;
        }
        return count;
    }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID /*name*/, UINT /*size*/, const void* /*data*/) override
    {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID /*name*/, const IUnknown* /*unknown*/) override
    {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID /*name*/, UINT* /*size*/, void* /*data*/) override
    {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID /*riid*/, void** parent) override
    {
        *parent = nullptr;
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE EnumAdapters(UINT /*index*/, IDXGIAdapter** adapter) override
    {
        *adapter = nullptr;
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE MakeWindowAssociation(HWND /*window*/, UINT /*flags*/) override
    {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE GetWindowAssociation(HWND* window) override
    {
        *window = nullptr;
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE CreateSwapChain(IUnknown* /*device*/, DXGI_SWAP_CHAIN_DESC* /*desc*/, IDXGISwapChain** swapChain) override
    {
        *swapChain = nullptr;
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE CreateSoftwareAdapter(HMODULE /*module*/, IDXGIAdapter** adapter) override
    {
        *adapter = nullptr;
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE EnumAdapters1(UINT index, IDXGIAdapter1** adapter) override
    {
        if (index >= m_Adapters.size())
        {
            *adapter = nullptr;
            return DXGI_ERROR_NOT_FOUND;
        }
        if (dxgiState().nullAdapterAt == index)
        {
            *adapter = nullptr;
            return S_OK;
        }
        *adapter = new FakeDXGIAdapter(m_Adapters[index]);
        return S_OK;
    }
    BOOL STDMETHODCALLTYPE IsCurrent() override
    {
        return m_Generation == dxgiState().generation ? TRUE : FALSE;
    }

  private:
    ~FakeDXGIFactory() = default;

    // The adapters present when this factory was made, as a real factory lists them.
    std::vector<DXGI_ADAPTER_DESC1> m_Adapters;
    int m_Generation;
    ULONG m_RefCount = 1;
};

// NOLINTEND(cppcoreguidelines-owning-memory,readability-identifier-naming,readability-convert-member-functions-to-static,cppcoreguidelines-special-member-functions,hicpp-special-member-functions,readability-non-const-parameter)

/// A new fake factory over the current adapter set, or null when creation is set to fail.
inline ComPtr<IDXGIFactory1> createFakeFactory()
{
    ComPtr<IDXGIFactory1> factory;
    if (dxgiState().failFactoryCreation)
    {
        return factory;
    }
    ++dxgiState().factoriesCreated;
    *factory.releaseAndGetAddressOf() = new FakeDXGIFactory(); // NOLINT(cppcoreguidelines-owning-memory) - COM, released by ComPtr
    return factory;
}

/// A fake compute accelerator (#1799): what DXCore reports for it, at @p pciLocation.
inline FakeAccelerator makeAccelerator(const char* name,
                                       std::uint32_t vendorId,
                                       std::uint32_t deviceId,
                                       std::uint32_t luidLowPart,
                                       std::optional<PciLocation> pciLocation,
                                       bool npu = true)
{
    FakeAccelerator accelerator;
    accelerator.record.luidLowPart = luidLowPart;
    accelerator.record.npu = npu;
    accelerator.record.computeAccelerator = !npu;
    accelerator.record.description = name;
    accelerator.record.vendorId = vendorId;
    accelerator.record.deviceId = deviceId;
    accelerator.record.sharedSystemMemory = 16ULL << 30U;
    accelerator.pciLocation = pciLocation;
    return accelerator;
}

/// What the fake DXCore lists: every fake accelerator's record.
inline std::vector<DXCoreAdapterRecord> listFakeAccelerators()
{
    ++dxgiState().acceleratorListings;
    std::vector<DXCoreAdapterRecord> records;
    for (const auto& accelerator : dxgiState().accelerators)
    {
        records.push_back(accelerator.record);
    }
    return records;
}

/// The PCI location the fake D3DKMT knows for an adapter or accelerator with this LUID, or null
/// when it knows no adapter with it (the location itself may be unset).
inline const std::optional<PciLocation>* locationWithLuid(const LUID& luid)
{
    for (const auto& adapter : dxgiState().adapters)
    {
        if (adapter.desc.AdapterLuid.LowPart == luid.LowPart && adapter.desc.AdapterLuid.HighPart == luid.HighPart)
        {
            return &adapter.pciLocation;
        }
    }
    for (const auto& accelerator : dxgiState().accelerators)
    {
        if (accelerator.record.luidLowPart == luid.LowPart && accelerator.record.luidHighPart == static_cast<std::uint32_t>(luid.HighPart))
        {
            return &accelerator.pciLocation;
        }
    }
    return nullptr;
}

/// The fake DXGI adapter with this LUID, or null for an accelerator or an unknown LUID.
inline FakeAdapter* adapterWithLuid(const LUID& luid)
{
    for (auto& adapter : dxgiState().adapters)
    {
        if (adapter.desc.AdapterLuid.LowPart == luid.LowPart && adapter.desc.AdapterLuid.HighPart == luid.HighPart)
        {
            return &adapter;
        }
    }
    return nullptr;
}

constexpr NTSTATUS FAKE_STATUS_INVALID_PARAMETER = static_cast<NTSTATUS>(0xC000000DL);

inline NTSTATUS APIENTRY fakeOpenAdapterFromLuid(const D3DKMT_OPENADAPTERFROMLUID* open)
{
    if (locationWithLuid(open->AdapterLuid) == nullptr)
    {
        return FAKE_STATUS_INVALID_PARAMETER;
    }
    ++dxgiState().adapterOpens;
    dxgiState().openedLuids.push_back(open->AdapterLuid);
    // The real call fills hAdapter through its nominally const argument.
    const_cast<D3DKMT_OPENADAPTERFROMLUID*>(open)->hAdapter = // NOLINT(cppcoreguidelines-pro-type-const-cast)
        static_cast<D3DKMT_HANDLE>(dxgiState().openedLuids.size());
    return 0;
}

inline NTSTATUS APIENTRY fakeQueryAdapterInfo(const D3DKMT_QUERYADAPTERINFO* query)
{
    const auto& opened = dxgiState().openedLuids;
    if (query->hAdapter == 0 || query->hAdapter > opened.size())
    {
        return FAKE_STATUS_INVALID_PARAMETER;
    }
    const std::optional<PciLocation>* location = locationWithLuid(opened[query->hAdapter - 1]);
    if (location == nullptr)
    {
        return FAKE_STATUS_INVALID_PARAMETER;
    }
    if (query->Type == KMTQAITYPE_ADAPTERADDRESS && query->PrivateDriverDataSize == sizeof(D3DKMT_ADAPTERADDRESS))
    {
        if (!location->has_value())
        {
            return FAKE_STATUS_INVALID_PARAMETER;
        }
        auto* address = static_cast<D3DKMT_ADAPTERADDRESS*>(query->pPrivateDriverData);
        address->BusNumber = (*location)->bus;
        address->DeviceNumber = (*location)->device;
        address->FunctionNumber = (*location)->function.value_or(0);
        return 0;
    }
    if (query->Type == KMTQAITYPE_ADAPTERTYPE && query->PrivateDriverDataSize == sizeof(D3DKMT_ADAPTERTYPE))
    {
        // A plain hardware render adapter: neither a software nor an indirect-display device.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-union-access) - D3DKMT_ADAPTERTYPE is a union of its bitfields and Value
        static_cast<D3DKMT_ADAPTERTYPE*>(query->pPrivateDriverData)->Value = 0;
        return 0;
    }
    // The perf-data queries (#2586): answered from the adapter's FakeAdapterPerf, refused without one
    // (an accelerator, or a driver that reports none), each for its own struct's size only.
    const FakeAdapter* adapter = adapterWithLuid(opened[query->hAdapter - 1]);
    const std::optional<FakeAdapterPerf>& perf = adapter != nullptr ? adapter->perf : std::optional<FakeAdapterPerf>{};
    if (query->Type == KMTQAITYPE_ADAPTERPERFDATA)
    {
        ++dxgiState().perfQueries;
        if (!perf.has_value() || query->PrivateDriverDataSize != sizeof(D3DKMT_ADAPTER_PERFDATA))
        {
            return FAKE_STATUS_INVALID_PARAMETER;
        }
        auto* data = static_cast<D3DKMT_ADAPTER_PERFDATA*>(query->pPrivateDriverData);
        data->Temperature = perf->temperatureDeciCelsius;
        data->FanRPM = perf->fanRpm;
        data->Power = perf->powerTenthsPercent;
        data->MemoryFrequency = perf->memoryFrequencyHz;
        data->MaxMemoryFrequency = perf->maxMemoryFrequencyHz;
        return 0;
    }
    if (query->Type == KMTQAITYPE_ADAPTERPERFDATA_CAPS)
    {
        if (!perf.has_value() || !perf->capsOk || query->PrivateDriverDataSize != sizeof(D3DKMT_ADAPTER_PERFDATACAPS))
        {
            return FAKE_STATUS_INVALID_PARAMETER;
        }
        static_cast<D3DKMT_ADAPTER_PERFDATACAPS*>(query->pPrivateDriverData)->MaxFanRPM = perf->maxFanRpm;
        return 0;
    }
    if (query->Type == KMTQAITYPE_NODEPERFDATA)
    {
        if (!perf.has_value() || !perf->nodeOk || query->PrivateDriverDataSize != sizeof(D3DKMT_NODE_PERFDATA))
        {
            return FAKE_STATUS_INVALID_PARAMETER;
        }
        auto* node = static_cast<D3DKMT_NODE_PERFDATA*>(query->pPrivateDriverData);
        if (node->NodeOrdinal != 0)
        {
            return FAKE_STATUS_INVALID_PARAMETER; // The fake has one engine node, the 3D engine
        }
        node->Frequency = perf->engineFrequencyHz;
        node->MaxFrequency = perf->maxEngineFrequencyHz;
        return 0;
    }
    return FAKE_STATUS_INVALID_PARAMETER;
}

inline NTSTATUS APIENTRY fakeCloseAdapter(const D3DKMT_CLOSEADAPTER* /*close*/)
{
    return 0;
}

/// The fake D3DKMT calls as a D3DKMTAdapterFunctions, for a probe's seam.
inline D3DKMTAdapterFunctions fakeD3DKMTFunctions()
{
    return D3DKMTAdapterFunctions{
        .openAdapterFromLuid = fakeOpenAdapterFromLuid,
        .queryAdapterInfo = fakeQueryAdapterInfo,
        .closeAdapter = fakeCloseAdapter,
    };
}

} // namespace DXGIFake

/// Test-only accessor (a friend of DXGIGPUProbe): swaps the probe's DXGI factory and D3DKMT calls
/// for the fakes above. DXCore is switched off, since it would answer for this machine's adapters;
/// adapters are then classified by their descriptor.
struct DXGIGPUProbeTestAccessor
{
    static void useFakes(DXGIGPUProbe& probe)
    {
        probe.unloadDXCore();
        probe.m_CreateFactory = []
        {
            return DXGIFake::createFakeFactory();
        };
        *probe.m_D3DKMT = D3DKMTAdapterFunctions{
            .openAdapterFromLuid = DXGIFake::fakeOpenAdapterFromLuid,
            .queryAdapterInfo = DXGIFake::fakeQueryAdapterInfo,
            .closeAdapter = DXGIFake::fakeCloseAdapter,
        };
        probe.m_ListedByLuid.clear();
        probe.m_IntegratedByLuid.clear();
        probe.m_IdByLuid.clear();
        probe.m_Initialized = probe.initialize();
    }

    /// Lists the fake accelerators (DXGIFake::FakeDXGIState::accelerators) as DXCore's NPUs and
    /// compute accelerators, in place of this machine's (#1799). Call after useFakes().
    static void useFakeAccelerators(DXGIGPUProbe& probe)
    {
        probe.m_ListAccelerators = DXGIFake::listFakeAccelerators;
        probe.m_AcceleratorLuids.reset();
    }

    /// Makes the DXCore factory again, as a full rescan does: false once useFakes() unloaded dxcore.dll.
    static bool createDXCoreFactory(DXGIGPUProbe& probe)
    {
        return probe.createDXCoreFactory();
    }

    /// Puts @p factory where dxcore.dll's adapter factory goes (a fake from tests/Mocks/WindowsDXCoreFake.h,
    /// #2039): DXCore then classifies the adapters it knows and lists the accelerators. Call after
    /// useFakes(); a rescan keeps it, since useFakes() left no dxcore.dll to make another from.
    static void useDXCoreFactory(DXGIGPUProbe& probe, ComPtr<IDXCoreAdapterFactory> factory)
    {
        probe.m_DXCoreFactory = std::move(factory);
        probe.m_IntegratedByLuid.clear();
    }

    /// Puts @p module where dxcore.dll goes; the probe frees it with dxcore.dll's handle, so pass one
    /// loaded for it. A module without DXCoreCreateAdapterFactory makes every factory creation fail.
    static void useDXCoreModule(DXGIGPUProbe& probe, void* module)
    {
        probe.m_DXCoreModule = module;
    }
};

} // namespace Platform

#endif // _WIN32
