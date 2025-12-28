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

#include <type_traits>

namespace Platform::Windows
{

// TODO: Accept std::string_view and pass .data() to GetProcAddress
template<typename T> [[nodiscard]] T getProcAddress(HMODULE module, const char* procName) noexcept
{
    static_assert(std::is_pointer_v<T>, "getProcAddress<T>: T must be a pointer type");

    if (module == nullptr)
    {
        return nullptr;
    }

    FARPROC proc = GetProcAddress(module, procName);
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
