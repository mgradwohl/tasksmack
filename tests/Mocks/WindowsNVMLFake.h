/// @file WindowsNVMLFake.h
/// @brief A fake NVML backend for the Windows NVMLGPUProbe, and the friend accessor that installs it.
///
/// loadNVML() deliberately restricts nvml.dll's search path to LOAD_LIBRARY_SEARCH_SYSTEM32
/// (security hardening so a portable installation cannot load an adjacent DLL), so - unlike
/// Linux's dlopen-based NVML probe - a fake DLL placed elsewhere cannot be picked up. Instead,
/// NVMLGPUProbeTestAccessor (a friend of NVMLGPUProbe, see NVMLGPUProbe.h) lets tests substitute
/// a fake NVMLFunctions table and device handles after construction. Shared by
/// test_WindowsNVMLGPUProbe.cpp and test_WindowsGPURescan.cpp, so everything here is inline: the
/// accessor must be one type across the test binary.

#pragma once

#ifdef _WIN32

#include "Platform/GPUTypes.h"
#include "Platform/NVMLTypes.h"
#include "Platform/Windows/NVMLGPUProbe.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iterator>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Platform
{
namespace NVMLFake
{

using namespace Platform::NVML; // NOLINT(google-build-using-namespace) - test fakes mirror the C API

// ---- Fake NVML backend --------------------------------------------------

struct FakeDeviceData
{
    // Value fields grouped before the bools below to avoid padding between them.
    std::uint64_t memUsed = 2ULL * 1024 * 1024 * 1024;
    std::uint64_t memTotal = 24ULL * 1024 * 1024 * 1024;
    std::string name = "NVIDIA GeForce RTX 4090";
    std::string uuid = "GPU-11111111-1111-1111-1111-111111111111";
    std::string vbios = "95.02.18.00.01";
    unsigned int temperatureC = 63;
    unsigned int powerMilliwatts = 180000;
    unsigned int powerLimitMilliwatts = 450000;
    unsigned int gpuClockMhz = 2100;
    unsigned int memClockMhz = 10500;
    unsigned int utilizationGpu = 37;
    unsigned int fanPercent = 48;
    unsigned int pciBus = 0x01;
    unsigned int pciDevice = 0x00;
    unsigned int pciDeviceId = 0x268410DEU; // (device ID << 16) | vendor ID, as NVML encodes it
    std::string pciBusId;                   // nvmlPciInfo_t::busId ("00000000:01:00.0"); empty reports none
    bool nameOk = true;
    bool uuidOk = true;
    bool vbiosOk = true;
    bool memoryOk = true;
    bool temperatureOk = true;
    bool powerOk = true;
    bool powerLimitOk = true;
    bool gpuClockOk = true;
    bool memClockOk = true;
    bool utilizationOk = true;
    bool fanOk = true;
    bool pciInfoOk = true;
    // What a failed UUID or PCI read (uuidOk/pciInfoOk false) returns: unsupported by default, or a
    // reset (NVML_ERROR_GPU_IS_LOST) mid-identity-read.
    nvmlReturn_t uuidFailure = NVML_ERROR_NOT_SUPPORTED;
    nvmlReturn_t pciInfoFailure = NVML_ERROR_NOT_SUPPORTED;
};

/// One running process as the fake reports it. The entry points write it in the layout of the
/// variant called: 16-byte nvmlProcessInfo_v1_t {pid, usedGpuMemory} from the unversioned export,
/// 24-byte nvmlProcessInfo_v2_t (adding the instance ids) from _v3 (#1313).
struct FakeProcess
{
    unsigned int pid = 0;
    std::uint64_t usedGpuMemory = 0;
    unsigned int gpuInstanceId = 0;
    unsigned int computeInstanceId = 0;
};

inline constexpr std::size_t FAKE_PROCESS_INFO_V1_SIZE = 16;
inline constexpr std::size_t FAKE_PROCESS_INFO_V2_SIZE = 24;

struct FakeProcessQuery
{
    std::vector<FakeProcess> processes;
    nvmlReturn_t firstCallResult = NVML_SUCCESS;
    nvmlReturn_t secondCallResult = NVML_SUCCESS;
    // When set, the "query count" call reports this instead of processes.size() - lets a
    // test simulate an implausible driver-reported count independent of the real list size.
    std::optional<unsigned int> reportedCountOverride;
};

struct FakeNvmlState
{
    nvmlReturn_t deviceCountResult = NVML_SUCCESS;
    unsigned int deviceCount = 0;
    std::unordered_set<unsigned int> invalidHandleIndices;
    std::unordered_map<unsigned int, FakeDeviceData> devices;
    std::unordered_map<unsigned int, FakeProcessQuery> computeProcesses;
    std::unordered_map<unsigned int, FakeProcessQuery> graphicsProcesses;
    int shutdownCallCount = 0;
    // NVML calls addressed to each device, so a test can prove a sleeping GPU wasn't touched (#1265)
    std::unordered_map<unsigned int, int> deviceQueries;
    // nvmlInit's answer and how often it was called, for restart() (#1294)
    nvmlReturn_t initResult = NVML_SUCCESS;
    int initCallCount = 0;
    // Devices whose readings (memory, sensors, VBIOS, running processes) report
    // NVML_ERROR_GPU_IS_LOST (a driver reset, say) (#1294); identity reads still answer
    std::unordered_set<unsigned int> lostDevices;
};

inline FakeNvmlState& fakeState()
{
    static FakeNvmlState state;
    return state;
}

inline FakeDeviceData& deviceData(unsigned int index)
{
    return fakeState().devices[index];
}

inline unsigned int deviceIndexOf(nvmlDevice_t device)
{
    return static_cast<unsigned int>(reinterpret_cast<std::uintptr_t>(device) - 1);
}

/// The fake device a call is for, counting the call against it (#1265).
inline const FakeDeviceData& touchDevice(nvmlDevice_t device)
{
    const unsigned int index = deviceIndexOf(device);
    ++fakeState().deviceQueries[index];
    return fakeState().devices.at(index);
}

/// Whether the device a call is for is in lostDevices, so the call reports NVML_ERROR_GPU_IS_LOST.
inline bool isLost(nvmlDevice_t device)
{
    return fakeState().lostDevices.contains(deviceIndexOf(device));
}

inline nvmlDevice_t deviceHandleFor(unsigned int index)
{
    // nvmlDevice_t is an opaque handle; encoding the index as a small sentinel pointer (never
    // dereferenced) is the simplest way for the fakes below to recover which device a call is for.
    return reinterpret_cast<nvmlDevice_t>(static_cast<std::uintptr_t>(index) + 1); // NOLINT(performance-no-int-to-ptr)
}

inline FakeProcessQuery makeProcessQuery(std::vector<FakeProcess> processes,
                                         nvmlReturn_t firstCallResult = NVML_SUCCESS,
                                         nvmlReturn_t secondCallResult = NVML_SUCCESS,
                                         std::optional<unsigned int> reportedCountOverride = std::nullopt)
{
    FakeProcessQuery query;
    query.processes = std::move(processes);
    query.firstCallResult = firstCallResult;
    query.secondCallResult = secondCallResult;
    query.reportedCountOverride = reportedCountOverride;
    return query;
}

inline void copyToBuffer(char* dst, unsigned int size, const std::string& s)
{
    if (size == 0)
    {
        return;
    }
    const auto n = std::min<std::size_t>(static_cast<std::size_t>(size - 1), s.size());
    std::memcpy(dst, s.data(), n);
    dst[n] = '\0';
}

inline nvmlReturn_t fakeDeviceGetCount(unsigned int* count)
{
    *count = fakeState().deviceCount;
    return fakeState().deviceCountResult;
}

inline nvmlReturn_t fakeDeviceGetHandleByIndex(unsigned int index, nvmlDevice_t* device)
{
    if (fakeState().invalidHandleIndices.contains(index))
    {
        return NVML_ERROR_NOT_FOUND;
    }
    *device = deviceHandleFor(index);
    return NVML_SUCCESS;
}

inline nvmlReturn_t fakeDeviceGetName(nvmlDevice_t device, char* buf, unsigned int size)
{
    const auto& d = touchDevice(device);
    if (!d.nameOk)
    {
        return NVML_ERROR_NOT_SUPPORTED;
    }
    copyToBuffer(buf, size, d.name);
    return NVML_SUCCESS;
}

inline nvmlReturn_t fakeDeviceGetUUID(nvmlDevice_t device, char* buf, unsigned int size)
{
    const auto& d = touchDevice(device);
    if (!d.uuidOk)
    {
        return d.uuidFailure;
    }
    copyToBuffer(buf, size, d.uuid);
    return NVML_SUCCESS;
}

inline nvmlReturn_t fakeDeviceGetVbiosVersion(nvmlDevice_t device, char* buf, unsigned int size)
{
    const auto& d = touchDevice(device);
    if (isLost(device))
    {
        return NVML_ERROR_GPU_IS_LOST;
    }
    if (!d.vbiosOk)
    {
        return NVML_ERROR_NOT_SUPPORTED;
    }
    copyToBuffer(buf, size, d.vbios);
    return NVML_SUCCESS;
}

inline nvmlReturn_t fakeDeviceGetMemoryInfo(nvmlDevice_t device, void* memInfoRaw)
{
    const auto& d = touchDevice(device);
    if (isLost(device))
    {
        return NVML_ERROR_GPU_IS_LOST;
    }
    if (!d.memoryOk)
    {
        return NVML_ERROR_NOT_SUPPORTED;
    }
    auto* memInfo = static_cast<nvmlMemory_t*>(memInfoRaw);
    memInfo->used = d.memUsed;
    memInfo->total = d.memTotal;
    memInfo->free = d.memTotal - d.memUsed;
    return NVML_SUCCESS;
}

inline nvmlReturn_t fakeDeviceGetTemperature(nvmlDevice_t device, int /*sensor*/, unsigned int* temp)
{
    const auto& d = touchDevice(device);
    if (isLost(device))
    {
        return NVML_ERROR_GPU_IS_LOST;
    }
    if (!d.temperatureOk)
    {
        return NVML_ERROR_NOT_SUPPORTED;
    }
    *temp = d.temperatureC;
    return NVML_SUCCESS;
}

inline nvmlReturn_t fakeDeviceGetPowerUsage(nvmlDevice_t device, unsigned int* mw)
{
    const auto& d = touchDevice(device);
    if (isLost(device))
    {
        return NVML_ERROR_GPU_IS_LOST;
    }
    if (!d.powerOk)
    {
        return NVML_ERROR_NOT_SUPPORTED;
    }
    *mw = d.powerMilliwatts;
    return NVML_SUCCESS;
}

inline nvmlReturn_t fakeDeviceGetPowerManagementLimit(nvmlDevice_t device, unsigned int* mw)
{
    const auto& d = touchDevice(device);
    if (isLost(device))
    {
        return NVML_ERROR_GPU_IS_LOST;
    }
    if (!d.powerLimitOk)
    {
        return NVML_ERROR_NOT_SUPPORTED;
    }
    *mw = d.powerLimitMilliwatts;
    return NVML_SUCCESS;
}

inline nvmlReturn_t fakeDeviceGetClockInfo(nvmlDevice_t device, int clockType, unsigned int* mhz)
{
    const auto& d = touchDevice(device);
    if (isLost(device))
    {
        return NVML_ERROR_GPU_IS_LOST;
    }
    if (clockType == static_cast<int>(NVML_CLOCK_GRAPHICS))
    {
        if (!d.gpuClockOk)
        {
            return NVML_ERROR_NOT_SUPPORTED;
        }
        *mhz = d.gpuClockMhz;
        return NVML_SUCCESS;
    }
    if (clockType == static_cast<int>(NVML_CLOCK_MEM))
    {
        if (!d.memClockOk)
        {
            return NVML_ERROR_NOT_SUPPORTED;
        }
        *mhz = d.memClockMhz;
        return NVML_SUCCESS;
    }
    return NVML_ERROR_INVALID_ARGUMENT;
}

inline nvmlReturn_t fakeDeviceGetUtilizationRates(nvmlDevice_t device, void* utilRaw)
{
    const auto& d = touchDevice(device);
    if (isLost(device))
    {
        return NVML_ERROR_GPU_IS_LOST;
    }
    if (!d.utilizationOk)
    {
        return NVML_ERROR_NOT_SUPPORTED;
    }
    auto* util = static_cast<nvmlUtilization_t*>(utilRaw);
    util->gpu = d.utilizationGpu;
    util->memory = 0;
    return NVML_SUCCESS;
}

inline nvmlReturn_t fakeDeviceGetFanSpeed(nvmlDevice_t device, unsigned int* speed)
{
    const auto& d = touchDevice(device);
    if (isLost(device))
    {
        return NVML_ERROR_GPU_IS_LOST;
    }
    if (!d.fanOk)
    {
        return NVML_ERROR_NOT_SUPPORTED;
    }
    *speed = d.fanPercent;
    return NVML_SUCCESS;
}

inline nvmlReturn_t fakeDeviceGetPciInfo(nvmlDevice_t device, nvmlPciInfo_t* pci)
{
    const auto& d = touchDevice(device);
    if (!d.pciInfoOk)
    {
        return d.pciInfoFailure;
    }
    *pci = nvmlPciInfo_t{};
    pci->bus = d.pciBus;
    pci->device = d.pciDevice;
    pci->pciDeviceId = d.pciDeviceId;
    std::copy_n(d.pciBusId.data(), std::min(d.pciBusId.size(), std::size(pci->busId) - 1), std::data(pci->busId));
    return NVML_SUCCESS;
}

/// Answers a running-process query like NVML does, writing `entrySize`-byte entries: the layout is
/// spelled out byte by byte (pid at 0, usedGpuMemory at 8, then the instance ids at 16 and 20 in
/// the 24-byte layout), independently of the parser under test.
inline nvmlReturn_t queryFakeProcesses(std::unordered_map<unsigned int, FakeProcessQuery>& table,
                                       unsigned int deviceIndex,
                                       unsigned int* count,
                                       nvmlProcessInfoEntries* buffer,
                                       std::size_t entrySize)
{
    ++fakeState().deviceQueries[deviceIndex];
    if (fakeState().lostDevices.contains(deviceIndex))
    {
        return NVML_ERROR_GPU_IS_LOST;
    }
    auto it = table.find(deviceIndex);
    if (it == table.end())
    {
        *count = 0;
        return NVML_SUCCESS;
    }

    const auto& query = it->second;
    const unsigned int reportedCount = query.reportedCountOverride.value_or(static_cast<unsigned int>(query.processes.size()));

    if (buffer == nullptr)
    {
        *count = reportedCount;
        return query.firstCallResult;
    }

    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) - NVML's untyped entry array
    auto* out = reinterpret_cast<std::byte*>(buffer);
    const unsigned int toCopy = std::min(*count, static_cast<unsigned int>(query.processes.size()));
    for (unsigned int i = 0; i < toCopy; ++i)
    {
        const FakeProcess& process = query.processes[i];
        std::byte* entry = out + (static_cast<std::size_t>(i) * entrySize);
        std::memcpy(entry, &process.pid, sizeof(process.pid));
        std::memcpy(entry + 8, &process.usedGpuMemory, sizeof(process.usedGpuMemory));
        if (entrySize == FAKE_PROCESS_INFO_V2_SIZE)
        {
            std::memcpy(entry + 16, &process.gpuInstanceId, sizeof(process.gpuInstanceId));
            std::memcpy(entry + 20, &process.computeInstanceId, sizeof(process.computeInstanceId));
        }
    }
    *count = toCopy;
    return query.secondCallResult;
}

// The running-process exports: the unversioned (v1) symbols write 16-byte entries, the _v3 ones
// 24-byte entries (#1313). Each takes nvmlProcessInfoEntries* like the probe's function-pointer
// type, so the call matches the callee's own type (UBSan -fsanitize=function, #1306).
inline nvmlReturn_t fakeDeviceGetComputeRunningProcesses(nvmlDevice_t device, unsigned int* count, nvmlProcessInfoEntries* buffer)
{
    return queryFakeProcesses(fakeState().computeProcesses, deviceIndexOf(device), count, buffer, FAKE_PROCESS_INFO_V1_SIZE);
}

inline nvmlReturn_t fakeDeviceGetGraphicsRunningProcesses(nvmlDevice_t device, unsigned int* count, nvmlProcessInfoEntries* buffer)
{
    return queryFakeProcesses(fakeState().graphicsProcesses, deviceIndexOf(device), count, buffer, FAKE_PROCESS_INFO_V1_SIZE);
}

// NOLINTNEXTLINE(readability-identifier-naming) - mirrors NVML's _v3 export name
inline nvmlReturn_t fakeDeviceGetComputeRunningProcesses_v3(nvmlDevice_t device, unsigned int* count, nvmlProcessInfoEntries* buffer)
{
    return queryFakeProcesses(fakeState().computeProcesses, deviceIndexOf(device), count, buffer, FAKE_PROCESS_INFO_V2_SIZE);
}

// NOLINTNEXTLINE(readability-identifier-naming) - mirrors NVML's _v3 export name
inline nvmlReturn_t fakeDeviceGetGraphicsRunningProcesses_v3(nvmlDevice_t device, unsigned int* count, nvmlProcessInfoEntries* buffer)
{
    return queryFakeProcesses(fakeState().graphicsProcesses, deviceIndexOf(device), count, buffer, FAKE_PROCESS_INFO_V2_SIZE);
}

/// The address of a fake export, as GetProcAddress would hand it to the loader.
template<typename Fn> void* exportAddress(Fn* fn)
{
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) - as GetProcAddress does
    return reinterpret_cast<void*>(fn);
}

/// The running-process symbols a fake nvml.dll exports, by name, for the probe's loader (#1313).
inline std::unordered_map<std::string, void*> runningProcessExports(bool withV1, bool withV3)
{
    std::unordered_map<std::string, void*> exports;
    if (withV1)
    {
        exports["nvmlDeviceGetComputeRunningProcesses"] = exportAddress(&fakeDeviceGetComputeRunningProcesses);
        exports["nvmlDeviceGetGraphicsRunningProcesses"] = exportAddress(&fakeDeviceGetGraphicsRunningProcesses);
    }
    if (withV3)
    {
        exports["nvmlDeviceGetComputeRunningProcesses_v3"] = exportAddress(&fakeDeviceGetComputeRunningProcesses_v3);
        exports["nvmlDeviceGetGraphicsRunningProcesses_v3"] = exportAddress(&fakeDeviceGetGraphicsRunningProcesses_v3);
    }
    return exports;
}

inline nvmlReturn_t fakeShutdown()
{
    ++fakeState().shutdownCallCount;
    return NVML_SUCCESS;
}

inline nvmlReturn_t fakeInit()
{
    ++fakeState().initCallCount;
    return fakeState().initResult;
}

inline nvmlReturn_t fakeSystemGetDriverVersion(char* buf, unsigned int size)
{
    copyToBuffer(buf, size, "999.99");
    return NVML_SUCCESS;
}

} // namespace NVMLFake

// Test-only accessor: lets unit tests inject a fake NVMLFunctions table and device handles
// so enumerateGPUs()/readGPUCounters()/readProcessGPUCounters()/capabilities() can be
// exercised deterministically without a real NVIDIA GPU or nvml.dll. Declared directly in
// namespace Platform (not inside an anonymous namespace) so the `friend struct
// NVMLGPUProbeTestAccessor;` declaration in NVMLGPUProbe.h resolves to this exact type; its
// members name the fakes above through NVMLFake. inject() below substitutes the backend after the
// constructor's real loadNVML() has already run; this does not change or bypass
// loadNVML()'s LOAD_LIBRARY_SEARCH_SYSTEM32 hardening in any way.
struct NVMLGPUProbeTestAccessor
{
    static void inject(NVMLGPUProbe& probe, const NVMLGPUProbe::NVMLFunctions& fns, bool initialized)
    {
        // The constructor already ran the real loadNVML()/initializeNVML() against whatever
        // NVML is actually present on this machine. On a machine with a real NVIDIA driver
        // installed, that leaves a real nvmlInit() outstanding and a real DLL handle open;
        // tear both down properly (matching real nvmlShutdown() to the real nvmlInit(), and
        // freeing the real DLL) before substituting the fake backend below, so this doesn't
        // leak an outstanding initialization or an unmatched DLL reference count.
        probe.shutdownNVML();
        probe.unloadNVML();

        probe.m_NVML = fns;
        probe.m_Initialized = initialized;
        // Every fake GPU is awake unless a test says otherwise: the real PnP query would look at
        // whatever adapter this machine has at the fake's PCI location (#1265).
        probe.m_IsAsleep = [](const PciLocation&)
        {
            return false;
        };
    }

    static void setAsleep(NVMLGPUProbe& probe, std::function<bool(const PciLocation&)> isAsleep)
    {
        probe.m_IsAsleep = std::move(isAsleep);
    }

    static void addDevice(NVMLGPUProbe& probe, uint32_t index, NVML::nvmlDevice_t handle)
    {
        probe.m_DeviceHandles[index] = handle;
    }

    [[nodiscard]] static std::string errorString(NVML::nvmlReturn_t result)
    {
        return NVMLGPUProbe::getNVMLErrorString(result);
    }

    /// Builds a fully-populated NVMLFunctions table pointing at the fakes above. Tests null
    /// out individual fields (e.g. per-process functions) to exercise "not available" branches.
    [[nodiscard]] static NVMLGPUProbe::NVMLFunctions fullFakeFunctions()
    {
        NVMLGPUProbe::NVMLFunctions fns{};
        fns.Init = NVMLFake::fakeInit;
        fns.SystemGetDriverVersion = NVMLFake::fakeSystemGetDriverVersion;
        fns.Shutdown = NVMLFake::fakeShutdown;
        fns.DeviceGetCount = NVMLFake::fakeDeviceGetCount;
        fns.DeviceGetHandleByIndex = NVMLFake::fakeDeviceGetHandleByIndex;
        fns.DeviceGetName = NVMLFake::fakeDeviceGetName;
        fns.DeviceGetUUID = NVMLFake::fakeDeviceGetUUID;
        fns.DeviceGetMemoryInfo = NVMLFake::fakeDeviceGetMemoryInfo;
        fns.DeviceGetTemperature = NVMLFake::fakeDeviceGetTemperature;
        fns.DeviceGetPowerUsage = NVMLFake::fakeDeviceGetPowerUsage;
        fns.DeviceGetPowerManagementLimit = NVMLFake::fakeDeviceGetPowerManagementLimit;
        fns.DeviceGetClockInfo = NVMLFake::fakeDeviceGetClockInfo;
        fns.DeviceGetUtilizationRates = NVMLFake::fakeDeviceGetUtilizationRates;
        fns.DeviceGetVbiosVersion = NVMLFake::fakeDeviceGetVbiosVersion;
        fns.DeviceGetFanSpeed = NVMLFake::fakeDeviceGetFanSpeed;
        fns.DeviceGetPciInfo = NVMLFake::fakeDeviceGetPciInfo;
        // What the loader picks from a current driver: the _v3 exports and their 24-byte entries.
        fns.DeviceGetComputeRunningProcesses = {.fn = NVMLFake::fakeDeviceGetComputeRunningProcesses_v3,
                                                .entrySize = NVMLFake::FAKE_PROCESS_INFO_V2_SIZE};
        fns.DeviceGetGraphicsRunningProcesses = {.fn = NVMLFake::fakeDeviceGetGraphicsRunningProcesses_v3,
                                                 .entrySize = NVMLFake::FAKE_PROCESS_INFO_V2_SIZE};
        return fns;
    }

    /// Loads the running-process entry points as loadNVML() does, but from `exports` rather than
    /// nvml.dll (#1313): the newest variant exported, with its entry size.
    static void loadRunningProcesses(NVMLGPUProbe& probe, const std::unordered_map<std::string, void*>& exports)
    {
        const auto resolve = [&exports](const std::string& name) -> void*
        {
            const auto it = exports.find(name);
            return it == exports.end() ? nullptr : it->second;
        };
        probe.m_NVML.DeviceGetComputeRunningProcesses =
            NVMLGPUProbe::loadRunningProcessesQuery("nvmlDeviceGetComputeRunningProcesses", resolve);
        probe.m_NVML.DeviceGetGraphicsRunningProcesses =
            NVMLGPUProbe::loadRunningProcessesQuery("nvmlDeviceGetGraphicsRunningProcesses", resolve);
    }

    /// The entry size of the loaded compute running-process entry point; 0 when none is loaded.
    [[nodiscard]] static std::size_t computeEntrySize(const NVMLGPUProbe& probe)
    {
        return probe.m_NVML.DeviceGetComputeRunningProcesses.entrySize;
    }
};

} // namespace Platform

#endif // _WIN32
