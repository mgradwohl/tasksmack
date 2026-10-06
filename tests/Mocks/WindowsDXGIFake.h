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
#pragma clang diagnostic pop
// clang-format on

namespace Platform
{
namespace DXGIFake
{

/// One fake adapter: its descriptor, and the PCI location the fake D3DKMT reports for its LUID.
struct FakeAdapter
{
    DXGI_ADAPTER_DESC1 desc{};
    std::optional<PciLocation> pciLocation;
};

struct FakeDXGIState
{
    std::vector<FakeAdapter> adapters;
    // Bumped whenever the adapter set changes: a factory made before is no longer current.
    int generation = 0;
    int factoriesCreated = 0;
    bool failFactoryCreation = false;
    // Kernel adapters opened, so a test can see a per-LUID decision being made again.
    int adapterOpens = 0;
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

inline const FakeAdapter* adapterWithLuid(const LUID& luid)
{
    for (const auto& adapter : dxgiState().adapters)
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
    if (adapterWithLuid(open->AdapterLuid) == nullptr)
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
    const FakeAdapter* adapter = adapterWithLuid(opened[query->hAdapter - 1]);
    if (adapter == nullptr)
    {
        return FAKE_STATUS_INVALID_PARAMETER;
    }
    if (query->Type == KMTQAITYPE_ADAPTERADDRESS && query->PrivateDriverDataSize == sizeof(D3DKMT_ADAPTERADDRESS))
    {
        if (!adapter->pciLocation.has_value())
        {
            return FAKE_STATUS_INVALID_PARAMETER;
        }
        auto* address = static_cast<D3DKMT_ADAPTERADDRESS*>(query->pPrivateDriverData);
        address->BusNumber = adapter->pciLocation->bus;
        address->DeviceNumber = adapter->pciLocation->device;
        address->FunctionNumber = 0;
        return 0;
    }
    if (query->Type == KMTQAITYPE_ADAPTERTYPE && query->PrivateDriverDataSize == sizeof(D3DKMT_ADAPTERTYPE))
    {
        // A plain hardware render adapter: neither a software nor an indirect-display device.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-union-access) - D3DKMT_ADAPTERTYPE is a union of its bitfields and Value
        static_cast<D3DKMT_ADAPTERTYPE*>(query->pPrivateDriverData)->Value = 0;
        return 0;
    }
    return FAKE_STATUS_INVALID_PARAMETER;
}

inline NTSTATUS APIENTRY fakeCloseAdapter(const D3DKMT_CLOSEADAPTER* /*close*/)
{
    return 0;
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
};

} // namespace Platform

#endif // _WIN32
