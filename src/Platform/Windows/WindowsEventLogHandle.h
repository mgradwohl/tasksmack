#pragma once

// The owner for an event log handle (#4367): a query EvtQuery opened, or an event EvtNext returned,
// closed with EvtClose. Its own header, not WindowsHandles.h, so winevt.h stays out of everything that
// includes that one.

#ifdef _WIN32
// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winevt.h>
// clang-format on

#include "WindowsHandles.h"

namespace Platform::Windows
{

/// Closes EVT_HANDLEs with EvtClose. The calls that make one report failure as null.
struct EvtHandleTraits
{
    using Type = EVT_HANDLE;

    [[nodiscard]] static Type invalid() noexcept
    {
        return nullptr;
    }

    [[nodiscard]] static bool isValid(Type handle) noexcept
    {
        return handle != nullptr;
    }

    static void close(Type handle) noexcept
    {
        static_cast<void>(EvtClose(handle));
    }
};

/// An owned EVT_HANDLE. Fill one from EvtNext's out-parameter with put().
using UniqueEvtHandle = UniqueResource<EvtHandleTraits>;

} // namespace Platform::Windows

#endif // _WIN32
