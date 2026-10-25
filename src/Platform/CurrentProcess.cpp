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
#include <utility>

namespace Platform
{

std::int32_t currentProcessId() noexcept
{
#ifdef _WIN32
    // A Windows PID is a DWORD with no guarantee of fitting an int32; one that does not is reported
    // as 0 (unknown), so self-detection never uses a wrapped value (#804 review).
    const auto pid = GetCurrentProcessId();
#else
    const auto pid = getpid();
#endif
    // Checked here, not with Domain::Numeric::narrowOr(): Platform has no Domain dependency.
    return std::in_range<std::int32_t>(pid) ? static_cast<std::int32_t>(pid) : std::int32_t{0};
}

} // namespace Platform
