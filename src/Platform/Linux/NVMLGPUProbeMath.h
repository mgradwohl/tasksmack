#pragma once

// Pure, injectable-function-pointer logic extracted from NVMLGPUProbe.cpp so it can be
// unit-tested directly, without going through dlopen()/the NVML mock library. See
// CONTRIBUTING.md's "extract the pure decision logic into a small header" pattern.

#include "Platform/Linux/PciRuntimePm.h"
#include "Platform/NVMLRunningProcesses.h"
#include "Platform/NVMLTypes.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

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

// Trailing-return spelling: clang-format 22 and 23 disagree on the space in `const char* (*)(...)` (#916).
using StatusStringFn = auto (*)(Platform::NVML::nvmlReturn_t) -> const char*;

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

/// The fields of a sysfs PCI address ("0000:01:00.0": domain, bus, device and function, in hex).
struct PciAddressFields
{
    std::uint32_t domain = 0;
    std::uint32_t bus = 0;
    std::uint32_t device = 0;
    std::uint32_t function = 0;
};

/// Parses a sysfs PCI address ("0000:01:00.0", or NVML's eight-digit-domain "00000000:01:00.0").
/// Nullopt unless it is "domain:bus:device.function" in hex. Lets a runtime-suspended GPU be
/// described from its sysfs name alone, without asking NVML (#1270).
[[nodiscard]] inline std::optional<PciAddressFields> parsePciAddress(std::string_view address)
{
    const auto firstColon = address.find(':');
    if (firstColon == std::string_view::npos)
    {
        return std::nullopt;
    }
    const auto secondColon = address.find(':', firstColon + 1);
    const auto dot = address.rfind('.');
    if (secondColon == std::string_view::npos || dot == std::string_view::npos || dot < secondColon)
    {
        return std::nullopt;
    }
    const auto parseHex = [](std::string_view digits, std::uint32_t& value)
    {
        const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), value, 16);
        return !digits.empty() && error == std::errc{} && end == digits.data() + digits.size();
    };
    PciAddressFields fields;
    if (!parseHex(address.substr(0, firstColon), fields.domain) ||
        !parseHex(address.substr(firstColon + 1, secondColon - firstColon - 1), fields.bus) ||
        !parseHex(address.substr(secondColon + 1, dot - secondColon - 1), fields.device) ||
        !parseHex(address.substr(dot + 1), fields.function))
    {
        return std::nullopt;
    }
    return fields;
}

/// What the nvidia kernel driver reports for a GPU in /proc/driver/nvidia/gpus/<address>/information
/// (#1270). The driver prints these from what it cached when it probed the device -- the model name
/// from the PCI ids, the UUID only once the GPU has been initialised -- so reading the file never
/// wakes a runtime-suspended GPU, unlike NVML's handle, UUID and PCI-info queries.
struct NvidiaProcGpuInfo
{
    std::string model; // empty when not reported
    std::string uuid;  // empty when not reported, or not known yet
};

/// Parses the "information" file's "Model:" and "GPU UUID:" lines. A UUID the driver doesn't know
/// yet (printed with '?' placeholders, "GPU-????????-...") is treated as absent.
[[nodiscard]] inline NvidiaProcGpuInfo parseNvidiaProcGpuInformation(std::string_view text)
{
    const auto trim = [](std::string_view value)
    {
        constexpr std::string_view WHITESPACE = " \t\r\n";
        const auto first = value.find_first_not_of(WHITESPACE);
        if (first == std::string_view::npos)
        {
            return std::string_view{};
        }
        return value.substr(first, value.find_last_not_of(WHITESPACE) - first + 1);
    };
    NvidiaProcGpuInfo info;
    while (!text.empty())
    {
        const auto newline = text.find('\n');
        const auto line = text.substr(0, newline);
        text = (newline == std::string_view::npos) ? std::string_view{} : text.substr(newline + 1);
        const auto colon = line.find(':');
        if (colon == std::string_view::npos)
        {
            continue;
        }
        const auto key = trim(line.substr(0, colon));
        const auto value = trim(line.substr(colon + 1));
        if (key == "Model")
        {
            info.model = std::string(value);
        }
        else if (key == "GPU UUID" && !value.empty() && !value.contains('?'))
        {
            info.uuid = std::string(value);
        }
    }
    return info;
}

/// Whether a device rebuilt by an NVML restart takes the id remembered for its PCI address (#1270).
/// A UUID reported now -- NVML's, or the driver's procfs one for a deferred device -- is
/// authoritative: one that differs from the remembered id is a different GPU in that slot, which
/// keeps its own id (and so gets none of the old GPU's sensors or memory total). With no UUID
/// reported, a remembered UUID is reused, whether the device is deferred or awake with NVML's UUID
/// query failing, rather than an index-based id breaking its history; a deferred device keeps its
/// remembered fallback id too, as it has nothing better.
[[nodiscard]] constexpr bool keepsRememberedId(bool reportsUuid, bool hasHandle, bool rememberedIsUuid)
{
    if (reportsUuid)
    {
        return false;
    }
    return rememberedIsUuid || !hasHandle;
}

} // namespace Platform::NVMLGPUProbeMath
