#include "WindowsPlatformSecurity.h"

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
// clang-format on

#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
#include <intrin.h> // __cpuid: the CPU vendor
#endif

#pragma comment(lib, "advapi32.lib")

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>

namespace Platform::WindowsPlatformSecurity
{

namespace
{

static_assert(TBS_SUCCESS_CODE == TBS_SUCCESS);
static_assert(TBS_E_TPM_NOT_FOUND_CODE == static_cast<std::uint32_t>(TBS_E_TPM_NOT_FOUND));
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
    const TbsiGetDeviceInfoFn fn = tbsiGetDeviceInfo();
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
    const Windows::NtQuerySystemInformationFn query = Windows::ntQuerySystemInformation();
    if (query == nullptr)
    {
        return STATUS_NOT_IMPLEMENTED_CODE;
    }
    return query(infoClass, buffer.data(), static_cast<ULONG>(buffer.size()), nullptr);
}

[[nodiscard]] std::string cpuVendor()
{
#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
    std::array<int, 4> registers{};
    __cpuid(registers.data(), 0);
    // The vendor string is EBX, EDX, ECX.
    std::array<char, 3 * sizeof(int)> vendor{};
    std::memcpy(vendor.data(), &registers[1], sizeof(int));
    std::memcpy(vendor.data() + sizeof(int), &registers[3], sizeof(int));
    std::memcpy(vendor.data() + (2 * sizeof(int)), &registers[2], sizeof(int));
    return {vendor.data(), vendor.size()};
#else
    return {};
#endif
}

} // namespace

Functions systemFunctions()
{
    return {
        .secureBootEnabledValue = &secureBootEnabledValue,
        .bootedWithUefi = &bootedWithUefi,
        .tpmDeviceInfo = &tpmDeviceInfo,
        .querySystemInformation = &querySystemInformation,
        .cpuVendor = &cpuVendor,
    };
}

} // namespace Platform::WindowsPlatformSecurity
