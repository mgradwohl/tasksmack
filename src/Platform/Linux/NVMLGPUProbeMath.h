#pragma once

// Pure, injectable-function-pointer logic extracted from NVMLGPUProbe.cpp so it can be
// unit-tested directly, without going through dlopen()/the NVML mock library. See
// CONTRIBUTING.md's "extract the pure decision logic into a small header" pattern.

#include "Platform/NVMLTypes.h"

#include <string>

namespace Platform::NVMLGPUProbeMath
{

using StatusStringFn = const char* (*) (Platform::NVML::nvmlReturn_t);

/// Resolves an NVML return code to a human-readable string via the (possibly unresolved)
/// nvmlErrorString function pointer, falling back to "Unknown NVML error" when the symbol
/// failed to load or the library itself returns null.
[[nodiscard]] inline std::string resolveErrorString(Platform::NVML::nvmlReturn_t result, StatusStringFn errorStringFn)
{
    if (errorStringFn != nullptr)
    {
        const char* errorStr = errorStringFn(result);
        if (errorStr != nullptr)
        {
            return {errorStr};
        }
    }
    return "Unknown NVML error";
}

} // namespace Platform::NVMLGPUProbeMath
