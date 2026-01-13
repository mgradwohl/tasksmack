#include "Platform/Windows/PDHGPUProbe.h"

#include "Platform/Windows/WinString.h"

#include <spdlog/spdlog.h>

// Windows headers
// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>  // PDH_MORE_DATA, PDH_CSTATUS_NEW_DATA, etc.
// clang-format on

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <ranges>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{

// Engine type names from GPU Engine counter
// These match the engtype_ suffix in instance names
struct EngineTypeInfo
{
    std::string suffix;
    std::string displayName;
};

// Known GPU engine types from Windows GPU Engine counters
const std::vector<EngineTypeInfo> KNOWN_ENGINE_TYPES = {
    EngineTypeInfo{.suffix = "3D", .displayName = "3D"},
    EngineTypeInfo{.suffix = "Copy", .displayName = "Copy"},
    EngineTypeInfo{.suffix = "VideoDecode", .displayName = "VideoDecode"},
    EngineTypeInfo{.suffix = "VideoEncode", .displayName = "VideoEncode"},
    EngineTypeInfo{.suffix = "VideoProcessing", .displayName = "VideoProcessing"},
    EngineTypeInfo{.suffix = "Compute_0", .displayName = "Compute"},
    EngineTypeInfo{.suffix = "Compute_1", .displayName = "Compute"},
    EngineTypeInfo{.suffix = "Graphics_0", .displayName = "3D"},
    EngineTypeInfo{.suffix = "Graphics_1", .displayName = "3D"},
};

/// @brief Convert wstring to UTF-8 string using WinString helper
/// Note: PDH counter instance names are typically ASCII-safe (PIDs, engine types),
/// but proper UTF-8 conversion is used for robustness and consistency.
/// If conversion fails, falls back to truncation with data loss warning.
std::string wideToUtf8Fallback(const std::wstring& wide)
{
    try
    {
        return Platform::WinString::wideToUtf8(wide);
    }
    catch (const std::exception& e)
    {
        // Fallback: truncate non-ASCII (data loss but prevents crash)
        spdlog::warn("PDHGPUProbe: Failed to convert wide string to UTF-8, truncating non-ASCII: {}", e.what());
        std::string result;
        result.reserve(wide.size());
        for (const wchar_t wc : wide)
        {
            result.push_back(static_cast<char>(wc));
        }
        return result;
    }
}

/// @brief Parse a GPU Engine or GPU Process Memory instance name to extract PID and engine type
/// GPU Engine format: pid_1234_luid_0x00000000_0x0000D3A0_phys_0_eng_0_engtype_3D
/// GPU Process Memory format: pid_1234_luid_0x00000000_0x0000D3A0_phys_0
/// Simplified format (some drivers): pid_1234_luid_0x00000000_0x0000_engtype_3D
struct ParsedInstance
{
    std::int32_t pid = 0;
    std::string engineType;
    std::string gpuLuid; // LUID as string for GPU identification
    bool valid = false;
};

ParsedInstance parseInstanceName(const std::string& instanceName)
{
    ParsedInstance result;

    // Look for pid_ prefix
    const std::string pidPrefix = "pid_";
    auto pidPos = instanceName.find(pidPrefix);
    if (pidPos == std::string::npos)
    {
        return result;
    }

    // Extract PID (ends at next underscore)
    auto pidStart = pidPos + pidPrefix.length();
    auto pidEnd = instanceName.find('_', pidStart);
    if (pidEnd == std::string::npos)
    {
        return result;
    }

    std::string pidStr = instanceName.substr(pidStart, pidEnd - pidStart);
    auto [ptr, ec] = std::from_chars(pidStr.data(), pidStr.data() + pidStr.size(), result.pid);
    if (ec != std::errc{})
    {
        return result;
    }

    // Extract LUID (for GPU identification)
    // Format: luid_0x00000000_0x0000D3A0 or luid_0x00000000_0x0000
    const std::string luidPrefix = "luid_";
    auto luidPos = instanceName.find(luidPrefix);
    if (luidPos != std::string::npos)
    {
        auto luidStart = luidPos + luidPrefix.length();
        // LUID ends at _phys_, _engtype_, or end of string
        auto luidEnd = instanceName.find("_phys_", luidStart);
        if (luidEnd == std::string::npos)
        {
            luidEnd = instanceName.find("_engtype_", luidStart);
        }
        if (luidEnd == std::string::npos)
        {
            luidEnd = instanceName.length();
        }
        result.gpuLuid = instanceName.substr(luidStart, luidEnd - luidStart);
    }

    // Extract engine type (if present - GPU Process Memory doesn't have this)
    const std::string engtypePrefix = "engtype_";
    auto engtypePos = instanceName.find(engtypePrefix);
    if (engtypePos != std::string::npos)
    {
        result.engineType = instanceName.substr(engtypePos + engtypePrefix.length());
    }

    // Instance is valid if we got a PID (engine type is optional for memory counters)
    result.valid = (result.pid > 0);
    return result;
}

/// @brief Map raw engine type to display name
std::string normalizeEngineType(const std::string& rawType)
{
    // Map common engine types to friendly names
    static const std::unordered_map<std::string, std::string> ENGINE_MAP = {
        {"3D", "3D"},
        {"Copy", "Copy"},
        {"VideoEncode", "VideoEncode"},
        {"VideoDecode", "VideoDecode"},
        {"VideoProcessing", "VideoProcessing"},
        {"Compute", "Compute"},
        {"Compute_0", "Compute"},
        {"Compute_1", "Compute"},
        {"Graphics_0", "3D"},
        {"Graphics_1", "3D"},
    };

    auto it = ENGINE_MAP.find(rawType);
    if (it != ENGINE_MAP.end())
    {
        return it->second;
    }
    return rawType; // Return as-is if unknown
}

} // namespace

namespace Platform
{

struct PDHGPUProbe::Impl
{
    // PDH function pointers (loaded dynamically)
    using PdhOpenQueryFn = PDH_STATUS(WINAPI*)(LPCWSTR, DWORD_PTR, PDH_HQUERY*);
    using PdhCloseQueryFn = PDH_STATUS(WINAPI*)(PDH_HQUERY);
    using PdhAddEnglishCounterFn = PDH_STATUS(WINAPI*)(PDH_HQUERY, LPCWSTR, DWORD_PTR, PDH_HCOUNTER*);
    using PdhCollectQueryDataFn = PDH_STATUS(WINAPI*)(PDH_HQUERY);
    using PdhGetFormattedCounterValueFn = PDH_STATUS(WINAPI*)(PDH_HCOUNTER, DWORD, LPDWORD, PPDH_FMT_COUNTERVALUE);
    using PdhEnumObjectItemsFn = PDH_STATUS(WINAPI*)(LPCWSTR, LPCWSTR, LPCWSTR, LPWSTR, LPDWORD, LPWSTR, LPDWORD, DWORD, DWORD);
    using PdhRemoveCounterFn = PDH_STATUS(WINAPI*)(PDH_HCOUNTER);

    HMODULE pdhModule = nullptr;
    PdhOpenQueryFn pdhOpenQuery = nullptr;
    PdhCloseQueryFn pdhCloseQuery = nullptr;
    PdhAddEnglishCounterFn pdhAddEnglishCounter = nullptr;
    PdhCollectQueryDataFn pdhCollectQueryData = nullptr;
    PdhGetFormattedCounterValueFn pdhGetFormattedCounterValue = nullptr;
    PdhEnumObjectItemsFn pdhEnumObjectItems = nullptr;
    PdhRemoveCounterFn pdhRemoveCounter = nullptr;

    PDH_HQUERY query = nullptr;
    bool initialized = false;

    // Warm-up tracking - first sample after adding counters is meaningless
    bool warmedUp = false;

    // Instance refresh timing.
    // PDH counter paths embed the PID in the instance name (e.g., "pid_1234_luid_0x...").
    // When a new process starts using the GPU, we must re-enumerate instances to discover it
    // and add counters for it. This is separate from ProcessModel's process enumeration -
    // ProcessModel discovers all processes via OS APIs, while PDH discovers specifically which
    // processes are actively using GPU resources via Performance Counters.
    //
    // NOTE: GPU discovery has a ~5-second detection delay. A process may appear in the
    // process list before its GPU usage is detected. This is because PDH instance enumeration
    // runs on a 5-second interval to balance freshness vs. CPU overhead. Users should be
    // aware that GPU metrics lag behind process discovery.
    //
    // Trade-off: Shorter intervals = faster detection of new GPU-using processes but more
    // CPU overhead from PdhEnumObjectItems calls. Default 5s is a reasonable balance.
    // Configurable via sampling.pdh_instance_refresh_seconds in user config.
    std::chrono::steady_clock::time_point lastRefreshTime; // Default-initialized to epoch
    std::chrono::seconds instanceRefreshInterval{5};       // Configurable via setInstanceRefreshInterval()

    // Cache of last valid results - returned during warm-up to avoid data gaps
    std::vector<ProcessGPUCounters> lastValidResults;
    std::chrono::steady_clock::time_point lastValidTimestamp{};

    // Cache of counter handles per instance
    struct CounterInfo
    {
        PDH_HCOUNTER counter = nullptr;
        std::int32_t pid = 0;
        std::string engineType;
        std::string gpuLuid;
        bool isMemoryCounter = false; // true = memory counter, false = utilization counter
    };
    std::vector<CounterInfo> counters;

    bool loadPDH()
    {
        pdhModule = LoadLibraryW(L"pdh.dll");
        if (pdhModule == nullptr)
        {
            spdlog::debug("PDHGPUProbe: Failed to load pdh.dll");
            return false;
        }

        // Load function pointers
        // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast) - Required for GetProcAddress
        pdhOpenQuery = reinterpret_cast<PdhOpenQueryFn>(GetProcAddress(pdhModule, "PdhOpenQueryW"));
        pdhCloseQuery = reinterpret_cast<PdhCloseQueryFn>(GetProcAddress(pdhModule, "PdhCloseQuery"));
        pdhAddEnglishCounter = reinterpret_cast<PdhAddEnglishCounterFn>(GetProcAddress(pdhModule, "PdhAddEnglishCounterW"));
        pdhCollectQueryData = reinterpret_cast<PdhCollectQueryDataFn>(GetProcAddress(pdhModule, "PdhCollectQueryData"));
        pdhGetFormattedCounterValue =
            reinterpret_cast<PdhGetFormattedCounterValueFn>(GetProcAddress(pdhModule, "PdhGetFormattedCounterValue"));
        pdhEnumObjectItems = reinterpret_cast<PdhEnumObjectItemsFn>(GetProcAddress(pdhModule, "PdhEnumObjectItemsW"));
        pdhRemoveCounter = reinterpret_cast<PdhRemoveCounterFn>(GetProcAddress(pdhModule, "PdhRemoveCounter"));
        // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)

        if (pdhOpenQuery == nullptr || pdhCloseQuery == nullptr || pdhAddEnglishCounter == nullptr || pdhCollectQueryData == nullptr ||
            pdhGetFormattedCounterValue == nullptr || pdhEnumObjectItems == nullptr)
        {
            spdlog::debug("PDHGPUProbe: Failed to load required PDH functions");
            FreeLibrary(pdhModule);
            pdhModule = nullptr;
            return false;
        }

        spdlog::debug("PDHGPUProbe: Loaded pdh.dll successfully");
        return true;
    }

    bool initialize()
    {
        if (initialized)
        {
            return true;
        }

        if (!loadPDH())
        {
            return false;
        }

        // Open a PDH query
        PDH_STATUS status = pdhOpenQuery(nullptr, 0, &query);
        if (status != ERROR_SUCCESS)
        {
            spdlog::debug("PDHGPUProbe: PdhOpenQuery failed: 0x{:x}", static_cast<unsigned>(status));
            return false;
        }

        initialized = true;
        spdlog::info("PDHGPUProbe: Initialized successfully");
        return true;
    }

    /// @brief Check if we need to refresh the counter list (instances change as processes start/stop)
    [[nodiscard]] bool needsRefresh() const
    {
        if (counters.empty())
        {
            return true;
        }
        auto now = std::chrono::steady_clock::now();
        return (now - lastRefreshTime) >= instanceRefreshInterval;
    }

    /// @brief Refresh the counter list (enumerate instances, add counters)
    void refreshCounters()
    {
        // Clear existing counters
        for (auto& ci : counters)
        {
            if (ci.counter != nullptr && pdhRemoveCounter != nullptr)
            {
                pdhRemoveCounter(ci.counter);
            }
        }
        counters.clear();

        // Add GPU Engine counters (for utilization %)
        addGPUEngineCounters();

        // Add GPU Process Memory counters (for dedicated/shared memory)
        addGPUProcessMemoryCounters();

        lastRefreshTime = std::chrono::steady_clock::now();
        warmedUp = false; // Need to warm up after adding new counters

        spdlog::debug("PDHGPUProbe: Added {} total GPU counters", counters.size());
    }

    void addGPUEngineCounters()
    {
        // Enumerate GPU Engine instances
        DWORD counterListSize = 0;
        DWORD instanceListSize = 0;
        PDH_STATUS status = pdhEnumObjectItems(
            nullptr, nullptr, L"GPU Engine", nullptr, &counterListSize, nullptr, &instanceListSize, PERF_DETAIL_WIZARD, 0);

        // PDH_MORE_DATA is unsigned, PDH_STATUS is signed - cast for comparison
        if (static_cast<unsigned long>(status) != PDH_MORE_DATA && status != ERROR_SUCCESS)
        {
            spdlog::debug("PDHGPUProbe: GPU Engine enum sizing failed: 0x{:x}", static_cast<unsigned>(status));
            return;
        }

        if (instanceListSize == 0)
        {
            spdlog::debug("PDHGPUProbe: No GPU Engine instances found");
            return;
        }

        std::vector<wchar_t> counterList(counterListSize);
        std::vector<wchar_t> instanceList(instanceListSize);

        status = pdhEnumObjectItems(nullptr,
                                    nullptr,
                                    L"GPU Engine",
                                    counterList.data(),
                                    &counterListSize,
                                    instanceList.data(),
                                    &instanceListSize,
                                    PERF_DETAIL_WIZARD,
                                    0);

        if (status != ERROR_SUCCESS)
        {
            spdlog::debug("PDHGPUProbe: GPU Engine enum failed: 0x{:x}", static_cast<unsigned>(status));
            return;
        }

        // Parse instance list (multi-string: null-separated, double-null terminated)
        std::vector<std::wstring> instances;
        const wchar_t* ptr = instanceList.data();
        while (*ptr != L'\0')
        {
            instances.emplace_back(ptr);
            ptr += wcslen(ptr) + 1;
        }

        spdlog::debug("PDHGPUProbe: Found {} GPU Engine instances", instances.size());

        // Add a counter for each instance
        for (const auto& instance : instances)
        {
            const std::string narrowInstance = wideToUtf8Fallback(instance);

            auto parsed = parseInstanceName(narrowInstance);
            if (!parsed.valid || parsed.pid <= 0)
            {
                continue;
            }

            const std::wstring counterPath = L"\\GPU Engine(" + instance + L")\\Utilization Percentage";

            PDH_HCOUNTER counter = nullptr;
            status = pdhAddEnglishCounter(query, counterPath.c_str(), 0, &counter);
            if (status == ERROR_SUCCESS)
            {
                CounterInfo ci;
                ci.counter = counter;
                ci.pid = parsed.pid;
                ci.engineType = normalizeEngineType(parsed.engineType);
                ci.gpuLuid = parsed.gpuLuid;
                ci.isMemoryCounter = false;
                counters.push_back(std::move(ci));
            }
        }
    }

    void addGPUProcessMemoryCounters()
    {
        // Enumerate GPU Process Memory instances
        // Instance format: pid_<PID>_luid_<LUID>_phys_<N>
        DWORD counterListSize = 0;
        DWORD instanceListSize = 0;
        PDH_STATUS status = pdhEnumObjectItems(
            nullptr, nullptr, L"GPU Process Memory", nullptr, &counterListSize, nullptr, &instanceListSize, PERF_DETAIL_WIZARD, 0);

        // PDH_MORE_DATA is unsigned, PDH_STATUS is signed - cast for comparison
        if (static_cast<unsigned long>(status) != PDH_MORE_DATA && status != ERROR_SUCCESS)
        {
            spdlog::debug("PDHGPUProbe: GPU Process Memory enum sizing failed: 0x{:x}", static_cast<unsigned>(status));
            return;
        }

        if (instanceListSize == 0)
        {
            spdlog::debug("PDHGPUProbe: No GPU Process Memory instances found");
            return;
        }

        std::vector<wchar_t> counterList(counterListSize);
        std::vector<wchar_t> instanceList(instanceListSize);

        status = pdhEnumObjectItems(nullptr,
                                    nullptr,
                                    L"GPU Process Memory",
                                    counterList.data(),
                                    &counterListSize,
                                    instanceList.data(),
                                    &instanceListSize,
                                    PERF_DETAIL_WIZARD,
                                    0);

        if (status != ERROR_SUCCESS)
        {
            spdlog::debug("PDHGPUProbe: GPU Process Memory enum failed: 0x{:x}", static_cast<unsigned>(status));
            return;
        }

        // Parse instance list
        std::vector<std::wstring> instances;
        const wchar_t* ptr = instanceList.data();
        while (*ptr != L'\0')
        {
            instances.emplace_back(ptr);
            ptr += wcslen(ptr) + 1;
        }

        spdlog::debug("PDHGPUProbe: Found {} GPU Process Memory instances", instances.size());

        // Add Dedicated Usage counter for each instance
        // Counter names: "Dedicated Usage", "Shared Usage", "Total Committed"
        for (const auto& instance : instances)
        {
            const std::string narrowInstance = wideToUtf8Fallback(instance);

            auto parsed = parseInstanceName(narrowInstance);
            if (!parsed.valid || parsed.pid <= 0)
            {
                continue;
            }

            // Add Dedicated Usage counter (VRAM)
            std::wstring counterPath = L"\\GPU Process Memory(" + instance + L")\\Dedicated Usage";

            PDH_HCOUNTER counter = nullptr;
            status = pdhAddEnglishCounter(query, counterPath.c_str(), 0, &counter);
            if (status == ERROR_SUCCESS)
            {
                CounterInfo ci;
                ci.counter = counter;
                ci.pid = parsed.pid;
                ci.engineType = "DedicatedMemory";
                ci.gpuLuid = parsed.gpuLuid;
                ci.isMemoryCounter = true;
                counters.push_back(std::move(ci));
            }

            // Also add Shared Usage counter (system memory used as GPU memory)
            counterPath = L"\\GPU Process Memory(" + instance + L")\\Shared Usage";
            counter = nullptr;
            status = pdhAddEnglishCounter(query, counterPath.c_str(), 0, &counter);
            if (status == ERROR_SUCCESS)
            {
                CounterInfo ci;
                ci.counter = counter;
                ci.pid = parsed.pid;
                ci.engineType = "SharedMemory";
                ci.gpuLuid = parsed.gpuLuid;
                ci.isMemoryCounter = true;
                counters.push_back(std::move(ci));
            }
        }
    }

    void shutdown()
    {
        for (auto& ci : counters)
        {
            if (ci.counter != nullptr && pdhRemoveCounter != nullptr)
            {
                pdhRemoveCounter(ci.counter);
            }
        }
        counters.clear();

        if (query != nullptr && pdhCloseQuery != nullptr)
        {
            pdhCloseQuery(query);
            query = nullptr;
        }

        if (pdhModule != nullptr)
        {
            FreeLibrary(pdhModule);
            pdhModule = nullptr;
        }

        initialized = false;
    }
};

PDHGPUProbe::PDHGPUProbe() : m_Impl(std::make_unique<Impl>())
{
    m_Impl->initialize();
}

PDHGPUProbe::~PDHGPUProbe()
{
    if (m_Impl)
    {
        m_Impl->shutdown();
    }
}

PDHGPUProbe::PDHGPUProbe(PDHGPUProbe&&) noexcept = default;
PDHGPUProbe& PDHGPUProbe::operator=(PDHGPUProbe&&) noexcept = default;

bool PDHGPUProbe::isAvailable() const
{
    return m_Impl && m_Impl->initialized;
}

void PDHGPUProbe::setInstanceRefreshInterval(std::chrono::seconds interval)
{
    if (m_Impl)
    {
        m_Impl->instanceRefreshInterval = interval;
        spdlog::debug("PDHGPUProbe: Instance refresh interval set to {}s", interval.count());
    }
}

std::vector<ProcessGPUCounters> PDHGPUProbe::readProcessGPUCounters()
{
    if (!m_Impl || !m_Impl->initialized)
    {
        return {};
    }

    // Refresh counter list periodically (instances change as processes start/stop)
    if (m_Impl->needsRefresh())
    {
        m_Impl->refreshCounters();
    }

    if (m_Impl->counters.empty())
    {
        // Return cached results if available
        if (m_Impl->lastValidTimestamp.time_since_epoch().count() > 0)
        {
            const auto ageMs =
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - m_Impl->lastValidTimestamp)
                    .count();
            spdlog::debug("PDHGPUProbe: Returning cached results (stale {} ms)", ageMs);
        }
        return m_Impl->lastValidResults;
    }

    // Collect query data BEFORE checking warm-up to avoid race with refreshCounters()
    // This ensures we always have fresh data when warmedUp is checked
    PDH_STATUS status = m_Impl->pdhCollectQueryData(m_Impl->query);
    if (status != ERROR_SUCCESS)
    {
        spdlog::debug("PDHGPUProbe: PdhCollectQueryData failed: 0x{:x}", static_cast<unsigned>(status));
        // Return cached results on failure
        if (m_Impl->lastValidTimestamp.time_since_epoch().count() > 0)
        {
            const auto ageMs =
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - m_Impl->lastValidTimestamp)
                    .count();
            spdlog::debug("PDHGPUProbe: Returning cached results (stale {} ms)", ageMs);
        }
        return m_Impl->lastValidResults;
    }

    // Handle warm-up: First sample after adding counters is meaningless
    // PDH needs two samples to compute utilization deltas
    if (!m_Impl->warmedUp)
    {
        m_Impl->warmedUp = true;
        spdlog::debug("PDHGPUProbe: Warm-up sample collected, returning cached results");
        // Return cached results during warm-up to avoid UI gaps
        m_Impl->lastValidTimestamp = std::chrono::steady_clock::now();
        return m_Impl->lastValidResults;
    }

    std::vector<ProcessGPUCounters> result;

    // Aggregate data per PID per GPU
    // Key: (pid, gpuLuid) -> aggregated data
    struct AggKey
    {
        std::int32_t pid;
        std::string gpuLuid;

        bool operator==(const AggKey& other) const
        {
            return pid == other.pid && gpuLuid == other.gpuLuid;
        }
    };
    struct AggKeyHash
    {
        std::size_t operator()(const AggKey& k) const
        {
            return std::hash<std::int32_t>{}(k.pid) ^ (std::hash<std::string>{}(k.gpuLuid) << 1);
        }
    };
    struct AggData
    {
        double totalUtilization = 0.0;
        std::uint64_t dedicatedMemory = 0;
        std::uint64_t sharedMemory = 0;
        std::vector<std::string> engines;
    };
    std::unordered_map<AggKey, AggData, AggKeyHash> aggregated;

    // Read counter values
    for (const auto& ci : m_Impl->counters)
    {
        PDH_FMT_COUNTERVALUE value{};
        DWORD counterType = 0;

        // Use appropriate format based on counter type
        const DWORD format = ci.isMemoryCounter ? PDH_FMT_LARGE : (PDH_FMT_DOUBLE | PDH_FMT_NOCAP100);
        status = m_Impl->pdhGetFormattedCounterValue(ci.counter, format, &counterType, &value);

        if (status != ERROR_SUCCESS || (value.CStatus != ERROR_SUCCESS && value.CStatus != PDH_CSTATUS_NEW_DATA))
        {
            continue;
        }

        const AggKey key{.pid = ci.pid, .gpuLuid = ci.gpuLuid};
        auto& agg = aggregated[key];

        if (ci.isMemoryCounter)
        {
            // Memory counter - aggregate by type
            // Verify we're using the correct union member for PDH_FMT_LARGE format
            if (ci.engineType == "DedicatedMemory")
            {
                // NOLINT(cppcoreguidelines-pro-type-union-access)
                agg.dedicatedMemory += static_cast<std::uint64_t>(value.largeValue);
            }
            else if (ci.engineType == "SharedMemory")
            {
                // NOLINT(cppcoreguidelines-pro-type-union-access)
                agg.sharedMemory += static_cast<std::uint64_t>(value.largeValue);
            }
        }
        else
        {
            // Utilization counter - verify we're using the correct union member for PDH_FMT_DOUBLE format
            // NOLINT(cppcoreguidelines-pro-type-union-access)
            agg.totalUtilization += value.doubleValue;

            // Add engine type if not already present
            if (std::ranges::find(agg.engines, ci.engineType) == agg.engines.end())
            {
                agg.engines.push_back(ci.engineType);
            }
        }
    }

    // Convert to ProcessGPUCounters
    for (const auto& [key, agg] : aggregated)
    {
        // Include processes with either GPU utilization or GPU memory usage
        if (agg.totalUtilization <= 0.0 && agg.dedicatedMemory == 0 && agg.sharedMemory == 0)
        {
            continue; // Skip processes with no GPU activity
        }

        ProcessGPUCounters counter;
        counter.pid = key.pid;
        // gpuLuid already contains format like "0x00000000_0x0000F78E" from instance name.
        // Prepend "GPU_" to match DXGI's luidId format ("GPU_0x00000000_0x0000F78E").
        // ProcessModel will match this against the gpuIdToName map (which includes both gpuId and luidId).
        counter.gpuId = "GPU_" + key.gpuLuid;
        counter.gpuUtilPercent = agg.totalUtilization;
        counter.gpuMemoryBytes = agg.dedicatedMemory + agg.sharedMemory;
        counter.activeEngines = agg.engines;

        result.push_back(std::move(counter));
    }

    if (!result.empty())
    {
        spdlog::debug("PDHGPUProbe: Got {} per-process GPU entries (util + memory)", result.size());
        // Cache valid results for use during warm-up periods and for staleness checks
        m_Impl->lastValidResults = result;
        m_Impl->lastValidTimestamp = std::chrono::steady_clock::now();
    }

    return result;
}

GPUCapabilities PDHGPUProbe::capabilities() const
{
    GPUCapabilities caps{};

    if (m_Impl && m_Impl->initialized)
    {
        caps.hasPerProcessMetrics = true;
        caps.hasEngineUtilization = true;
        caps.supportsMultiGPU = true;
    }

    // PDH provides utilization but not system-level GPU metrics
    caps.hasTemperature = false;
    caps.hasPowerMetrics = false;
    caps.hasClockSpeeds = false;
    caps.hasFanSpeed = false;

    return caps;
}

} // namespace Platform
