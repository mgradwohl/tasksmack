#include "ROCmGPUProbe.h"

#include "PciDisplayDevices.h"
#include "PciRuntimePm.h"
#include "Platform/GPUTypes.h"
#include "ROCmGPUProbeMath.h"

#include <spdlog/spdlog.h>

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include <dlfcn.h>

// ROCm SMI types and constants (for dynamic loading without requiring rocm_smi.h)
// These definitions match the ROCm SMI API but don't require ROCm installation
namespace
{

// NOLINTBEGIN(readability-identifier-naming) - these types mirror AMD ROCm SMI C API naming
using rsmi_device_t = std::uint32_t;
using rsmi_status_t = std::uint32_t;
// NOLINTEND(readability-identifier-naming)

// ROCm SMI return codes
constexpr rsmi_status_t RSMI_STATUS_SUCCESS = 0;
[[maybe_unused]] constexpr rsmi_status_t RSMI_STATUS_INVALID_ARGS = 1;
constexpr rsmi_status_t RSMI_STATUS_NOT_SUPPORTED = 2;
[[maybe_unused]] constexpr rsmi_status_t RSMI_STATUS_FILE_ERROR = 3;
[[maybe_unused]] constexpr rsmi_status_t RSMI_STATUS_PERMISSION = 4;
[[maybe_unused]] constexpr rsmi_status_t RSMI_STATUS_OUT_OF_RESOURCES = 5;
[[maybe_unused]] constexpr rsmi_status_t RSMI_STATUS_INTERNAL_EXCEPTION = 6;
[[maybe_unused]] constexpr rsmi_status_t RSMI_STATUS_INPUT_OUT_OF_BOUNDS = 7;
constexpr rsmi_status_t RSMI_STATUS_INIT_ERROR = 8;
constexpr rsmi_status_t RSMI_STATUS_NOT_YET_IMPLEMENTED = 9;
constexpr rsmi_status_t RSMI_STATUS_NOT_FOUND = 10;
[[maybe_unused]] constexpr rsmi_status_t RSMI_STATUS_INSUFFICIENT_SIZE = 11;
[[maybe_unused]] constexpr rsmi_status_t RSMI_STATUS_INTERRUPT = 12;
[[maybe_unused]] constexpr rsmi_status_t RSMI_STATUS_UNEXPECTED_SIZE = 13;
[[maybe_unused]] constexpr rsmi_status_t RSMI_STATUS_NO_DATA = 14;
[[maybe_unused]] constexpr rsmi_status_t RSMI_STATUS_UNKNOWN_ERROR = 0xFFFFFFFF;

// ROCm SMI temperature types
// NOLINTBEGIN(cppcoreguidelines-use-enum-class,performance-enum-size,readability-identifier-naming) - must match AMD ROCm SMI API
enum rsmi_temperature_type_t : std::uint32_t
{
    RSMI_TEMP_TYPE_EDGE = 0,
    RSMI_TEMP_TYPE_JUNCTION = 1,
    RSMI_TEMP_TYPE_MEMORY = 2,
    RSMI_TEMP_TYPE_HBM_0 = 3,
    RSMI_TEMP_TYPE_HBM_1 = 4,
    RSMI_TEMP_TYPE_HBM_2 = 5,
    RSMI_TEMP_TYPE_HBM_3 = 6
};
// NOLINTEND(cppcoreguidelines-use-enum-class,performance-enum-size,readability-identifier-naming)

// ROCm SMI temperature metric
// NOLINTBEGIN(cppcoreguidelines-use-enum-class,performance-enum-size,readability-identifier-naming) - must match AMD ROCm SMI API
enum rsmi_temperature_metric_t : std::uint32_t
{
    RSMI_TEMP_CURRENT = 0,
    RSMI_TEMP_MAX = 1,
    RSMI_TEMP_MIN = 2,
    RSMI_TEMP_MAX_HYST = 3,
    RSMI_TEMP_MIN_HYST = 4,
    RSMI_TEMP_CRITICAL = 5,
    RSMI_TEMP_CRITICAL_HYST = 6,
    RSMI_TEMP_EMERGENCY = 7,
    RSMI_TEMP_EMERGENCY_HYST = 8,
    RSMI_TEMP_CRIT_MIN = 9,
    RSMI_TEMP_CRIT_MIN_HYST = 10,
    RSMI_TEMP_OFFSET = 11,
    RSMI_TEMP_LOWEST = 12,
    RSMI_TEMP_HIGHEST = 13
};
// NOLINTEND(cppcoreguidelines-use-enum-class,performance-enum-size,readability-identifier-naming)

// ROCm SMI clock types
// NOLINTBEGIN(cppcoreguidelines-use-enum-class,performance-enum-size,readability-identifier-naming) - must match AMD ROCm SMI API
enum rsmi_clk_type_t : std::uint32_t
{
    RSMI_CLK_TYPE_SYS = 0,
    RSMI_CLK_TYPE_DF = 1,
    RSMI_CLK_TYPE_DCEF = 2,
    RSMI_CLK_TYPE_SOC = 3,
    RSMI_CLK_TYPE_MEM = 4,
    RSMI_CLK_TYPE_FIRST = RSMI_CLK_TYPE_SYS,
    RSMI_CLK_TYPE_LAST = RSMI_CLK_TYPE_MEM
};
// NOLINTEND(cppcoreguidelines-use-enum-class,performance-enum-size,readability-identifier-naming)

// ROCm SMI memory types
// NOLINTBEGIN(cppcoreguidelines-use-enum-class,performance-enum-size,readability-identifier-naming) - must match AMD ROCm SMI API
enum rsmi_memory_type_t : std::uint32_t
{
    RSMI_MEM_TYPE_VRAM = 0,
    RSMI_MEM_TYPE_VIS_VRAM = 1,
    RSMI_MEM_TYPE_GTT = 2,
    RSMI_MEM_TYPE_FIRST = RSMI_MEM_TYPE_VRAM,
    RSMI_MEM_TYPE_LAST = RSMI_MEM_TYPE_GTT
};
// NOLINTEND(cppcoreguidelines-use-enum-class,performance-enum-size,readability-identifier-naming)

// ROCm SMI buffer size constants
constexpr std::size_t RSMI_MAX_BUFFER_LENGTH = 256;

// ROCm SMI library version
// NOLINTNEXTLINE(readability-identifier-naming) - must match AMD ROCm SMI API
struct rsmi_version_t
{
    std::uint32_t major;
    std::uint32_t minor;
    std::uint32_t patch;
    const char* build;
};

// rsmi_frequencies_t changed layout across ROCm versions, so it is only forward-declared: the
// function pointer keeps the library's real signature, and the probe passes the address of a
// ROCmGPUProbeMath::RsmiFrequenciesBuffer, which is larger than every layout (#1088).
// NOLINTNEXTLINE(readability-identifier-naming) - must match AMD ROCm SMI API
struct rsmi_frequencies_t;

[[nodiscard]] rsmi_frequencies_t* asFrequencies(Platform::ROCmGPUProbeMath::RsmiFrequenciesBuffer& buffer) noexcept
{
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) - opaque C struct filled by the library
    return reinterpret_cast<rsmi_frequencies_t*>(buffer.bytes.data());
}

/// Reads a sysfs decimal attribute ("11\n"); nullopt if it can't be read or isn't a number.
[[nodiscard]] std::optional<std::uint32_t> readDecimalAttribute(const std::filesystem::path& path)
{
    std::ifstream file(path);
    std::string text;
    if (!file.is_open() || !std::getline(file, text))
    {
        return std::nullopt;
    }
    std::string_view digits(text);
    while (!digits.empty() && (digits.back() == '\r' || digits.back() == ' '))
    {
        digits.remove_suffix(1);
    }
    std::uint32_t value = 0;
    const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), value);
    if (error != std::errc{} || end != digits.data() + digits.size() || digits.empty())
    {
        return std::nullopt;
    }
    return value;
}

/// The graphics core's IP version from amdgpu's ip_discovery sysfs tree under the PCI device
/// directory `devicePath` (#1266). The IP is listed both by name (GC) and by hardware id (11);
/// nullopt on kernels or ASICs without the tree. These are attributes amdgpu cached at probe time,
/// so reading them never wakes a runtime-suspended GPU (#1117).
[[nodiscard]] std::optional<Platform::ROCmGPUProbeMath::GcIpVersion> readGcIpVersion(const std::string& devicePath)
{
    for (const char* gcDir : {"/ip_discovery/die/0/GC/0", "/ip_discovery/die/0/11/0"})
    {
        const std::filesystem::path dir = devicePath + gcDir;
        const auto major = readDecimalAttribute(dir / "major");
        const auto minor = readDecimalAttribute(dir / "minor");
        const auto revision = readDecimalAttribute(dir / "revision");
        if (major.has_value() && minor.has_value() && revision.has_value())
        {
            return Platform::ROCmGPUProbeMath::GcIpVersion{.major = *major, .minor = *minor, .revision = *revision};
        }
    }
    return std::nullopt;
}

} // anonymous namespace

namespace Platform
{

struct ROCmGPUProbe::Impl
{
    void* rocmHandle = nullptr;
    bool initialized = false;
    std::uint32_t deviceCount = 0;
    std::vector<rsmi_device_t> devices;
    // Device ids resolved once at load, parallel to devices (#1162): re-deriving them on every
    // read let a transient lookup failure turn a GPU into a different "amd_N" id for one sample.
    std::vector<std::string> deviceIds;
    // Each device's sysfs directory, parallel to devices, whose power/runtime_status says whether it
    // is asleep (#1117); empty when ROCm SMI can't report its PCI address (it is then always read).
    std::vector<std::string> sysfsPaths;
    // The last VRAM total read while each device was awake, reported while it sleeps (#1117).
    std::vector<std::uint64_t> lastMemoryTotalBytes;
    // Each device's name, read at load, so a repeat enumerateGPUs() needn't ask again.
    std::vector<std::string> names;
    // Whether each device is an APU's integrated GPU (#1266), decided at load from amdgpu's sysfs
    // (ROCmGPUProbeMath::isAmdApu), parallel to devices.
    std::vector<bool> integrated;
    // Which sensors each device reports, found by the first enumerateGPUs() that sees it awake
    // (#1112). Unset while it has only been seen asleep: it isn't woken to find out (#1117), and
    // rescanGPUs() asks for a re-enumeration once it is awake (#1289).
    std::vector<std::optional<GPUCapabilities>> sensors;
    std::string pciDevicesRoot;
    // AMD display devices in sysfs at the last full rescan (PciDisplayDevices::list): a change means
    // a GPU was hot-plugged, removed or rebound, which ROCm SMI only sees after a re-init (#1116).
    std::vector<std::string> pciDevicesSeen;
    // A read returned RSMI_STATUS_INIT_ERROR since the last (re)init, or the last load or re-init
    // failed while an amdgpu-bound GPU is present: the next full rescan re-initialises ROCm SMI (#1116).
    bool reinitNeeded = false;
    // dlopen() and dlsym() have succeeded (the library stays loaded across a re-init).
    bool symbolsLoaded = false;
    // dlopen() and dlsym() have succeeded at least once: ROCm SMI is installed, so a failed start is
    // the driver not being ready rather than ROCm SMI missing, and is worth retrying.
    bool libraryFound = false;

    /// Whether a failed load or re-init is retried at the next full rescan: ROCm SMI is installed and
    /// a GPU bound to amdgpu is present, so it should come up once the driver is ready. Otherwise (no
    /// AMD GPU, or one on radeon or vfio-pci, or ROCm SMI not installed) retrying could never succeed;
    /// a driver binding later changes the PCI list, which restarts ROCm SMI anyway.
    [[nodiscard]] bool loadFailureIsRetryable() const
    {
        return libraryFound && PciDisplayDevices::anyBoundTo(pciDevicesSeen, PciDisplayDevices::DRIVER_AMDGPU);
    }

    /// Note a read's result: an initialisation error means ROCm SMI must be re-initialised (#1116).
    rsmi_status_t noteResult(rsmi_status_t result)
    {
        if (result == RSMI_STATUS_INIT_ERROR)
        {
            reinitNeeded = true;
        }
        return result;
    }

    /// Whether device `deviceIdx` is runtime-suspended now, so must not be queried (#1117).
    [[nodiscard]] bool asleep(std::uint32_t deviceIdx) const
    {
        return deviceIdx < sysfsPaths.size() && PciRuntimePm::isRuntimeSuspended(sysfsPaths[deviceIdx]);
    }

    // ROCm SMI function pointers
    rsmi_status_t (*rsmi_init)(std::uint64_t) = nullptr;
    rsmi_status_t (*rsmi_shut_down)() = nullptr;
    rsmi_status_t (*rsmi_num_monitor_devices)(std::uint32_t*) = nullptr;
    rsmi_status_t (*rsmi_dev_name_get)(std::uint32_t, char*, std::size_t) = nullptr;
    rsmi_status_t (*rsmi_dev_id_get)(std::uint32_t, std::uint16_t*) = nullptr;
    rsmi_status_t (*rsmi_dev_pci_id_get)(std::uint32_t, std::uint64_t*) = nullptr;
    rsmi_status_t (*rsmi_dev_unique_id_get)(std::uint32_t, std::uint64_t*) = nullptr;
    rsmi_status_t (*rsmi_dev_gpu_busy_percent_get)(std::uint32_t, std::uint32_t*) = nullptr;
    rsmi_status_t (*rsmi_dev_memory_usage_get)(std::uint32_t, rsmi_memory_type_t, std::uint64_t*) = nullptr;
    rsmi_status_t (*rsmi_dev_memory_total_get)(std::uint32_t, rsmi_memory_type_t, std::uint64_t*) = nullptr;
    rsmi_status_t (*rsmi_dev_temp_metric_get)(std::uint32_t, rsmi_temperature_type_t, rsmi_temperature_metric_t, std::int64_t*) = nullptr;
    rsmi_status_t (*rsmi_dev_power_ave_get)(std::uint32_t, std::uint32_t, std::uint64_t*) = nullptr;
    rsmi_status_t (*rsmi_dev_power_cap_get)(std::uint32_t, std::uint32_t, std::uint64_t*) = nullptr;
    // rsmi_frequencies_t's layout depends on the library version: see frequenciesLayout.
    rsmi_status_t (*rsmi_dev_gpu_clk_freq_get)(std::uint32_t, rsmi_clk_type_t, rsmi_frequencies_t*) = nullptr;
    rsmi_status_t (*rsmi_version_get)(rsmi_version_t*) = nullptr;
    rsmi_status_t (*rsmi_dev_fan_speed_get)(std::uint32_t, std::uint32_t, std::int64_t*) = nullptr;
    rsmi_status_t (*rsmi_dev_fan_speed_max_get)(std::uint32_t, std::uint32_t, std::uint64_t*) = nullptr;
    const char* (*rsmi_status_string)(rsmi_status_t) = nullptr;

    // Which rsmi_frequencies_t layout to try first for the loaded library; set in loadROCmSMI().
    ROCmGPUProbeMath::FrequenciesLayout frequenciesLayout = ROCmGPUProbeMath::FrequenciesLayout::V6;

    bool loadROCmSMI();
    bool loadSymbols();
    /// rsmi_init() and the device list; on failure the library is unloaded and false returned.
    bool startROCmSMI();
    /// Re-initialise ROCm SMI and rebuild the device list (#1116), keeping each surviving device's
    /// last-known VRAM total and the sensor set already found for it, so one asleep through the
    /// restart doesn't lose it (it isn't woken to find it again, #1117). ROCm is left unavailable if
    /// the re-init fails or finds no device.
    void restartROCmSMI();
    void unloadROCmSMI();
    [[nodiscard]] std::string getROCmError(rsmi_status_t result) const;

    // Derive a stable device ID using the same chain as enumerateGPUs() so
    // that GPUCounters::gpuId always matches GPUInfo::id for domain correlation.
    [[nodiscard]] std::string deriveDeviceId(std::uint32_t deviceIdx) const;
};

bool ROCmGPUProbe::Impl::loadROCmSMI()
{
    if (initialized)
    {
        return true;
    }
    if (!symbolsLoaded && !loadSymbols())
    {
        return false;
    }
    return startROCmSMI();
}

bool ROCmGPUProbe::Impl::loadSymbols()
{
    // Try to load librocm_smi64.so (dynamic loading for graceful fallback)
    // Note: tests inject the mock by setting LD_LIBRARY_PATH before process start (via CTest ENVIRONMENT_MODIFICATION)
    if (rocmHandle == nullptr)
    {
        rocmHandle = dlopen("librocm_smi64.so.6", RTLD_NOW);
    }
    if (rocmHandle == nullptr)
    {
        rocmHandle = dlopen("librocm_smi64.so", RTLD_NOW);
        if (rocmHandle == nullptr)
        {
            // NOLINTNEXTLINE(concurrency-mt-unsafe) - dlerror() is safe in single-threaded init
            spdlog::debug("ROCmGPUProbe: Failed to load librocm_smi64.so - {}", dlerror());
            return false;
        }
    }

// Load function pointers
// Note: dlsym returns void* by POSIX definition; reinterpret_cast to the function pointer type is required
// and safe here because we only use it for known ROCm SMI symbols with matching signatures.
// NOLINTBEGIN(concurrency-mt-unsafe,bugprone-macro-parentheses) - dlerror() safe in single-threaded init, macro pattern is correct
#define LOAD_ROCM_FUNC(name)                                                                                                               \
    name = reinterpret_cast<decltype(name)>(dlsym(rocmHandle, #name));                                                                     \
    if (name == nullptr)                                                                                                                   \
    {                                                                                                                                      \
        spdlog::warn("ROCmGPUProbe: Failed to load function {} - {}", #name, dlerror());                                                   \
        unloadROCmSMI();                                                                                                                   \
        return false;                                                                                                                      \
    }
// Optional symbols degrade gracefully: if missing, the dependent metric is simply left
// unavailable instead of taking down the entire probe (unlike LOAD_ROCM_FUNC above).
#define LOAD_ROCM_FUNC_OPTIONAL(name)                                                                                                      \
    name = reinterpret_cast<decltype(name)>(dlsym(rocmHandle, #name));                                                                     \
    if (name == nullptr)                                                                                                                   \
    {                                                                                                                                      \
        spdlog::debug("ROCmGPUProbe: Optional function {} not available - {}", #name, dlerror());                                          \
    }

    LOAD_ROCM_FUNC(rsmi_init);
    LOAD_ROCM_FUNC(rsmi_shut_down);
    LOAD_ROCM_FUNC(rsmi_num_monitor_devices);
    LOAD_ROCM_FUNC(rsmi_dev_name_get);
    LOAD_ROCM_FUNC(rsmi_dev_id_get);
    LOAD_ROCM_FUNC(rsmi_dev_pci_id_get);
    LOAD_ROCM_FUNC(rsmi_dev_unique_id_get);
    LOAD_ROCM_FUNC(rsmi_dev_gpu_busy_percent_get);
    LOAD_ROCM_FUNC(rsmi_dev_memory_usage_get);
    LOAD_ROCM_FUNC(rsmi_dev_memory_total_get);
    LOAD_ROCM_FUNC(rsmi_dev_temp_metric_get);
    LOAD_ROCM_FUNC(rsmi_dev_power_ave_get);
    LOAD_ROCM_FUNC(rsmi_dev_power_cap_get);
    LOAD_ROCM_FUNC(rsmi_dev_gpu_clk_freq_get);
    LOAD_ROCM_FUNC(rsmi_dev_fan_speed_get);
    LOAD_ROCM_FUNC_OPTIONAL(rsmi_dev_fan_speed_max_get);
    LOAD_ROCM_FUNC_OPTIONAL(rsmi_version_get);
    LOAD_ROCM_FUNC(rsmi_status_string);

#undef LOAD_ROCM_FUNC
#undef LOAD_ROCM_FUNC_OPTIONAL
    // NOLINTEND(concurrency-mt-unsafe,bugprone-macro-parentheses)

    symbolsLoaded = true;
    libraryFound = true;
    return true;
}

bool ROCmGPUProbe::Impl::startROCmSMI()
{
    // Initialize ROCm SMI (flags = 0 for default initialization)
    rsmi_status_t result = rsmi_init(0);
    if (result != RSMI_STATUS_SUCCESS)
    {
        spdlog::warn("ROCmGPUProbe: Failed to initialize ROCm SMI - {}", getROCmError(result));
        unloadROCmSMI();
        return false;
    }

    // Get device count
    result = rsmi_num_monitor_devices(&deviceCount);
    if (result != RSMI_STATUS_SUCCESS || deviceCount == 0)
    {
        spdlog::debug("ROCmGPUProbe: No AMD GPUs found or failed to get device count - {}", getROCmError(result));
        rsmi_shut_down();
        unloadROCmSMI();
        return false;
    }

    // rsmi_frequencies_t's layout depends on the library version (#1088).
    std::optional<std::uint32_t> libraryMajor;
    if (rsmi_version_t version{}; rsmi_version_get != nullptr && rsmi_version_get(&version) == RSMI_STATUS_SUCCESS)
    {
        libraryMajor = version.major;
    }
    frequenciesLayout = ROCmGPUProbeMath::frequenciesLayoutFor(libraryMajor);
    spdlog::debug("ROCmGPUProbe: ROCm SMI library major version {}, frequency layout {}",
                  libraryMajor.has_value() ? std::to_string(*libraryMajor) : std::string("unknown"),
                  frequenciesLayout == ROCmGPUProbeMath::FrequenciesLayout::V5 ? "ROCm 5" : "ROCm 6+");

    // Populate device handles
    devices.clear();
    devices.reserve(deviceCount);
    deviceIds.clear();
    deviceIds.reserve(deviceCount);
    for (std::uint32_t i = 0; i < deviceCount; ++i)
    {
        devices.push_back(i);
        deviceIds.push_back(deriveDeviceId(i));
    }
    // PCI addresses, looked up after every id so a failing lookup here can't change an id.
    sysfsPaths.assign(deviceCount, std::string{});
    lastMemoryTotalBytes.assign(deviceCount, 0);
    for (std::uint32_t i = 0; i < deviceCount; ++i)
    {
        if (std::uint64_t bdfId = 0; rsmi_dev_pci_id_get(i, &bdfId) == RSMI_STATUS_SUCCESS)
        {
            sysfsPaths[i] = pciDevicesRoot + "/" + ROCmGPUProbeMath::sysfsPciAddress(bdfId);
        }
    }
    names.assign(deviceCount, std::string{});
    for (std::uint32_t i = 0; i < deviceCount; ++i)
    {
        // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays) - C API buffer
        char nameBuf[RSMI_MAX_BUFFER_LENGTH] = {};
        names[i] =
            (rsmi_dev_name_get(i, nameBuf, sizeof(nameBuf)) == RSMI_STATUS_SUCCESS) ? std::string(nameBuf) : "AMD GPU " + std::to_string(i);
    }
    sensors.assign(deviceCount, std::nullopt);
    // ROCm SMI has no APU flag, so ask amdgpu (#1266): the graphics core's IP version, the signal its
    // own AMD_IS_APU flag comes from, or else the PCI device id. Both are cached sysfs attributes
    // (rsmi_dev_id_get reads the same file), so this never wakes a sleeping GPU (#1117).
    integrated.assign(deviceCount, false);
    for (std::uint32_t i = 0; i < deviceCount; ++i)
    {
        std::optional<ROCmGPUProbeMath::GcIpVersion> gcVersion;
        std::optional<std::uint16_t> pciDeviceId;
        if (!sysfsPaths[i].empty())
        {
            gcVersion = readGcIpVersion(sysfsPaths[i]);
            if (std::uint32_t id = 0; PciDisplayDevices::readHexAttribute(sysfsPaths[i] + "/device", id) && id != 0 && id <= 0xFFFFU)
            {
                pciDeviceId = static_cast<std::uint16_t>(id);
            }
        }
        if (std::uint16_t id = 0; !pciDeviceId.has_value() && rsmi_dev_id_get(i, &id) == RSMI_STATUS_SUCCESS && id != 0)
        {
            pciDeviceId = id;
        }
        integrated[i] = ROCmGPUProbeMath::isAmdApu(gcVersion, pciDeviceId);
    }

    initialized = true;
    reinitNeeded = false;
    spdlog::info("ROCmGPUProbe: Initialized successfully with {} AMD GPU(s)", deviceCount);
    return true;
}

void ROCmGPUProbe::Impl::restartROCmSMI()
{
    // What each device learnt while it was known, by id: a GPU suspended now can't be asked again.
    struct Remembered
    {
        std::uint64_t lastMemoryTotalBytes = 0;
        std::optional<GPUCapabilities> sensors;
    };
    std::unordered_map<std::string, Remembered> remembered;
    for (std::size_t i = 0; i < deviceIds.size() && i < lastMemoryTotalBytes.size() && i < sensors.size(); ++i)
    {
        remembered.emplace(deviceIds[i], Remembered{.lastMemoryTotalBytes = lastMemoryTotalBytes[i], .sensors = sensors[i]});
    }

    // rsmi_shut_down() then rsmi_init() starts over with the library still loaded; a failed start
    // unloads it, and the next restart loads it again.
    if (initialized && rsmi_shut_down != nullptr)
    {
        rsmi_shut_down();
    }
    initialized = false;
    deviceCount = 0;
    devices.clear();
    deviceIds.clear();
    sysfsPaths.clear();
    lastMemoryTotalBytes.clear();
    names.clear();
    sensors.clear();
    integrated.clear();
    // Retried at the next full rescan if it fails while an amdgpu-bound GPU is present (a driver
    // mid-reload).
    reinitNeeded = !loadROCmSMI() && loadFailureIsRetryable();
    for (std::size_t i = 0; i < deviceIds.size(); ++i)
    {
        if (const auto it = remembered.find(deviceIds[i]); it != remembered.end())
        {
            lastMemoryTotalBytes[i] = it->second.lastMemoryTotalBytes;
            sensors[i] = it->second.sensors;
        }
    }
}

void ROCmGPUProbe::Impl::unloadROCmSMI()
{
    if (rocmHandle != nullptr)
    {
        dlclose(rocmHandle);
        rocmHandle = nullptr;
    }
    initialized = false;
    symbolsLoaded = false;
    deviceCount = 0;
    devices.clear();
    deviceIds.clear();
    sysfsPaths.clear();
    lastMemoryTotalBytes.clear();
    names.clear();
    sensors.clear();
    integrated.clear();
}

std::string ROCmGPUProbe::Impl::getROCmError(rsmi_status_t result) const
{
    return ROCmGPUProbeMath::resolveErrorString(result, rsmi_status_string);
}

std::string ROCmGPUProbe::Impl::deriveDeviceId(std::uint32_t deviceIdx) const
{
    // Chain: uniqueId → pciId → "amd_N" fallback. Must match the enumerateGPUs() chain
    // exactly so GPUCounters::gpuId correlates to GPUInfo::id in the domain layer.
    return ROCmGPUProbeMath::deriveDeviceId(deviceIdx, rsmi_dev_unique_id_get, rsmi_dev_pci_id_get);
}

ROCmGPUProbe::ROCmGPUProbe(std::string pciDevicesRoot) : m_Impl(std::make_unique<Impl>())
{
    m_Impl->pciDevicesRoot = std::move(pciDevicesRoot);
    m_Impl->pciDevicesSeen = PciDisplayDevices::list(m_Impl->pciDevicesRoot, PciDisplayDevices::PCI_VENDOR_AMD);
    // ROCm SMI installed but not starting while an amdgpu-bound GPU is present (TaskSmack started
    // during a driver reload, say) is retried at the next full rescan, which reports the GPUs it finds.
    if (!m_Impl->loadROCmSMI() && m_Impl->loadFailureIsRetryable())
    {
        m_Impl->reinitNeeded = true;
        spdlog::info("ROCmGPUProbe: ROCm SMI did not start with an AMD GPU present; retrying at the next full rescan");
    }
}

ROCmGPUProbe::~ROCmGPUProbe()
{
    if (m_Impl && m_Impl->initialized && m_Impl->rsmi_shut_down != nullptr)
    {
        m_Impl->rsmi_shut_down();
    }
    m_Impl->unloadROCmSMI();
}

bool ROCmGPUProbe::isAvailable() const
{
    return m_Impl && m_Impl->initialized;
}

std::vector<GPUInfo> ROCmGPUProbe::enumerateGPUs()
{
    std::vector<GPUInfo> gpus;

    if (!isAvailable())
    {
        return gpus;
    }

    gpus.reserve(m_Impl->deviceCount);

    for (std::uint32_t deviceIdx = 0; deviceIdx < m_Impl->deviceCount; ++deviceIdx)
    {
        GPUInfo info{};
        info.deviceIndex = deviceIdx;
        info.vendor = "AMD";
        info.isIntegrated = m_Impl->integrated[deviceIdx]; // An APU's GPU, decided at load (#1266)
        info.name = m_Impl->names[deviceIdx];              // Read at load ("AMD GPU N" if ROCm SMI has none)

        // The id resolved once at load (uniqueId → pciId → "amd_N", #1162); readGPUCounters() uses
        // the same cached value, so GPUInfo::id and GPUCounters::gpuId always match.
        info.id = m_Impl->deviceIds[deviceIdx];

        // Driver version: ROCm SMI doesn't directly expose driver version
        // We could read from /sys/module/amdgpu/version, but keeping it simple for now
        info.driverVersion = "ROCm";

        // Which sensors this device actually reports (#1112): capabilities() covers ROCm SMI as a
        // whole, but e.g. an APU or a passively cooled card has no fan, and older parts have no
        // junction sensor. Only a definitive answer (not supported, not found, not implemented) means
        // the device lacks a sensor: a transient failure now (busy, a reset) must not hide it for the
        // session, since the answer is kept (#1111). Found once per device: a sleeping GPU isn't woken
        // to find out (#1117), so until it is seen awake the probe's capabilities apply to it, and
        // rescanGPUs() asks for a re-enumeration then (#1289).
        auto& deviceSensors = m_Impl->sensors[deviceIdx];
        if (!deviceSensors.has_value() && !m_Impl->asleep(deviceIdx))
        {
            const auto supported = [](rsmi_status_t result)
            {
                return result != RSMI_STATUS_NOT_SUPPORTED && result != RSMI_STATUS_NOT_FOUND && result != RSMI_STATUS_NOT_YET_IMPLEMENTED;
            };
            GPUCapabilities sensors = capabilities();
            std::int64_t probeTemp = 0;
            sensors.hasTemperature =
                supported(m_Impl->rsmi_dev_temp_metric_get(deviceIdx, RSMI_TEMP_TYPE_EDGE, RSMI_TEMP_CURRENT, &probeTemp));
            sensors.hasHotspotTemp =
                supported(m_Impl->rsmi_dev_temp_metric_get(deviceIdx, RSMI_TEMP_TYPE_JUNCTION, RSMI_TEMP_CURRENT, &probeTemp));
            std::uint64_t probePower = 0;
            sensors.hasPowerMetrics = supported(m_Impl->rsmi_dev_power_ave_get(deviceIdx, 0, &probePower));
            ROCmGPUProbeMath::RsmiFrequenciesBuffer probeFreq;
            const rsmi_status_t freqResult = m_Impl->rsmi_dev_gpu_clk_freq_get(deviceIdx, RSMI_CLK_TYPE_SYS, asFrequencies(probeFreq));
            // Only a definitive answer removes the clock. A sample that came back but can't be decoded
            // now (a zero frequency, an out-of-range current index) may decode next time, and
            // readGPUCounters() already reports such samples as unavailable (a gap).
            sensors.hasClockSpeeds = supported(freqResult);
            std::int64_t probeFan = 0;
            sensors.hasFanSpeed = sensors.hasFanSpeed && supported(m_Impl->rsmi_dev_fan_speed_get(deviceIdx, 0, &probeFan));
            deviceSensors = sensors;
        }
        info.sensorCapabilities = deviceSensors;

        gpus.push_back(std::move(info));
    }

    return gpus;
}

std::vector<GPUCounters> ROCmGPUProbe::readGPUCounters()
{
    std::vector<GPUCounters> counters;

    if (!isAvailable())
    {
        return counters;
    }

    counters.reserve(m_Impl->deviceCount);

    for (std::uint32_t deviceIdx = 0; deviceIdx < m_Impl->deviceCount; ++deviceIdx)
    {
        GPUCounters counter{};

        // The id cached at load (#1162), the same value enumerateGPUs() reports as GPUInfo::id, so
        // a lookup failing later can't give this sample a different id.
        counter.gpuId = m_Impl->deviceIds[deviceIdx];

        // A runtime-suspended GPU gets no ROCm SMI query at all, which would wake it (#1117): every
        // reading is unavailable this sample, and the VRAM total is the last one read awake.
        if (m_Impl->asleep(deviceIdx))
        {
            counter.suspended = true;
            counter.utilizationAvailable = false;
            counter.temperatureAvailable = false;
            counter.powerAvailable = false;
            counter.gpuClockAvailable = false;
            counter.memoryAvailable = false;
            counter.memoryTotalBytes = m_Impl->lastMemoryTotalBytes[deviceIdx];
            counters.push_back(std::move(counter));
            continue;
        }

        // GPU utilization (0-100%)
        std::uint32_t busyPercent = 0;
        rsmi_status_t result = m_Impl->noteResult(m_Impl->rsmi_dev_gpu_busy_percent_get(deviceIdx, &busyPercent));
        if (result == RSMI_STATUS_SUCCESS)
        {
            counter.utilizationPercent = static_cast<double>(busyPercent);
        }
        else
        {
            counter.utilizationAvailable = false; // Unread this sample: not a real 0% (#1111)
        }

        // Memory usage (VRAM)
        std::uint64_t memUsed = 0;
        result = m_Impl->rsmi_dev_memory_usage_get(deviceIdx, RSMI_MEM_TYPE_VRAM, &memUsed);
        if (result == RSMI_STATUS_SUCCESS)
        {
            counter.memoryUsedBytes = memUsed;
        }
        else
        {
            counter.memoryAvailable = false; // Unread this sample: not a real 0% (#1111)
        }

        std::uint64_t memTotal = 0;
        result = m_Impl->rsmi_dev_memory_total_get(deviceIdx, RSMI_MEM_TYPE_VRAM, &memTotal);
        if (result == RSMI_STATUS_SUCCESS)
        {
            counter.memoryTotalBytes = memTotal;
            m_Impl->lastMemoryTotalBytes[deviceIdx] = memTotal;
        }
        else
        {
            counter.memoryAvailable = false;
        }

        // Memory utilization percentage is computed by Domain layer from memoryUsedBytes/memoryTotalBytes
        // Platform layer provides raw counters only

        // Temperature (edge/die temperature)
        std::int64_t tempMilliC = 0;
        result = m_Impl->rsmi_dev_temp_metric_get(deviceIdx, RSMI_TEMP_TYPE_EDGE, RSMI_TEMP_CURRENT, &tempMilliC);
        if (result == RSMI_STATUS_SUCCESS)
        {
            counter.temperatureC = static_cast<std::int32_t>(tempMilliC / 1000); // Convert milli-degrees to degrees
        }
        else
        {
            counter.temperatureAvailable = false;
        }

        // Hotspot temperature (junction temperature)
        std::int64_t hotspotMilliC = 0;
        result = m_Impl->rsmi_dev_temp_metric_get(deviceIdx, RSMI_TEMP_TYPE_JUNCTION, RSMI_TEMP_CURRENT, &hotspotMilliC);
        if (result == RSMI_STATUS_SUCCESS)
        {
            counter.hotspotTempC = static_cast<std::int32_t>(hotspotMilliC / 1000);
        }
        else
        {
            counter.hotspotTempC = -1; // Not available
        }

        // Power draw (average power in microwatts)
        std::uint64_t powerMicroW = 0;
        result = m_Impl->rsmi_dev_power_ave_get(deviceIdx, 0, &powerMicroW);
        if (result == RSMI_STATUS_SUCCESS)
        {
            counter.powerDrawWatts = static_cast<double>(powerMicroW) / 1000000.0; // Convert µW to W
        }
        else
        {
            counter.powerAvailable = false;
        }

        // Power limit (power cap in microwatts)
        std::uint64_t powerCapMicroW = 0;
        result = m_Impl->rsmi_dev_power_cap_get(deviceIdx, 0, &powerCapMicroW);
        if (result == RSMI_STATUS_SUCCESS)
        {
            counter.powerLimitWatts = static_cast<double>(powerCapMicroW) / 1000000.0; // Convert µW to W
        }

        // GPU clock speed (system clock)
        ROCmGPUProbeMath::RsmiFrequenciesBuffer gpuFreq;
        result = m_Impl->rsmi_dev_gpu_clk_freq_get(deviceIdx, RSMI_CLK_TYPE_SYS, asFrequencies(gpuFreq));
        if (const auto hz = ROCmGPUProbeMath::currentFrequencyHz(gpuFreq, m_Impl->frequenciesLayout);
            result == RSMI_STATUS_SUCCESS && hz.has_value())
        {
            counter.gpuClockMHz = static_cast<std::uint32_t>(*hz / 1000000); // Convert Hz to MHz
        }
        else
        {
            counter.gpuClockAvailable = false;
        }

        // Memory clock speed
        ROCmGPUProbeMath::RsmiFrequenciesBuffer memFreq;
        result = m_Impl->rsmi_dev_gpu_clk_freq_get(deviceIdx, RSMI_CLK_TYPE_MEM, asFrequencies(memFreq));
        if (const auto hz = ROCmGPUProbeMath::currentFrequencyHz(memFreq, m_Impl->frequenciesLayout);
            result == RSMI_STATUS_SUCCESS && hz.has_value())
        {
            counter.memoryClockMHz = static_cast<std::uint32_t>(*hz / 1000000); // Convert Hz to MHz
        }

        // Fan speed (sensor 0). rsmi_dev_fan_speed_get() returns a raw value relative to
        // RSMI_MAX_FAN_SPEED, not RPM (rsmi_dev_fan_rpms_get() is the RPM query, a different
        // function) -- see #734. Store both raw numbers unconverted; Domain (GPUModel)
        // normalizes them to a percentage, consistent with how it derives memoryUsedPercent
        // and powerUtilPercent from other raw counter pairs. The max-speed query is loaded
        // optionally (LOAD_ROCM_FUNC_OPTIONAL): older/partial ROCm SMI builds that lack it just
        // don't report fan speed, rather than losing the whole probe.
        if (m_Impl->rsmi_dev_fan_speed_max_get != nullptr)
        {
            std::int64_t fanSpeed = 0;
            result = m_Impl->rsmi_dev_fan_speed_get(deviceIdx, 0, &fanSpeed);
            // fanSpeed == 0 is a legitimate reading (fan stopped / 0% duty on an idle GPU), not
            // a missing value, so it must still be stored -- only reject a negative value, which
            // would indicate a buggy/corrupted driver rather than "not spinning".
            if (result == RSMI_STATUS_SUCCESS && fanSpeed >= 0)
            {
                std::uint64_t maxFanSpeed = 0;
                result = m_Impl->rsmi_dev_fan_speed_max_get(deviceIdx, 0, &maxFanSpeed);
                if (result == RSMI_STATUS_SUCCESS && maxFanSpeed > 0)
                {
                    counter.fanSpeedRaw = static_cast<std::uint32_t>(fanSpeed);
                    counter.fanSpeedMaxRaw = static_cast<std::uint32_t>(maxFanSpeed);
                }
            }
        }

        // PCIe throughput: Not directly available via ROCm SMI
        // Would need to read from sysfs (/sys/class/drm/card*/device/pcie_bw)
        counter.pcieTxBytes = 0;
        counter.pcieRxBytes = 0;

        // Engine utilization: Not available via ROCm SMI
        counter.computeUtilPercent = 0.0;
        counter.encoderUtilPercent = 0.0;
        counter.decoderUtilPercent = 0.0;

        counters.push_back(std::move(counter));
    }

    return counters;
}

std::vector<ProcessGPUCounters> ROCmGPUProbe::readProcessGPUCounters()
{
    // ROCm SMI does not provide per-process GPU utilization or memory allocation
    // This is a known limitation of the ROCm ecosystem compared to NVIDIA's NVML
    // Per-process GPU memory could potentially be obtained from /sys/kernel/debug/dri/[card]/amdgpu_pm_info
    // but this requires root privileges and is not part of the standard ROCm SMI API

    return {};
}

bool ROCmGPUProbe::rescanGPUs(GPURescan depth)
{
    if (depth == GPURescan::Full)
    {
        auto seen = PciDisplayDevices::list(m_Impl->pciDevicesRoot, PciDisplayDevices::PCI_VENDOR_AMD);
        const bool pciChanged = seen != m_Impl->pciDevicesSeen;
        m_Impl->pciDevicesSeen = std::move(seen);
        if (pciChanged || m_Impl->reinitNeeded)
        {
            spdlog::info("ROCmGPUProbe: {}; re-initialising ROCm SMI", pciChanged ? "AMD PCI devices changed" : "ROCm SMI needs a re-init");
            m_Impl->restartROCmSMI();
            // As NVML: a change only if the restart worked or no amdgpu GPU is left; a transient failure
            // keeps GPUModel's known list (readings are gaps) and the next full rescan retries.
            return isAvailable() || !PciDisplayDevices::anyBoundTo(m_Impl->pciDevicesSeen, PciDisplayDevices::DRIVER_AMDGPU);
        }
    }

    // An adapter asleep when it was enumerated and awake now: enumerate again to find its sensors
    // (#1289). Reads only runtime_status, and only for such adapters.
    if (!isAvailable())
    {
        return false;
    }
    for (std::uint32_t deviceIdx = 0; deviceIdx < m_Impl->deviceCount; ++deviceIdx)
    {
        if (!m_Impl->sensors[deviceIdx].has_value() && !m_Impl->asleep(deviceIdx))
        {
            return true;
        }
    }
    return false;
}

GPUCapabilities ROCmGPUProbe::capabilities() const
{
    GPUCapabilities caps{};

    if (!isAvailable())
    {
        return caps;
    }

    // ROCm SMI provides system-level metrics
    caps.hasTemperature = true;
    caps.hasHotspotTemp = true; // Junction temperature available
    caps.hasPowerMetrics = true;
    caps.hasClockSpeeds = true;
    // Only advertised when the optional max-speed symbol loaded (see readGPUCounters()) --
    // without it we can't normalize the raw ROCm reading to a percentage, so nothing is reported.
    caps.hasFanSpeed = m_Impl->rsmi_dev_fan_speed_max_get != nullptr;
    caps.hasPCIeMetrics = false;       // Not directly available via ROCm SMI
    caps.hasEngineUtilization = false; // Not available
    caps.hasPerProcessMetrics = false; // Major limitation: no per-process data
    caps.hasEncoderDecoder = false;    // Not available via ROCm SMI
    caps.supportsMultiGPU = true;      // Multiple AMD GPUs supported

    return caps;
}

} // namespace Platform
