#include "Platform/CurrentProcess.h"

#ifdef _WIN32
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
#else
#include <unistd.h>
#endif

#include <cstdint>

namespace Platform
{

std::int32_t currentProcessId() noexcept
{
#ifdef _WIN32
    // A Windows PID is a DWORD, but every PID the system hands out fits the int32 the process
    // snapshots use (they are multiples of 4, well below 2^31).
    return static_cast<std::int32_t>(GetCurrentProcessId());
#else
    return static_cast<std::int32_t>(getpid());
#endif
}

} // namespace Platform
