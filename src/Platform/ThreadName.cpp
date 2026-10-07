#include "Platform/ThreadName.h"

#if defined(_WIN32)
// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <processthreadsapi.h>
// clang-format on

#include "Platform/Windows/WinString.h"
#else
#include <pthread.h>
#endif

#include <exception>
#include <string>
#include <string_view>

namespace Platform
{

bool setCurrentThreadName(std::string_view name) noexcept
{
    try
    {
#if defined(_WIN32)
        const std::wstring wide = WinString::utf8ToWide(name);
        return SUCCEEDED(SetThreadDescription(GetCurrentThread(), wide.c_str()));
#else
        const std::string truncated = truncateThreadName(name);
        return pthread_setname_np(pthread_self(), truncated.c_str()) == 0;
#endif
    }
    catch (const std::exception&)
    {
        // Allocation failure building the name: naming is diagnostic only, so report and move on.
        return false;
    }
}

bool setMainThreadName([[maybe_unused]] std::string_view name) noexcept
{
#if defined(_WIN32)
    return setCurrentThreadName(name);
#else
    return true;
#endif
}

} // namespace Platform
