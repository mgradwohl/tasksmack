#pragma once

// The rated base clock WindowsSystemProbe scales by "% Processor Performance" (#1530). In its own
// header, with CallNtPowerInformation injectable, so tests can drive the read with fabricated
// processor tables and failures instead of whatever this machine reports.

#include "Platform/Windows/WindowsSystemProbeMath.h"

#include <spdlog/spdlog.h>

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <powerbase.h> // CallNtPowerInformation (powrprof)
// clang-format on

#include <algorithm>
#include <cstdint>
#include <vector>

namespace Platform
{

/// CallNtPowerInformation's signature: powrprof's export, or a test's fake. NTSTATUS is a LONG.
using CallNtPowerInformationFn = LONG(WINAPI*)(POWER_INFORMATION_LEVEL, PVOID, ULONG, PVOID, ULONG);

/// PROCESSOR_POWER_INFORMATION, which is documented but not declared in the SDK's headers.
struct ProcessorPowerInformation
{
    ULONG number;
    ULONG maxMhz;
    ULONG currentMhz;
    ULONG mhzLimit;
    ULONG maxIdleState;
    ULONG currentIdleState;
};

/// Each logical processor's rated base clock (MaxMhz) from `callNtPowerInformation(ProcessorInformation)`
/// -- no admin, no WMI; empty if the call fails.
[[nodiscard]] inline std::vector<std::uint32_t> readProcessorMaxMHz(CallNtPowerInformationFn callNtPowerInformation)
{
    // One entry per processor; sized for every group, and entries left unwritten stay 0 (ignored)
    std::vector<ProcessorPowerInformation> info(std::max<DWORD>(GetMaximumProcessorCount(ALL_PROCESSOR_GROUPS), 1));
    const LONG status = callNtPowerInformation(
        ProcessorInformation, nullptr, 0, info.data(), static_cast<ULONG>(info.size() * sizeof(ProcessorPowerInformation)));
    if (status != 0) // STATUS_SUCCESS = 0
    {
        spdlog::debug("WindowsSystemProbe: CallNtPowerInformation(ProcessorInformation) failed ({:#x}); using ~MHz as the base clock",
                      static_cast<std::uint32_t>(status));
        return {};
    }
    std::vector<std::uint32_t> maxMHz;
    maxMHz.reserve(info.size());
    for (const ProcessorPowerInformation& processor : info)
    {
        maxMHz.push_back(processor.maxMhz);
    }
    return maxMHz;
}

/// The nominal base clock "% Processor Performance" scales: the highest rated MaxMhz read through
/// `callNtPowerInformation`, else `registryMHz` (the registry's ~MHz); 0 if neither is known.
[[nodiscard]] inline std::uint64_t readNominalCpuBaseMHz(CallNtPowerInformationFn callNtPowerInformation, std::uint64_t registryMHz)
{
    const std::vector<std::uint32_t> maxMHz = readProcessorMaxMHz(callNtPowerInformation);
    return nominalCpuBaseMHz(maxMHz, registryMHz);
}

} // namespace Platform
