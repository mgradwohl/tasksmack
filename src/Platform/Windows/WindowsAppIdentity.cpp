#include "WindowsAppIdentity.h"

#include "ComPtr.h"
#include "WinString.h"

// NOLINTBEGIN(misc-include-cleaner) - Windows umbrella headers; symbols come from implementation sub-headers
// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <appmodel.h>
#include <objbase.h>
#include <propidl.h>
#include <propsys.h>
#include <shellapi.h>
#include <shobjidl_core.h>
// clang-format on
// NOLINTEND(misc-include-cleaner)

#include <algorithm>
#include <cstdint>
#include <cwchar>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace Platform::Windows
{

// NOLINTBEGIN(misc-include-cleaner) - Windows APIs; Win32 types come from windows.h sub-headers
namespace
{

/// PKEY_AppUserModel_ID ({9F4C2855-9F79-4B39-A8D0-E1D42DE1D5F3}, 5), spelled out so no GUID library is needed.
constexpr PROPERTYKEY APP_USER_MODEL_ID_KEY{
    .fmtid = {.Data1 = 0x9F4C2855, .Data2 = 0x9F79, .Data3 = 0x4B39, .Data4 = {0xA8, 0xD0, 0xE1, 0xD4, 0x2D, 0xE1, 0xD5, 0xF3}},
    .pid = 5,
};

/// A string an appmodel call writes: asked for its length first, then read. Empty when it can't be.
template<typename Fn> [[nodiscard]] std::string readAppModelString(const Fn& read)
{
    UINT32 length = 0;
    if (read(&length, nullptr) != ERROR_INSUFFICIENT_BUFFER || length == 0)
    {
        return {}; // APPMODEL_ERROR_NO_PACKAGE / APPMODEL_ERROR_NO_APPLICATION, or the process gone
    }
    std::wstring text(length, L'\0');
    if (read(&length, text.data()) != ERROR_SUCCESS)
    {
        return {};
    }
    text.resize(std::wcslen(text.c_str()));
    return WinString::wideToUtf8(text);
}

/// PropVariantClear on a PROPVARIANT when the scope that read it ends, however it ends.
class PropVariantClearer
{
  public:
    explicit PropVariantClearer(PROPVARIANT* value) noexcept : m_Value(value)
    {}
    ~PropVariantClearer()
    {
        static_cast<void>(PropVariantClear(m_Value));
    }
    PropVariantClearer(const PropVariantClearer&) = delete;
    PropVariantClearer& operator=(const PropVariantClearer&) = delete;
    PropVariantClearer(PropVariantClearer&&) = delete;
    PropVariantClearer& operator=(PropVariantClearer&&) = delete;

  private:
    PROPVARIANT* m_Value;
};

/// Frees a string the shell allocated with CoTaskMemAlloc.
struct CoTaskMemDeleter
{
    void operator()(wchar_t* text) const noexcept
    {
        CoTaskMemFree(text);
    }
};

/// windowAppUserModelIds()' work for one window: keeps each visible, unowned, answering top-level window of a
/// process in @p pids (sorted). Its push can throw; guardedWindowCallback() keeps that inside the callback.
void collectAppWindow(HWND window, std::span<const std::uint32_t> pids, std::vector<std::pair<std::uint32_t, HWND>>& windows)
{
    if (IsWindowVisible(window) == FALSE || GetWindow(window, GW_OWNER) != nullptr || IsHungAppWindow(window) != FALSE)
    {
        return;
    }
    DWORD pid = 0;
    if (GetWindowThreadProcessId(window, &pid) == 0 || pid == 0)
    {
        return;
    }
    if (std::ranges::binary_search(pids, static_cast<std::uint32_t>(pid)))
    {
        windows.emplace_back(static_cast<std::uint32_t>(pid), window);
    }
}

/// The explicit AppUserModelID @p window sets, empty when it sets none.
[[nodiscard]] std::string windowAppUserModelId(HWND window)
{
    ComPtr<IPropertyStore> store;
    if (FAILED(SHGetPropertyStoreForWindow(
            window,
            IID_IPropertyStore,
            reinterpret_cast<void**>(store.releaseAndGetAddressOf())))) // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
    {
        return {};
    }
    PROPVARIANT value{};
    PropVariantInit(&value);
    const PropVariantClearer clearer(&value); // Cleared however this returns, a throwing conversion included
    std::string id;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-union-access) - PROPVARIANT is a tagged union; vt says which
    if (SUCCEEDED(store->GetValue(APP_USER_MODEL_ID_KEY, &value)) && value.vt == VT_LPWSTR && value.pwszVal != nullptr)
    {
        id = WinString::wideToUtf8(value.pwszVal); // NOLINT(cppcoreguidelines-pro-type-union-access) - checked vt above
    }
    return id;
}

} // namespace

BOOL CALLBACK guardedWindowCallback(HWND window, LPARAM param) noexcept
{
    // NOLINTNEXTLINE(performance-no-int-to-ptr,cppcoreguidelines-pro-type-reinterpret-cast) - EnumWindows' LPARAM
    auto* visitor = reinterpret_cast<WindowVisitor*>(param);
    if (visitor == nullptr || !visitor->visit)
    {
        return FALSE;
    }
    try
    {
        return visitor->visit(window) ? TRUE : FALSE;
    }
    catch (...) // Nothing may unwind through user32's frames: stop the walk and say why
    {
        visitor->failed = true;
        return FALSE;
    }
}

PackageIdentity readPackageIdentity(HANDLE process)
{
    PackageIdentity identity;
    if (process == nullptr)
    {
        return identity;
    }
    identity.family = readAppModelString([process](UINT32* length, PWSTR text) { return GetPackageFamilyName(process, length, text); });
    if (!identity.family.empty())
    {
        identity.appUserModelId =
            readAppModelString([process](UINT32* length, PWSTR text) { return GetApplicationUserModelId(process, length, text); });
    }
    return identity;
}

void windowAppUserModelIds(std::span<const std::uint32_t> pids, std::vector<WindowAppId>& found)
{
    found.clear();
    if (pids.empty())
    {
        return;
    }
    std::vector<std::pair<std::uint32_t, HWND>> windows;
    WindowVisitor visitor{
        .visit =
            [pids, &windows](HWND window)
        {
            collectAppWindow(window, pids, windows);
            return true; // Every window: the walk stops only if this throws
        },
        .failed = false,
    };
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) - EnumWindows' LPARAM
    static_cast<void>(EnumWindows(&guardedWindowCallback, reinterpret_cast<LPARAM>(&visitor)));
    if (visitor.failed)
    {
        return; // A walk cut short (out of memory) finds nothing; its processes are looked at again later
    }
    for (const auto& [pid, window] : windows)
    {
        if (std::ranges::find(found, pid, &WindowAppId::pid) != found.end())
        {
            continue; // Its first window that sets one already answered
        }
        if (std::string id = windowAppUserModelId(window); !id.empty())
        {
            found.push_back({.pid = pid, .appUserModelId = std::move(id)});
        }
    }
}

std::string appDisplayName(const std::string& appUserModelId)
{
    if (appUserModelId.empty())
    {
        return {};
    }
    const std::wstring path = L"shell:AppsFolder\\" + WinString::utf8ToWide(appUserModelId);
    ComPtr<IShellItem> item;
    if (FAILED(SHCreateItemFromParsingName(
            path.c_str(),
            nullptr,
            IID_IShellItem,
            reinterpret_cast<void**>(item.releaseAndGetAddressOf())))) // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
    {
        return {};
    }
    PWSTR name = nullptr;
    const HRESULT named = item->GetDisplayName(SIGDN_NORMALDISPLAY, &name);
    const std::unique_ptr<wchar_t, CoTaskMemDeleter> owned(name); // Freed however this returns
    if (FAILED(named) || owned == nullptr)
    {
        return {};
    }
    const std::string text = WinString::wideToUtf8(owned.get());
    // A name the Apps folder doesn't know comes back as the parsing name itself: no name, then.
    return (text == appUserModelId) ? std::string{} : text;
}

// NOLINTEND(misc-include-cleaner)

} // namespace Platform::Windows
