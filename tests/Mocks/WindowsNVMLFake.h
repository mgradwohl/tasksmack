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
///
/// fakeLibraryFunctions() is a fake nvml.dll loader for NVMLGPUProbe(NVMLLibraryFunctions) (#1720):
/// it resolves each NVML export to the fakes below, so loadNVML() itself can be tested.

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
#include <string>
#include <string_view>
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
    unsigned int encoderPercent = 30; // nvmlDeviceGetEncoderUtilization (#1485)
    unsigned int decoderPercent = 12; // nvmlDeviceGetDecoderUtilization (#1485)
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
    // What the video-engine queries answer instead of a reading (#1485): NVML_SUCCESS reads
    // encoderPercent/decoderPercent; NVML_ERROR_NOT_SUPPORTED is a GPU without that engine, and
    // anything else (NVML_ERROR_TIMEOUT) a transient failure.
    nvmlReturn_t encoderResult = NVML_SUCCESS;
    nvmlReturn_t decoderResult = NVML_SUCCESS;
};

struct FakeNvmlState
{
    nvmlReturn_t deviceCountResult = NVML_SUCCESS;
    unsigned int deviceCount = 0;
    std::unordered_set<unsigned int> invalidHandleIndices;
    std::unordered_map<unsigned int, FakeDeviceData> devices;
    int shutdownCallCount = 0;
    // NVML calls addressed to each device, so a test can prove a sleeping GPU wasn't touched (#1265)
    std::unordered_map<unsigned int, int> deviceQueries;
    // nvmlInit's answer and how often it was called, for restart() (#1294)
    nvmlReturn_t initResult = NVML_SUCCESS;
    int initCallCount = 0;
    // nvmlSystemGetDriverVersion's answer (#1720)
    nvmlReturn_t driverVersionResult = NVML_SUCCESS;
    // Devices whose readings (memory, sensors, VBIOS) report
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

/// A video engine's utilization (#1485), as NVML reports it: the percentage and the period it was
/// averaged over.
inline nvmlReturn_t fakeEngineUtilization(nvmlDevice_t device, bool encoder, unsigned int* utilization, unsigned int* samplingPeriodUs)
{
    const auto& d = touchDevice(device);
    if (isLost(device))
    {
        return NVML_ERROR_GPU_IS_LOST;
    }
    const nvmlReturn_t result = encoder ? d.encoderResult : d.decoderResult;
    if (result != NVML_SUCCESS)
    {
        return result;
    }
    constexpr unsigned int SAMPLING_PERIOD_US = 167'000;
    *utilization = encoder ? d.encoderPercent : d.decoderPercent;
    *samplingPeriodUs = SAMPLING_PERIOD_US;
    return NVML_SUCCESS;
}

inline nvmlReturn_t fakeDeviceGetEncoderUtilization(nvmlDevice_t device, unsigned int* utilization, unsigned int* samplingPeriodUs)
{
    return fakeEngineUtilization(device, true, utilization, samplingPeriodUs);
}

inline nvmlReturn_t fakeDeviceGetDecoderUtilization(nvmlDevice_t device, unsigned int* utilization, unsigned int* samplingPeriodUs)
{
    return fakeEngineUtilization(device, false, utilization, samplingPeriodUs);
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
    if (fakeState().driverVersionResult != NVML_SUCCESS)
    {
        return fakeState().driverVersionResult;
    }
    copyToBuffer(buf, size, "999.99");
    return NVML_SUCCESS;
}

inline nvmlReturn_t fakeDeviceGetMaxClockInfo(nvmlDevice_t device, int clockType, unsigned int* mhz)
{
    return fakeDeviceGetClockInfo(device, clockType, mhz);
}

inline nvmlReturn_t fakeDeviceGetPcieThroughput(nvmlDevice_t /*device*/, int /*counter*/, unsigned int* /*value*/)
{
    return NVML_ERROR_NOT_SUPPORTED;
}

// ---- Fake nvml.dll loader (#1720) ----------------------------------------

/// What the fake loader (fakeLibraryFunctions()) finds, and what it was asked.
struct FakeLibraryState
{
    bool present = true;                            ///< Whether loadLibrary finds nvml.dll
    std::unordered_set<std::string> missingExports; ///< Exports getProcAddress doesn't find
    std::vector<std::string> lookups;               ///< Every export asked for, in order
    int loadCount = 0;
    int freeCount = 0;
    void* lastFreed = nullptr;
};

inline FakeLibraryState& fakeLibrary()
{
    static FakeLibraryState state;
    return state;
}

/// The fake module handle: never dereferenced, only compared.
inline void* fakeModule()
{
    static int module = 0;
    return &module;
}

/// A fake as the untyped procedure GetProcAddress returns.
template<typename Fn> NVMLLibraryFunctions::Proc asProc(Fn fn)
{
    return reinterpret_cast<NVMLLibraryFunctions::Proc>(fn); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast) - as GetProcAddress does
}

/// The fake each NVML export resolves to.
inline NVMLLibraryFunctions::Proc fakeExport(std::string_view name)
{
    static const std::unordered_map<std::string_view, NVMLLibraryFunctions::Proc> exports = {
        {"nvmlInit", asProc(&fakeInit)},
        {"nvmlShutdown", asProc(&fakeShutdown)},
        {"nvmlDeviceGetCount", asProc(&fakeDeviceGetCount)},
        {"nvmlDeviceGetHandleByIndex", asProc(&fakeDeviceGetHandleByIndex)},
        {"nvmlDeviceGetName", asProc(&fakeDeviceGetName)},
        {"nvmlDeviceGetUUID", asProc(&fakeDeviceGetUUID)},
        {"nvmlDeviceGetMemoryInfo", asProc(&fakeDeviceGetMemoryInfo)},
        {"nvmlDeviceGetTemperature", asProc(&fakeDeviceGetTemperature)},
        {"nvmlDeviceGetPowerUsage", asProc(&fakeDeviceGetPowerUsage)},
        {"nvmlDeviceGetPowerManagementLimit", asProc(&fakeDeviceGetPowerManagementLimit)},
        {"nvmlDeviceGetClockInfo", asProc(&fakeDeviceGetClockInfo)},
        {"nvmlDeviceGetMaxClockInfo", asProc(&fakeDeviceGetMaxClockInfo)},
        {"nvmlDeviceGetUtilizationRates", asProc(&fakeDeviceGetUtilizationRates)},
        {"nvmlSystemGetDriverVersion", asProc(&fakeSystemGetDriverVersion)},
        {"nvmlDeviceGetVbiosVersion", asProc(&fakeDeviceGetVbiosVersion)},
        {"nvmlDeviceGetFanSpeed", asProc(&fakeDeviceGetFanSpeed)},
        {"nvmlDeviceGetPcieThroughput", asProc(&fakeDeviceGetPcieThroughput)},
        {"nvmlDeviceGetEncoderUtilization", asProc(&fakeDeviceGetEncoderUtilization)},
        {"nvmlDeviceGetDecoderUtilization", asProc(&fakeDeviceGetDecoderUtilization)},
        {"nvmlDeviceGetPciInfo_v3", asProc(&fakeDeviceGetPciInfo)},
        {"nvmlDeviceGetPciInfo_v2", asProc(&fakeDeviceGetPciInfo)},
    };
    const auto found = exports.find(name);
    return found == exports.end() ? nullptr : found->second;
}

inline void* fakeLoadLibrary()
{
    ++fakeLibrary().loadCount;
    return fakeLibrary().present ? fakeModule() : nullptr;
}

inline NVMLLibraryFunctions::Proc fakeGetProcAddress(void* module, const char* name)
{
    fakeLibrary().lookups.emplace_back(name);
    if (module != fakeModule() || fakeLibrary().missingExports.contains(name))
    {
        return nullptr;
    }
    return fakeExport(name);
}

inline void fakeFreeLibrary(void* module)
{
    ++fakeLibrary().freeCount;
    fakeLibrary().lastFreed = module;
}

/// A loader whose nvml.dll is the fakes above, as fakeLibrary() configures it.
inline NVMLLibraryFunctions fakeLibraryFunctions()
{
    return {.loadLibrary = fakeLoadLibrary, .getProcAddress = fakeGetProcAddress, .freeLibrary = fakeFreeLibrary};
}

} // namespace NVMLFake

// Test-only accessor: lets unit tests inject a fake NVMLFunctions table and device handles
// so enumerateGPUs()/readGPUCounters()/capabilities() can be exercised deterministically without a real NVIDIA GPU or nvml.dll. Declared
// directly in namespace Platform (not inside an anonymous namespace) so the `friend struct NVMLGPUProbeTestAccessor;` declaration in
// NVMLGPUProbe.h resolves to this exact type; its members name the fakes above through NVMLFake. inject() below substitutes the backend
// after the constructor's real loadNVML() has already run; this does not change or bypass loadNVML()'s LOAD_LIBRARY_SEARCH_SYSTEM32
// hardening in any way.
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
    /// out individual fields (e.g. the video-engine queries) to exercise "not available" branches.
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
        fns.DeviceGetEncoderUtilization = NVMLFake::fakeDeviceGetEncoderUtilization;
        fns.DeviceGetDecoderUtilization = NVMLFake::fakeDeviceGetDecoderUtilization;
        return fns;
    }
};

} // namespace Platform

#endif // _WIN32
