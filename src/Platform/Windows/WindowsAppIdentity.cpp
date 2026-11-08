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

struct WindowWalk
{
    std::span<const std::uint32_t> pids;
    std::vector<std::pair<std::uint32_t, HWND>> windows;
};

/// EnumWindows' callback for windowAppUserModelIds(): each visible, unowned top-level window of a process asked about.
BOOL CALLBACK collectAppWindow(HWND window, LPARAM param)
{
    if (IsWindowVisible(window) == FALSE || GetWindow(window, GW_OWNER) != nullptr || IsHungAppWindow(window) != FALSE)
    {
        return TRUE;
    }
    DWORD pid = 0;
    if (GetWindowThreadProcessId(window, &pid) == 0 || pid == 0)
    {
        return TRUE;
    }
    // NOLINTNEXTLINE(performance-no-int-to-ptr,cppcoreguidelines-pro-type-reinterpret-cast) - EnumWindows' LPARAM
    auto* walk = reinterpret_cast<WindowWalk*>(param);
    if (std::ranges::binary_search(walk->pids, static_cast<std::uint32_t>(pid)))
    {
        walk->windows.emplace_back(static_cast<std::uint32_t>(pid), window);
    }
    return TRUE;
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
    std::string id;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-union-access) - PROPVARIANT is a tagged union; vt says which
    if (SUCCEEDED(store->GetValue(APP_USER_MODEL_ID_KEY, &value)) && value.vt == VT_LPWSTR && value.pwszVal != nullptr)
    {
        id = WinString::wideToUtf8(value.pwszVal); // NOLINT(cppcoreguidelines-pro-type-union-access) - checked vt above
    }
    static_cast<void>(PropVariantClear(&value));
    return id;
}

} // namespace

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
    WindowWalk walk{.pids = pids, .windows = {}};
    // The callback never stops the walk; a failure leaves what was collected.
    static_cast<void>(EnumWindows(&collectAppWindow, reinterpret_cast<LPARAM>(&walk)));
    for (const auto& [pid, window] : walk.windows)
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
    if (FAILED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &name)) || name == nullptr)
    {
        return {};
    }
    const std::string text = WinString::wideToUtf8(name);
    CoTaskMemFree(name);
    // A name the Apps folder doesn't know comes back as the parsing name itself: no name, then.
    return (text == appUserModelId) ? std::string{} : text;
}

// NOLINTEND(misc-include-cleaner)

} // namespace Platform::Windows
