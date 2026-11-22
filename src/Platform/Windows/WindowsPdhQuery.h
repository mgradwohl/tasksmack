#pragma once

// The owner for a PDH query (#4367): what PdhOpenQueryW opens, closed with PdhCloseQuery, which also
// frees the counters added to it. Its own header, not WindowsHandles.h, so pdh.h (whose HLOG clashes
// with lm.h's) stays out of everything that includes that one.

#ifdef _WIN32
// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <pdh.h>
// clang-format on

#include "WindowsHandles.h"

namespace Platform::Windows
{

/// Closes PDH queries with PdhCloseQuery. PdhOpenQueryW leaves its out-parameter null when it fails.
struct PdhQueryTraits
{
    using Type = PDH_HQUERY;

    [[nodiscard]] static Type invalid() noexcept
    {
        return nullptr;
    }

    [[nodiscard]] static bool isValid(Type query) noexcept
    {
        return query != nullptr;
    }

    static void close(Type query) noexcept
    {
        static_cast<void>(PdhCloseQuery(query));
    }
};

/// An owned PDH query. Fill one from PdhOpenQueryW's out-parameter with put().
using UniquePdhQuery = UniqueResource<PdhQueryTraits>;

} // namespace Platform::Windows

#endif // _WIN32
