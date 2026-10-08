#include "Platform/CurrentProcess.h"

#include "Domain/Numeric.h"

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
    // A Windows PID is a DWORD with no guarantee of fitting an int32; one that does not is reported
    // as 0 (unknown), as WindowsProcessProbe does, so self-detection never uses a wrapped value
    // (#804 review).
    return Domain::Numeric::narrowOr<std::int32_t>(GetCurrentProcessId(), std::int32_t{0});
#else
    return Domain::Numeric::narrowOr<std::int32_t>(getpid(), std::int32_t{0});
#endif
}

} // namespace Platform
