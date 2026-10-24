#pragma once

// NVML's per-process utilization (nvmlDeviceGetProcessUtilization, #2589): the sized query of the samples
// newer than the last ones seen, and each process's latest share. Pure logic over an injected function,
// so it is unit-tested without a driver. Used by the Linux NVML probe; Windows takes per-process
// utilization from PDH.

#include "Platform/NVMLTypes.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <vector>

namespace Platform::NVMLProcessUtilization
{

/// Upper bound on the samples allocated for, whatever count the driver reports.
inline constexpr unsigned int MAX_PLAUSIBLE_SAMPLE_COUNT = 65536;

/// One process's latest utilization on a device, in percent.
struct ProcessShare
{
    unsigned int smPercent = 0;
    unsigned int encoderPercent = 0;
    unsigned int decoderPercent = 0;
};

/// What one query gave.
struct Query
{
    bool ok = false;                             ///< The driver answered (no samples newer than lastSeen counts)
    std::map<std::uint32_t, ProcessShare> byPid; ///< Each process's newest sample
    unsigned long long newestTimeStamp = 0;      ///< The newest sample's time, or lastSeen when none
};

/// Asks @p fn (nvmlDeviceGetProcessUtilization bound to a device: (samples, count, lastSeen)) for the samples
/// newer than @p lastSeen: once for the count, once for them. NVML_ERROR_NOT_FOUND (none newer) is an answer
/// with no samples; any other failure isn't. A process with several samples keeps its newest; percentages
/// are held to 100.
template<typename Fn> [[nodiscard]] Query query(const Fn& fn, unsigned long long lastSeen)
{
    Query result;
    result.newestTimeStamp = lastSeen;
    unsigned int count = 0;
    NVML::nvmlReturn_t status = fn(nullptr, &count, lastSeen);
    if (status == NVML::NVML_ERROR_NOT_FOUND || (status == NVML::NVML_SUCCESS && count == 0))
    {
        result.ok = true;
        return result;
    }
    if (status != NVML::NVML_ERROR_INSUFFICIENT_SIZE || count == 0 || count > MAX_PLAUSIBLE_SAMPLE_COUNT)
    {
        return result;
    }
    std::vector<NVML::nvmlProcessUtilizationSample_t> samples(count);
    status = fn(samples.data(), &count, lastSeen);
    if (status == NVML::NVML_ERROR_NOT_FOUND)
    {
        result.ok = true;
        return result;
    }
    if (status != NVML::NVML_SUCCESS)
    {
        return result;
    }
    samples.resize(std::min<std::size_t>(count, samples.size()));
    std::map<std::uint32_t, unsigned long long> newest;
    for (const NVML::nvmlProcessUtilizationSample_t& sample : samples)
    {
        result.newestTimeStamp = std::max(result.newestTimeStamp, sample.timeStamp);
        auto [it, inserted] = newest.try_emplace(sample.pid, sample.timeStamp);
        if (!inserted && sample.timeStamp < it->second)
        {
            continue;
        }
        it->second = sample.timeStamp;
        result.byPid[sample.pid] = ProcessShare{.smPercent = std::min(sample.smUtil, 100U),
                                                .encoderPercent = std::min(sample.encUtil, 100U),
                                                .decoderPercent = std::min(sample.decUtil, 100U)};
    }
    result.ok = true;
    return result;
}

} // namespace Platform::NVMLProcessUtilization
