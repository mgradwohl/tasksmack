#pragma once

// Pure, injectable-function-pointer logic extracted from NVMLGPUProbe.cpp so it can be
// unit-tested directly, without going through dlopen()/the NVML mock library. See
// CONTRIBUTING.md's "extract the pure decision logic into a small header" pattern.

#include "Platform/Linux/PciRuntimePm.h"
#include "Platform/NVMLRunningProcesses.h"
#include "Platform/NVMLTypes.h"

#include <string>

namespace Platform::NVMLGPUProbeMath
{

// The running-process helpers moved to the platform-neutral Platform/NVMLRunningProcesses.h so the
// Windows probe shares them (#1313); re-exported here so the Linux probe and its tests are unchanged.
using NVMLRunningProcesses::chooseRunningProcessesSymbol;
using NVMLRunningProcesses::combineRunningProcesses;
using NVMLRunningProcesses::MAX_PLAUSIBLE_PROCESS_COUNT;
using NVMLRunningProcesses::NO_INSTANCE_ID;
using NVMLRunningProcesses::PROCESS_INFO_COMPUTE_INSTANCE_OFFSET;
using NVMLRunningProcesses::PROCESS_INFO_GPU_INSTANCE_OFFSET;
using NVMLRunningProcesses::PROCESS_INFO_MEMORY_OFFSET;
using NVMLRunningProcesses::PROCESS_INFO_V1_SIZE;
using NVMLRunningProcesses::PROCESS_INFO_V2_SIZE;
using NVMLRunningProcesses::ProcessUsage;
using NVMLRunningProcesses::queryRunningProcesses;
using NVMLRunningProcesses::RunningProcess;
using NVMLRunningProcesses::RunningProcessesSymbol;
using NVMLRunningProcesses::VALUE_NOT_AVAILABLE;

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

/// The sysfs name ("0000:01:00.0") of the PCI device NVML describes, for its power/runtime_status
/// (#1117). NVML's domain is 32-bit and printed with eight digits in busId ("00000000:01:00.0"),
/// while sysfs prints at least four, so the name is rebuilt from the fields; only the function
/// number has no field of its own and comes from busId's ".F" suffix (0 if it can't be read).
[[nodiscard]] inline std::string sysfsPciAddress(const NVML::nvmlPciInfo_t& pci)
{
    return PciRuntimePm::pciAddress(pci.domain, pci.bus, pci.device, NVML::pciFunction(pci).value_or(0));
}

} // namespace Platform::NVMLGPUProbeMath
