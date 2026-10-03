#pragma once

// Pure, injectable-function-pointer logic extracted from NVMLGPUProbe.cpp so it can be
// unit-tested directly, without going through dlopen()/the NVML mock library. See
// CONTRIBUTING.md's "extract the pure decision logic into a small header" pattern.

#include "Platform/NVMLTypes.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace Platform::NVMLGPUProbeMath
{

/// NVML_VALUE_NOT_AVAILABLE: what usedGpuMemory holds when NVML cannot report it (for example
/// in containers, or without permission to see another user's process).
inline constexpr std::uint64_t kValueNotAvailable = std::numeric_limits<std::uint64_t>::max();

/// Size of one entry written by nvmlDeviceGet{Compute,Graphics}RunningProcesses. The legacy
/// unversioned symbols write nvmlProcessInfo_v1_t {pid, usedGpuMemory}; the _v2 and _v3 entry
/// points write nvmlProcessInfo_v2_t, which adds gpuInstanceId and computeInstanceId (#1092).
/// pid sits at offset 0 and usedGpuMemory at offset 8 in both.
inline constexpr std::size_t kProcessInfoV1Size = 16;
inline constexpr std::size_t kProcessInfoV2Size = 24;
inline constexpr std::size_t kProcessInfoMemoryOffset = 8;

/// Upper bound on the entries we allocate for, whatever the driver reports: a corrupt count must
/// not drive the sampler into repeated multi-gigabyte allocations. Matches the Windows probe.
inline constexpr unsigned int kMaxPlausibleProcessCount = 65536;

struct RunningProcess
{
    std::uint32_t pid = 0;
    std::optional<std::uint64_t> usedGpuMemoryBytes; ///< nullopt when NVML reports it unavailable
};

/// Lists the processes running on a device through one of the running-process entry points.
/// `query(unsigned int* count, void* buffer)` forwards to it. NVML answers a count-only call
/// (count 0, null buffer) with NVML_ERROR_INSUFFICIENT_SIZE and the needed count whenever any
/// process is running, and with NVML_SUCCESS only when none are. The list can also grow between
/// the two calls, so the sized call is retried a few times with headroom. Counts are capped at
/// kMaxPlausibleProcessCount; a list the driver says is longer than that is treated as unreadable.
template<typename Query> [[nodiscard]] std::vector<RunningProcess> queryRunningProcesses(const Query& query, std::size_t entrySize)
{
    constexpr int MAX_ATTEMPTS = 3;
    constexpr unsigned int HEADROOM = 4;

    unsigned int count = 0;
    NVML::nvmlReturn_t result = query(&count, nullptr);
    if (result == NVML::NVML_SUCCESS && count == 0)
    {
        return {};
    }
    if (result != NVML::NVML_SUCCESS && result != NVML::NVML_ERROR_INSUFFICIENT_SIZE)
    {
        return {};
    }

    std::vector<std::byte> buffer;
    result = NVML::NVML_ERROR_INSUFFICIENT_SIZE;
    for (int attempt = 0; attempt < MAX_ATTEMPTS && result == NVML::NVML_ERROR_INSUFFICIENT_SIZE; ++attempt)
    {
        if (count > kMaxPlausibleProcessCount)
        {
            return {};
        }
        count = std::min(count + HEADROOM, kMaxPlausibleProcessCount);
        buffer.assign(static_cast<std::size_t>(count) * entrySize, std::byte{0});
        result = query(&count, buffer.data());
    }
    if (result != NVML::NVML_SUCCESS)
    {
        return {};
    }

    const std::size_t entries = std::min(static_cast<std::size_t>(count), buffer.size() / entrySize);
    std::vector<RunningProcess> processes;
    processes.reserve(entries);
    for (std::size_t i = 0; i < entries; ++i)
    {
        const std::byte* entry = buffer.data() + (i * entrySize);
        unsigned int pid = 0;
        std::uint64_t usedGpuMemory = 0;
        std::memcpy(&pid, entry, sizeof(pid));
        std::memcpy(&usedGpuMemory, entry + kProcessInfoMemoryOffset, sizeof(usedGpuMemory));
        processes.push_back(
            {.pid = pid, .usedGpuMemoryBytes = (usedGpuMemory == kValueNotAvailable) ? std::nullopt : std::optional{usedGpuMemory}});
    }
    return processes;
}

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
