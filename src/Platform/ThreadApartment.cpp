#include "Platform/ThreadApartment.h"

#if defined(_WIN32)
// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
// clang-format on

#pragma comment(lib, "ole32.lib")
#endif

namespace Platform
{

MultiThreadedApartment::MultiThreadedApartment() noexcept
{
#if defined(_WIN32)
    // S_OK: entered; S_FALSE: already in the MTA, one more reference. RPC_E_CHANGED_MODE: the thread is
    // in an STA already, which stays as it is (COM can still be used there).
    const HRESULT result = CoInitializeEx(nullptr, static_cast<DWORD>(COINIT_MULTITHREADED) | static_cast<DWORD>(COINIT_DISABLE_OLE1DDE));
    m_Entered = SUCCEEDED(result);
#endif
}

MultiThreadedApartment::~MultiThreadedApartment()
{
#if defined(_WIN32)
    if (m_Entered)
    {
        CoUninitialize();
    }
#endif
}

SingleThreadedApartment::SingleThreadedApartment() noexcept
{
#if defined(_WIN32)
    // S_OK: entered; S_FALSE: already in an STA, one more reference. RPC_E_CHANGED_MODE: the thread is
    // in the MTA already, which stays as it is (COM can still be used there).
    const HRESULT result =
        CoInitializeEx(nullptr, static_cast<DWORD>(COINIT_APARTMENTTHREADED) | static_cast<DWORD>(COINIT_DISABLE_OLE1DDE));
    m_Entered = SUCCEEDED(result);
#endif
}

SingleThreadedApartment::~SingleThreadedApartment()
{
#if defined(_WIN32)
    if (m_Entered)
    {
        CoUninitialize();
    }
#endif
}

} // namespace Platform
