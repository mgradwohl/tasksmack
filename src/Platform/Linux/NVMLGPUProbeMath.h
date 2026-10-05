#pragma once

// Pure, injectable-function-pointer logic extracted from NVMLGPUProbe.cpp so it can be
// unit-tested directly, without going through dlopen()/the NVML mock library. See
// CONTRIBUTING.md's "extract the pure decision logic into a small header" pattern.

#include "Platform/Linux/PciRuntimePm.h"
#include "Platform/NVMLTypes.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace Platform::NVMLGPUProbeMath
{

/// NVML_VALUE_NOT_AVAILABLE: what usedGpuMemory holds when NVML cannot report it (for example
/// in containers, or without permission to see another user's process).
inline constexpr std::uint64_t VALUE_NOT_AVAILABLE = std::numeric_limits<std::uint64_t>::max();

/// Size of one entry written by nvmlDeviceGet{Compute,Graphics}RunningProcesses. The legacy
/// unversioned symbols write nvmlProcessInfo_v1_t {pid, usedGpuMemory}; the _v2 and _v3 entry
/// points write nvmlProcessInfo_v2_t, which adds gpuInstanceId and computeInstanceId (#1092).
/// pid sits at offset 0 and usedGpuMemory at offset 8 in both.
inline constexpr std::size_t PROCESS_INFO_V1_SIZE = 16;
inline constexpr std::size_t PROCESS_INFO_V2_SIZE = 24;
inline constexpr std::size_t PROCESS_INFO_MEMORY_OFFSET = 8;

/// Upper bound on the entries we allocate for, whatever the driver reports: a corrupt count must
/// not drive the sampler into repeated multi-gigabyte allocations. Matches the Windows probe.
inline constexpr unsigned int MAX_PLAUSIBLE_PROCESS_COUNT = 65536;

/// A running-process entry point chosen by chooseRunningProcessesSymbol().
struct RunningProcessesSymbol
{
    void* address = nullptr; ///< nullptr when no variant is exported
    std::string name;
    std::size_t entrySize = 0;
};

/// Picks the newest running-process entry point for `baseName` (for example
/// "nvmlDeviceGetComputeRunningProcesses"): _v3, then _v2, both writing 24-byte entries, then
/// the legacy unversioned symbol, which writes 16-byte entries (#1092). `resolve(name)` returns
/// the symbol's address, or nullptr if it isn't exported.
template<typename Resolve>
[[nodiscard]] RunningProcessesSymbol chooseRunningProcessesSymbol(std::string_view baseName, const Resolve& resolve)
{
    struct Candidate
    {
        std::string_view suffix;
        std::size_t entrySize = 0;
    };
    constexpr std::array<Candidate, 3> CANDIDATES{{
        {.suffix = "_v3", .entrySize = PROCESS_INFO_V2_SIZE},
        {.suffix = "_v2", .entrySize = PROCESS_INFO_V2_SIZE},
        {.suffix = "", .entrySize = PROCESS_INFO_V1_SIZE},
    }};
    for (const auto& candidate : CANDIDATES)
    {
        std::string name(baseName);
        name += candidate.suffix;
        if (void* address = resolve(name); address != nullptr)
        {
            return {.address = address, .name = std::move(name), .entrySize = candidate.entrySize};
        }
    }
    return {};
}

/// A process's GPU instance and compute instance ids are only in the 24-byte entries; for MIG
/// partitions one process can be listed once per instance (#1213 review).
inline constexpr std::uint32_t NO_INSTANCE_ID = std::numeric_limits<std::uint32_t>::max();
inline constexpr std::size_t PROCESS_INFO_GPU_INSTANCE_OFFSET = 16;
inline constexpr std::size_t PROCESS_INFO_COMPUTE_INSTANCE_OFFSET = 20;

struct RunningProcess
{
    std::uint32_t pid = 0;
    std::optional<std::uint64_t> usedGpuMemoryBytes; ///< nullopt when NVML reports it unavailable
    std::uint32_t gpuInstanceId = NO_INSTANCE_ID;    ///< NO_INSTANCE_ID for the legacy 16-byte entries
    std::uint32_t computeInstanceId = NO_INSTANCE_ID;
};

/// Lists the processes running on a device through one of the running-process entry points.
/// `query(unsigned int* count, void* buffer)` forwards to it. NVML answers a count-only call
/// (count 0, null buffer) with NVML_ERROR_INSUFFICIENT_SIZE and the needed count whenever any
/// process is running, and with NVML_SUCCESS only when none are. The list can also grow between
/// the two calls, so the sized call is retried a few times with headroom. Counts are capped at
/// MAX_PLAUSIBLE_PROCESS_COUNT; a list the driver says is longer than that is treated as unreadable.
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
        if (count > MAX_PLAUSIBLE_PROCESS_COUNT)
        {
            return {};
        }
        count = std::min(count + HEADROOM, MAX_PLAUSIBLE_PROCESS_COUNT);
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
        std::memcpy(&usedGpuMemory, entry + PROCESS_INFO_MEMORY_OFFSET, sizeof(usedGpuMemory));
        RunningProcess process{.pid = pid,
                               .usedGpuMemoryBytes = (usedGpuMemory == VALUE_NOT_AVAILABLE) ? std::nullopt : std::optional{usedGpuMemory}};
        if (entrySize >= PROCESS_INFO_V2_SIZE)
        {
            std::memcpy(&process.gpuInstanceId, entry + PROCESS_INFO_GPU_INSTANCE_OFFSET, sizeof(process.gpuInstanceId));
            std::memcpy(&process.computeInstanceId, entry + PROCESS_INFO_COMPUTE_INSTANCE_OFFSET, sizeof(process.computeInstanceId));
        }
        processes.push_back(process);
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

/// One process's GPU use on one device, combined from the compute and graphics lists.
struct ProcessUsage
{
    std::uint32_t pid = 0;
    std::uint64_t memoryBytes = 0;
    bool compute = false;
    bool graphics = false;
};

/// Combines one device's compute and graphics lists into one entry per process, in first-seen
/// order. The two lists report the same allocation for a process on one instance, so within an
/// instance the larger figure is taken; under MIG a process can run on several GPU/compute
/// instances, each with its own memory, so those are summed (#1213 review). Legacy 16-byte entries
/// carry no instance ids and count as a single instance.
[[nodiscard]] inline std::vector<ProcessUsage> combineRunningProcesses(const std::vector<RunningProcess>& compute,
                                                                       const std::vector<RunningProcess>& graphics)
{
    using InstanceKey = std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>; // pid, GPU instance, compute instance
    std::map<InstanceKey, std::uint64_t> memoryByInstance;
    std::vector<ProcessUsage> usages;
    std::map<std::uint32_t, std::size_t> usageByPid;

    const auto add = [&](const RunningProcess& process, bool isCompute)
    {
        const InstanceKey key{process.pid, process.gpuInstanceId, process.computeInstanceId};
        auto& instanceMemory = memoryByInstance[key];
        instanceMemory = std::max(instanceMemory, process.usedGpuMemoryBytes.value_or(0));

        auto [it, inserted] = usageByPid.try_emplace(process.pid, usages.size());
        if (inserted)
        {
            usages.push_back(ProcessUsage{.pid = process.pid});
        }
        auto& usage = usages[it->second];
        (isCompute ? usage.compute : usage.graphics) = true;
    };
    for (const auto& process : compute)
    {
        add(process, true);
    }
    for (const auto& process : graphics)
    {
        add(process, false);
    }

    for (const auto& [key, memoryBytes] : memoryByInstance)
    {
        usages[usageByPid.at(std::get<0>(key))].memoryBytes += memoryBytes;
    }
    return usages;
}

/// The sysfs name ("0000:01:00.0") of the PCI device NVML describes, for its power/runtime_status
/// (#1117). NVML's domain is 32-bit and printed with eight digits in busId ("00000000:01:00.0"),
/// while sysfs prints at least four, so the name is rebuilt from the fields; only the function
/// number has no field of its own and comes from busId's ".F" suffix (0 if it can't be read).
[[nodiscard]] inline std::string sysfsPciAddress(const NVML::nvmlPciInfo_t& pci)
{
    const std::string_view busId(std::data(pci.busId), ::strnlen(std::data(pci.busId), std::size(pci.busId)));
    std::uint32_t function = 0;
    if (const auto dot = busId.rfind('.'); dot != std::string_view::npos && dot + 1 < busId.size())
    {
        const char digit = busId[dot + 1];
        if (digit >= '0' && digit <= '7')
        {
            function = static_cast<std::uint32_t>(digit - '0');
        }
    }
    return PciRuntimePm::pciAddress(pci.domain, pci.bus, pci.device, function);
}

} // namespace Platform::NVMLGPUProbeMath
