/// @file WindowsDXCoreFake.h
/// @brief A fake DXCore adapter factory, adapter list and adapter for the Windows DXGIGPUProbe (#2039).
///
/// The fakes implement the SDK's own IDXCoreAdapterFactory, IDXCoreAdapterList and IDXCoreAdapter
/// (dxcore_interface.h) and answer with the SDK's GUIDs (dxguid.lib), so the probe's spelled-out
/// hardware-type attributes are checked against the real ones. A test describes each adapter -- its
/// LUID, hardware-type attributes and property values -- and which calls fail, then installs a factory
/// with DXGIGPUProbeTestAccessor::useDXCoreFactory(). Every object counts itself in and out of
/// FakeDXCoreState::liveObjects, so a test can check that the probe released all it was handed.
/// Everything is inline, like WindowsDXGIFake.h.

#pragma once

#ifdef _WIN32

#include "Platform/Windows/ComPtr.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <optional>
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
#include <dxcore_interface.h>
#pragma clang diagnostic pop
// clang-format on

namespace Platform
{
namespace DXCoreFake
{

/// One fake DXCore adapter: what it reports, and which of its reads fail.
struct FakeCoreAdapter
{
    LUID luid{};
    /// The hardware-type and other attributes it supports (DXCORE_HARDWARE_TYPE_ATTRIBUTE_NPU, ...).
    std::vector<GUID> attributes;
    /// Each supported property's bytes; a property not here is unsupported.
    std::map<DXCoreAdapterProperty, std::vector<std::byte>> properties;
    /// Supported properties whose GetProperty() fails.
    std::vector<DXCoreAdapterProperty> failingReads;
    /// GetPropertySize() fails for every property.
    bool failPropertySize = false;
};

/// Set property @p property of @p adapter to @p value's bytes.
template<typename T> void setProperty(FakeCoreAdapter& adapter, DXCoreAdapterProperty property, const T& value)
{
    std::vector<std::byte> bytes(sizeof(T));
    std::memcpy(bytes.data(), &value, sizeof(T));
    adapter.properties[property] = std::move(bytes);
}

/// Set the adapter's DriverDescription to @p text and its terminating NUL, as DXCore reports it.
inline void setDescription(FakeCoreAdapter& adapter, std::string_view text)
{
    std::vector<std::byte> bytes(text.size() + 1, std::byte{0});
    std::memcpy(bytes.data(), text.data(), text.size());
    adapter.properties[DXCoreAdapterProperty::DriverDescription] = std::move(bytes);
}

/// An adapter with this LUID (low part) and these attributes, its InstanceLuid readable and nothing else set.
inline FakeCoreAdapter makeCoreAdapter(std::uint32_t luidLowPart, std::vector<GUID> attributes = {})
{
    FakeCoreAdapter adapter;
    adapter.luid.LowPart = luidLowPart;
    adapter.luid.HighPart = 0;
    adapter.attributes = std::move(attributes);
    setProperty(adapter, DXCoreAdapterProperty::InstanceLuid, adapter.luid);
    return adapter;
}

struct FakeDXCoreState
{
    std::vector<FakeCoreAdapter> adapters;
    /// CreateAdapterList() fails when asked for a list filtered by any of these attributes.
    std::vector<GUID> failingListAttributes;
    /// Every list's GetAdapter() fails at this index.
    std::optional<std::uint32_t> failGetAdapterAt;
    /// GetAdapterByLuid() fails for every LUID (it always fails for a LUID no adapter has).
    bool failGetAdapterByLuid = false;
    int adapterListsCreated = 0;
    int adaptersByLuid = 0;
    /// Fake COM objects not yet released by their last owner.
    int liveObjects = 0;
};

inline FakeDXCoreState& dxcoreState()
{
    static FakeDXCoreState state;
    return state;
}

/// The adapter with @p luid, or null when none has it.
inline const FakeCoreAdapter* adapterWithLuid(const LUID& luid)
{
    const auto it = std::ranges::find_if(dxcoreState().adapters,
                                         [&luid](const FakeCoreAdapter& adapter)
                                         { return adapter.luid.LowPart == luid.LowPart && adapter.luid.HighPart == luid.HighPart; });
    return it == dxcoreState().adapters.end() ? nullptr : &*it;
}

// NOLINTBEGIN(cppcoreguidelines-owning-memory,readability-identifier-naming,readability-convert-member-functions-to-static,cppcoreguidelines-special-member-functions,hicpp-special-member-functions,readability-non-const-parameter)
// - COM objects: self-deleting on the last Release(), with the interface's own method names.

/// IUnknown's reference counting and QueryInterface for one fake implementing @p Interface (@p iid).
template<typename Interface> class FakeUnknown : public Interface
{
  public:
    FakeUnknown(const FakeUnknown&) = delete;
    FakeUnknown& operator=(const FakeUnknown&) = delete;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override
    {
        if (riid == unknownId() || riid == interfaceId())
        {
            AddRef();
            *object = this;
            return S_OK;
        }
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

  protected:
    FakeUnknown()
    {
        ++dxcoreState().liveObjects;
    }
    virtual ~FakeUnknown()
    {
        --dxcoreState().liveObjects;
    }

    /// Hand this object out through @p object as @p riid, as a DXCore call does: AddRef'd, or
    /// E_NOINTERFACE and null when it isn't that interface. The caller's own reference is kept.
    HRESULT handOut(REFIID riid, void** object)
    {
        return QueryInterface(riid, object);
    }

  private:
    [[nodiscard]] static const IID& interfaceId();
    [[nodiscard]] static const IID& unknownId()
    {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wlanguage-extension-token"
        return __uuidof(IUnknown);
#pragma clang diagnostic pop
    }

    ULONG m_RefCount = 1;
};

template<> inline const IID& FakeUnknown<IDXCoreAdapter>::interfaceId()
{
    return IID_IDXCoreAdapter;
}
template<> inline const IID& FakeUnknown<IDXCoreAdapterList>::interfaceId()
{
    return IID_IDXCoreAdapterList;
}
template<> inline const IID& FakeUnknown<IDXCoreAdapterFactory>::interfaceId()
{
    return IID_IDXCoreAdapterFactory;
}

class FakeDXCoreAdapter final : public FakeUnknown<IDXCoreAdapter>
{
  public:
    explicit FakeDXCoreAdapter(FakeCoreAdapter adapter) : m_Adapter(std::move(adapter))
    {}

    /// A new adapter object for @p adapter, handed out as @p riid; the caller then owns the only reference.
    static HRESULT create(const FakeCoreAdapter& adapter, REFIID riid, void** object)
    {
        auto* created = new FakeDXCoreAdapter(adapter);
        const HRESULT hr = created->handOut(riid, object);
        created->Release();
        return hr;
    }

    bool STDMETHODCALLTYPE IsValid() override
    {
        return true;
    }
    bool STDMETHODCALLTYPE IsAttributeSupported(REFGUID attributeGUID) override
    {
        return std::ranges::find(m_Adapter.attributes, attributeGUID) != m_Adapter.attributes.end();
    }
    bool STDMETHODCALLTYPE IsPropertySupported(DXCoreAdapterProperty property) override
    {
        return m_Adapter.properties.contains(property);
    }
    HRESULT STDMETHODCALLTYPE GetProperty(DXCoreAdapterProperty property, size_t bufferSize, void* propertyData) override
    {
        const auto it = m_Adapter.properties.find(property);
        if (it == m_Adapter.properties.end() || propertyData == nullptr)
        {
            return E_INVALIDARG;
        }
        if (std::ranges::find(m_Adapter.failingReads, property) != m_Adapter.failingReads.end())
        {
            return E_FAIL;
        }
        if (bufferSize < it->second.size())
        {
            return E_INVALIDARG;
        }
        std::memcpy(propertyData, it->second.data(), it->second.size());
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPropertySize(DXCoreAdapterProperty property, size_t* bufferSize) override
    {
        const auto it = m_Adapter.properties.find(property);
        if (it == m_Adapter.properties.end() || m_Adapter.failPropertySize)
        {
            return E_INVALIDARG;
        }
        *bufferSize = it->second.size();
        return S_OK;
    }
    bool STDMETHODCALLTYPE IsQueryStateSupported(DXCoreAdapterState /*property*/) override
    {
        return false;
    }
    HRESULT STDMETHODCALLTYPE QueryState(DXCoreAdapterState /*state*/,
                                         size_t /*inputStateDetailsSize*/,
                                         const void* /*inputStateDetails*/,
                                         size_t /*outputBufferSize*/,
                                         void* /*outputBuffer*/) override
    {
        return E_NOTIMPL;
    }
    bool STDMETHODCALLTYPE IsSetStateSupported(DXCoreAdapterState /*property*/) override
    {
        return false;
    }
    HRESULT STDMETHODCALLTYPE SetState(DXCoreAdapterState /*state*/,
                                       size_t /*inputStateDetailsSize*/,
                                       const void* /*inputStateDetails*/,
                                       size_t /*inputDataSize*/,
                                       const void* /*inputData*/) override
    {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE GetFactory(REFIID /*riid*/, void** factory) override
    {
        *factory = nullptr;
        return E_NOTIMPL;
    }

  private:
    ~FakeDXCoreAdapter() override = default;

    FakeCoreAdapter m_Adapter;
};

class FakeDXCoreAdapterList final : public FakeUnknown<IDXCoreAdapterList>
{
  public:
    /// The adapters with every one of @p attributes, as dxcoreState() lists them now.
    explicit FakeDXCoreAdapterList(const std::vector<GUID>& attributes)
    {
        for (const auto& adapter : dxcoreState().adapters)
        {
            if (std::ranges::all_of(attributes,
                                    [&adapter](const GUID& attribute)
                                    { return std::ranges::find(adapter.attributes, attribute) != adapter.attributes.end(); }))
            {
                m_Adapters.push_back(adapter);
            }
        }
    }

    /// A new list for @p attributes, handed out as @p riid; the caller then owns the only reference.
    static HRESULT create(const std::vector<GUID>& attributes, REFIID riid, void** object)
    {
        auto* created = new FakeDXCoreAdapterList(attributes);
        const HRESULT hr = created->handOut(riid, object);
        created->Release();
        return hr;
    }

    HRESULT STDMETHODCALLTYPE GetAdapter(uint32_t index, REFIID riid, void** adapter) override
    {
        if (index >= m_Adapters.size() || dxcoreState().failGetAdapterAt == index)
        {
            *adapter = nullptr;
            return E_INVALIDARG;
        }
        return FakeDXCoreAdapter::create(m_Adapters[index], riid, adapter);
    }
    uint32_t STDMETHODCALLTYPE GetAdapterCount() override
    {
        return static_cast<uint32_t>(m_Adapters.size());
    }
    bool STDMETHODCALLTYPE IsStale() override
    {
        return false;
    }
    HRESULT STDMETHODCALLTYPE GetFactory(REFIID /*riid*/, void** factory) override
    {
        *factory = nullptr;
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE Sort(uint32_t /*numPreferences*/, const DXCoreAdapterPreference* /*preferences*/) override
    {
        return E_NOTIMPL;
    }
    bool STDMETHODCALLTYPE IsAdapterPreferenceSupported(DXCoreAdapterPreference /*preference*/) override
    {
        return false;
    }

  private:
    ~FakeDXCoreAdapterList() override = default;

    // The adapters present when this list was made, as a real list holds them.
    std::vector<FakeCoreAdapter> m_Adapters;
};

class FakeDXCoreAdapterFactory final : public FakeUnknown<IDXCoreAdapterFactory>
{
  public:
    FakeDXCoreAdapterFactory() = default;

    HRESULT STDMETHODCALLTYPE CreateAdapterList(uint32_t numAttributes,
                                                const GUID* filterAttributes,
                                                REFIID riid,
                                                void** adapterList) override
    {
        *adapterList = nullptr;
        if (numAttributes == 0 || filterAttributes == nullptr)
        {
            return E_INVALIDARG;
        }
        const std::vector<GUID> attributes(
            filterAttributes,
            filterAttributes + numAttributes); // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic) - the COM call's array
        const auto& failing = dxcoreState().failingListAttributes;
        if (std::ranges::any_of(attributes,
                                [&failing](const GUID& attribute) { return std::ranges::find(failing, attribute) != failing.end(); }))
        {
            return E_FAIL;
        }
        ++dxcoreState().adapterListsCreated;
        return FakeDXCoreAdapterList::create(attributes, riid, adapterList);
    }
    HRESULT STDMETHODCALLTYPE GetAdapterByLuid(const LUID& adapterLUID, REFIID riid, void** adapter) override
    {
        *adapter = nullptr;
        const FakeCoreAdapter* found = adapterWithLuid(adapterLUID);
        if (found == nullptr || dxcoreState().failGetAdapterByLuid)
        {
            return E_INVALIDARG;
        }
        ++dxcoreState().adaptersByLuid;
        return FakeDXCoreAdapter::create(*found, riid, adapter);
    }
    bool STDMETHODCALLTYPE IsNotificationTypeSupported(DXCoreNotificationType /*notificationType*/) override
    {
        return false;
    }
    HRESULT STDMETHODCALLTYPE RegisterEventNotification(IUnknown* /*dxCoreObject*/,
                                                        DXCoreNotificationType /*notificationType*/,
                                                        PFN_DXCORE_NOTIFICATION_CALLBACK /*callbackFunction*/,
                                                        void* /*callbackContext*/,
                                                        uint32_t* eventCookie) override
    {
        *eventCookie = 0;
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE UnregisterEventNotification(uint32_t /*eventCookie*/) override
    {
        return E_NOTIMPL;
    }

  private:
    ~FakeDXCoreAdapterFactory() override = default;
};

// NOLINTEND(cppcoreguidelines-owning-memory,readability-identifier-naming,readability-convert-member-functions-to-static,cppcoreguidelines-special-member-functions,hicpp-special-member-functions,readability-non-const-parameter)

/// A new fake DXCore factory over dxcoreState()'s adapters.
inline ComPtr<IDXCoreAdapterFactory> createFakeDXCoreFactory()
{
    ComPtr<IDXCoreAdapterFactory> factory;
    *factory.releaseAndGetAddressOf() = new FakeDXCoreAdapterFactory(); // NOLINT(cppcoreguidelines-owning-memory) - COM, released by ComPtr
    return factory;
}

} // namespace DXCoreFake
} // namespace Platform

#endif // _WIN32
