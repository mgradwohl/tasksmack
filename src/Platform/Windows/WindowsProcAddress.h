#pragma once

#ifdef _WIN32
// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// clang-format on

#include <string_view>
#include <type_traits>

namespace Platform::Windows
{

/// Type-safe wrapper for GetProcAddress that returns the requested function pointer type.
/// @tparam T The function pointer type to return (must be a pointer type)
/// @param module The module handle to search for the procedure
/// @param procName The name of the procedure to find (must be null-terminated)
/// @return The function pointer cast to type T, or nullptr if not found
template<typename T> [[nodiscard]] T getProcAddress(HMODULE module, std::string_view procName) noexcept
{
    static_assert(std::is_pointer_v<T>, "getProcAddress<T>: T must be a pointer type");

    if (module == nullptr)
    {
        return nullptr;
    }

    // GetProcAddress requires null-terminated string; std::string_view::data() is safe
    // because callers pass string literals or std::string which are null-terminated.
    // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage) - callers guarantee null-termination
    FARPROC proc = GetProcAddress(module, procName.data());
    if (proc == nullptr)
    {
        return nullptr;
    }

    static_assert(sizeof(T) == sizeof(proc), "getProcAddress<T>: pointer size mismatch");

    // FARPROC is a function pointer type; converting to another function pointer type is inherently platform/ABI-specific.
    // We keep it isolated here.
    return reinterpret_cast<T>(proc); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
}

} // namespace Platform::Windows

#endif // _WIN32
