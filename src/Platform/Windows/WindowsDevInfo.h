#pragma once

// The owner for a SetupAPI device information set (#4367): what SetupDiGetClassDevsW returns, destroyed
// with SetupDiDestroyDeviceInfoList. Its own header, not WindowsHandles.h, so setupapi.h stays out of
// everything that includes that one.

#ifdef _WIN32
// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <setupapi.h>
// clang-format on

#include "WindowsHandles.h"

namespace Platform::Windows
{

/// Destroys device information sets with SetupDiDestroyDeviceInfoList. SetupDiGetClassDevsW fails with
/// INVALID_HANDLE_VALUE and never returns null, so both count as "no set", as for a kernel handle.
struct DevInfoTraits
{
    using Type = HDEVINFO;

    [[nodiscard]] static Type invalid() noexcept
    {
        return nullptr;
    }

    [[nodiscard]] static bool isValid(Type devices) noexcept
    {
        return devices != nullptr && devices != INVALID_HANDLE_VALUE;
    }

    static void close(Type devices) noexcept
    {
        static_cast<void>(SetupDiDestroyDeviceInfoList(devices));
    }
};

/// An owned device information set. Construct it from SetupDiGetClassDevsW's result and test it, so a
/// failure is never destroyed.
using UniqueDevInfo = UniqueResource<DevInfoTraits>;

} // namespace Platform::Windows

#endif // _WIN32
