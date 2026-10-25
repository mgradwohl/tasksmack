#include "WindowsPlatformSecurity.h"

#include "ComPtr.h"
#include "ComScope.h"
#include "Platform/Windows/WindowsHandles.h"
#include "Platform/Windows/WindowsNtQuery.h"
#include "Platform/Windows/WindowsProcAddress.h"

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tbs.h>
#include <objbase.h>
#include <propsys.h>
#include <shobjidl_core.h> // SHGetPropertyStoreFromParsingName
// clang-format on

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "uuid.lib") // IID_IPropertyStore

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Platform::WindowsPlatformSecurity
{

namespace
{

static_assert(TBS_SUCCESS_CODE == TBS_SUCCESS);
constexpr auto TPM_NOT_FOUND = static_cast<std::uint32_t>(TBS_E_TPM_NOT_FOUND);
static_assert(TBS_E_TPM_NOT_FOUND_CODE == TPM_NOT_FOUND);
static_assert(TPM_VERSION_12_CODE == TPM_VERSION_12 && TPM_VERSION_20_CODE == TPM_VERSION_20);

/// UEFISecureBootEnabled, as Windows records it at boot.
[[nodiscard]] std::optional<std::uint32_t> secureBootEnabledValue()
{
    DWORD value = 0;
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_LOCAL_MACHINE,
                     L"SYSTEM\\CurrentControlSet\\Control\\SecureBoot\\State",
                     L"UEFISecureBootEnabled",
                     RRF_RT_REG_DWORD,
                     nullptr,
                     &value,
                     &size) != ERROR_SUCCESS)
    {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(value);
}

[[nodiscard]] std::optional<bool> bootedWithUefi()
{
    FIRMWARE_TYPE type = FirmwareTypeUnknown;
    if (GetFirmwareType(&type) == FALSE || (type != FirmwareTypeUefi && type != FirmwareTypeBios))
    {
        return std::nullopt;
    }
    return type == FirmwareTypeUefi;
}

/// Tbsi_GetDeviceInfo from tbs.dll, loaded from System32 the first time it is asked for and kept for
/// the process's life; null when either is missing.
using TbsiGetDeviceInfoFn = TBS_RESULT(WINAPI*)(UINT32 size, PVOID info);

[[nodiscard]] TbsiGetDeviceInfoFn tbsiGetDeviceInfo()
{
    static const Windows::UniqueModule tbs(LoadLibraryExW(L"tbs.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32));
    static const auto fn = Windows::getProcAddress<TbsiGetDeviceInfoFn>(tbs.get(), "Tbsi_GetDeviceInfo");
    return fn;
}

[[nodiscard]] std::optional<std::uint32_t> tpmDeviceInfo(std::uint32_t& version)
{
    TbsiGetDeviceInfoFn fn = tbsiGetDeviceInfo();
    if (fn == nullptr)
    {
        return std::nullopt;
    }
    TPM_DEVICE_INFO info{};
    const TBS_RESULT result = fn(sizeof(info), &info);
    version = result == TBS_SUCCESS ? info.tpmVersion : 0;
    return result;
}

[[nodiscard]] std::int32_t querySystemInformation(std::uint32_t infoClass, std::span<std::byte> buffer)
{
    constexpr LONG STATUS_NOT_IMPLEMENTED_CODE = static_cast<LONG>(0xC000'0002U);
    Windows::NtQuerySystemInformationFn query = Windows::ntQuerySystemInformation();
    if (query == nullptr)
    {
        return STATUS_NOT_IMPLEMENTED_CODE;
    }
    return query(infoClass, buffer.data(), static_cast<ULONG>(buffer.size()), nullptr);
}

/// The drive letters GetLogicalDrives can report.
constexpr int DRIVE_LETTERS = 26;

[[nodiscard]] std::optional<std::vector<std::string>> fixedVolumes()
{
    const DWORD drives = GetLogicalDrives();
    if (drives == 0)
    {
        return std::nullopt;
    }
    std::vector<std::string> volumes;
    for (int letter = 0; letter < DRIVE_LETTERS; ++letter)
    {
        if ((drives & (1U << static_cast<unsigned>(letter))) == 0)
        {
            continue;
        }
        const auto drive = static_cast<wchar_t>(L'A' + letter);
        const std::wstring root{drive, L':', L'\\'};
        if (GetDriveTypeW(root.c_str()) == DRIVE_FIXED)
        {
            volumes.push_back(std::string{static_cast<char>('A' + letter), ':'});
        }
    }
    return volumes;
}

// System.Volume.BitLockerProtection: not in the SDK's propkey.h, so spelled out as PSGetPropertyKeyFromName
// returns it for that name.
constexpr PROPERTYKEY BITLOCKER_PROTECTION_KEY{
    .fmtid = {.Data1 = 0x2d15a9a1, .Data2 = 0xa556, .Data3 = 0x4189, .Data4 = {0x91, 0xad, 0x02, 0x74, 0x58, 0xf1, 0x1a, 0x07}},
    .pid = 1717,
};

/// COM is initialised (apartment-threaded, as the shell expects) for the call, on the reading thread.
[[nodiscard]] std::optional<std::uint32_t> bitLockerProtection(const std::string& volume)
{
    const ComScope com;
    if (!com.usable())
    {
        return std::nullopt;
    }
    std::wstring root(volume.begin(), volume.end());
    root += L'\\';
    ComPtr<IPropertyStore> store;
    if (FAILED(SHGetPropertyStoreFromParsingName(
            root.c_str(), nullptr, GPS_DEFAULT, IID_IPropertyStore, reinterpret_cast<void**>(store.releaseAndGetAddressOf()))) ||
        !store)
    {
        return std::nullopt;
    }
    PROPVARIANT value;
    PropVariantInit(&value);
    std::optional<std::uint32_t> protection;
    // NOLINTBEGIN(cppcoreguidelines-pro-type-union-access) - PROPVARIANT is a tagged union; vt is checked first
    if (SUCCEEDED(store->GetValue(BITLOCKER_PROTECTION_KEY, &value)))
    {
        if (value.vt == VT_I4 && value.lVal >= 0)
        {
            protection = static_cast<std::uint32_t>(value.lVal);
        }
        else if (value.vt == VT_UI4)
        {
            protection = static_cast<std::uint32_t>(value.ulVal);
        }
    }
    // NOLINTEND(cppcoreguidelines-pro-type-union-access)
    PropVariantClear(&value);
    return protection;
}

} // namespace

Functions systemFunctions()
{
    return {
        .secureBootEnabledValue = &secureBootEnabledValue,
        .bootedWithUefi = &bootedWithUefi,
        .tpmDeviceInfo = &tpmDeviceInfo,
        .querySystemInformation = &querySystemInformation,
        .fixedVolumes = &fixedVolumes,
        .bitLockerProtection = &bitLockerProtection,
    };
}

} // namespace Platform::WindowsPlatformSecurity
