#pragma once

// Internal implementation detail of PDHGPUProbe, split into its own header (rather than being
// defined inline in PDHGPUProbe.cpp) so test code can construct a PDHGPUProbe::Impl directly,
// with a fake PdhXxx function table injected in place of the real pdh.dll exports. This is not
// part of PDHGPUProbe's public API - normal consumers should include only PDHGPUProbe.h.

#include "Platform/GPUTypes.h"
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

#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Platform::PDHGPUProbeImplDetail
{

/// @brief Convert wstring to UTF-8 string using WinString helper
/// Note: PDH counter instance names are typically ASCII-safe (PIDs, engine types),
/// but proper UTF-8 conversion is used for robustness and consistency.
/// If conversion fails, falls back to truncation with data loss warning.
inline std::string wideToUtf8Fallback(const std::wstring& wide)
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

inline ParsedInstance parseInstanceName(const std::string& instanceName)
{
    ParsedInstance result;
    constexpr std::string_view luidHexPrefix{"0x"};

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
    if (ec != std::errc{} || ptr != (pidStr.data() + pidStr.size()))
    {
        return result;
    }

    // Extract LUID (for GPU identification)
    // Format: luid_0x00000000_0x0000D3A0 or luid_0x00000000_0x0000
    const std::string luidPrefix = "luid_";
    auto luidPos = instanceName.find(luidPrefix);
    if (luidPos == std::string::npos)
    {
        return result;
    }

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
    if (result.gpuLuid.empty())
    {
        return result;
    }
    const auto luidSeparator = result.gpuLuid.find('_');
    if (luidSeparator == std::string::npos)
    {
        return result;
    }
    const std::string_view luidHigh{result.gpuLuid.data(), luidSeparator};
    const std::string_view luidLow{result.gpuLuid.data() + luidSeparator + 1, result.gpuLuid.size() - luidSeparator - 1};
    if (!luidHigh.starts_with(luidHexPrefix) || !luidLow.starts_with(luidHexPrefix))
    {
        return result;
    }
    if (luidHigh.size() == luidHexPrefix.size() || luidLow.size() == luidHexPrefix.size())
    {
        return result;
    }
    if (luidStart < 5 || instanceName.compare(luidStart - 5, 5, "_luid") != 0)
    {
        return result;
    }
    const bool hasPhysSuffix = instanceName.find("_phys_", luidEnd) != std::string::npos;
    const bool hasEngineSuffix = instanceName.find("_engtype_", luidEnd) != std::string::npos;
    if (!hasPhysSuffix && !hasEngineSuffix)
    {
        return result;
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
inline std::string normalizeEngineType(const std::string& rawType)
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

} // namespace Platform::PDHGPUProbeImplDetail

namespace Platform
{

struct PDHGPUProbe::Impl
{
    // PDH function pointers (loaded dynamically, or injected by tests)
    using PdhOpenQueryFn = PDH_STATUS(WINAPI*)(LPCWSTR, DWORD_PTR, PDH_HQUERY*);
    using PdhCloseQueryFn = PDH_STATUS(WINAPI*)(PDH_HQUERY);
    using PdhAddEnglishCounterFn = PDH_STATUS(WINAPI*)(PDH_HQUERY, LPCWSTR, DWORD_PTR, PDH_HCOUNTER*);
    using PdhCollectQueryDataFn = PDH_STATUS(WINAPI*)(PDH_HQUERY);
    using PdhGetFormattedCounterArrayFn = PDH_STATUS(WINAPI*)(PDH_HCOUNTER, DWORD, LPDWORD, LPDWORD, PPDH_FMT_COUNTERVALUE_ITEM_W);

    /// Guard against unbounded instance-name cache growth from PID churn over long sessions.
    static constexpr std::size_t MAX_INSTANCE_CACHE_ENTRIES = 16384;

    // Wildcard counter paths. PDH expands the '*' instance wildcard on every collect.
    static constexpr const wchar_t* UTILIZATION_COUNTER_PATH = L"\\GPU Engine(*)\\Utilization Percentage";
    static constexpr const wchar_t* DEDICATED_MEMORY_COUNTER_PATH = L"\\GPU Process Memory(*)\\Dedicated Usage";
    static constexpr const wchar_t* SHARED_MEMORY_COUNTER_PATH = L"\\GPU Process Memory(*)\\Shared Usage";

    HMODULE pdhModule = nullptr;
    PdhOpenQueryFn pdhOpenQuery = nullptr;
    PdhCloseQueryFn pdhCloseQuery = nullptr;
    PdhAddEnglishCounterFn pdhAddEnglishCounter = nullptr;
    PdhCollectQueryDataFn pdhCollectQueryData = nullptr;
    PdhGetFormattedCounterArrayFn pdhGetFormattedCounterArray = nullptr;

    PDH_HQUERY query = nullptr;
    bool initialized = false;

    // Warm-up tracking - the first PdhCollectQueryData sample cannot produce utilization
    // values because PDH needs two samples to compute deltas.
    bool warmedUp = false;

    // Persistent wildcard counters. PDH expands the '*' instance wildcard on every
    // PdhCollectQueryData call, so new GPU-using processes are discovered automatically
    // each sample - no periodic PdhEnumObjectItems re-enumeration or counter rebuilds.
    // Measured on a 651-instance system: wildcard collect + array read is ~1 ms total vs
    // ~8-30 ms per collect for per-instance counters plus ~140 ms per re-enumeration.
    PDH_HCOUNTER utilizationCounter = nullptr;     // "\GPU Engine(*)\Utilization Percentage"
    PDH_HCOUNTER dedicatedMemoryCounter = nullptr; // "\GPU Process Memory(*)\Dedicated Usage"
    PDH_HCOUNTER sharedMemoryCounter = nullptr;    // "\GPU Process Memory(*)\Shared Usage"

    // Scratch buffer reused across PdhGetFormattedCounterArray calls to avoid
    // per-sample allocation churn.
    std::vector<std::byte> arrayBuffer;

    // Cache of last valid results - returned during warm-up to avoid data gaps
    std::vector<ProcessGPUCounters> lastValidResults;
    std::chrono::steady_clock::time_point lastValidTimestamp;

    /// Parsed metadata for a counter instance name. Cached because instance names repeat
    /// every sample and wide->UTF-8 conversion plus parsing is per-name work.
    struct CachedInstance
    {
        std::int32_t pid = 0;
        std::string engineType; // Normalized display name; empty for memory instances
        std::string gpuLuid;
        bool valid = false;
    };

    struct WideStringHash
    {
        using is_transparent = void;
        [[nodiscard]] std::size_t operator()(std::wstring_view value) const noexcept
        {
            return std::hash<std::wstring_view>{}(value);
        }
    };

    std::unordered_map<std::wstring, CachedInstance, WideStringHash, std::equal_to<>> instanceCache;

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;
    ~Impl() noexcept
    {
        shutdown();
    }

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
        pdhGetFormattedCounterArray =
            reinterpret_cast<PdhGetFormattedCounterArrayFn>(GetProcAddress(pdhModule, "PdhGetFormattedCounterArrayW"));
        // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)

        if (pdhOpenQuery == nullptr || pdhCloseQuery == nullptr || pdhAddEnglishCounter == nullptr || pdhCollectQueryData == nullptr ||
            pdhGetFormattedCounterArray == nullptr)
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

        // Wildcard counters expand to all current instances on every collect, so
        // per-process discovery is automatic and no re-enumeration is ever needed.
        // Failures here are not fatal: ensureCounters() retries missing counters
        // on every read so a transient PDH failure at startup can recover.
        ensureCounters();

        initialized = true;
        spdlog::info("PDHGPUProbe: Initialized successfully");
        return true;
    }

    void addWildcardCounter(const wchar_t* counterPath, PDH_HCOUNTER& counter)
    {
        const PDH_STATUS status = pdhAddEnglishCounter(query, counterPath, 0, &counter);
        if (status != ERROR_SUCCESS)
        {
            counter = nullptr;
            spdlog::debug("PDHGPUProbe: Failed to add wildcard counter: 0x{:x}", static_cast<unsigned>(status));
        }
    }

    /// @brief Retry any wildcard counters that failed to add previously.
    /// Counter-add failures can be transient (e.g. the GPU performance counter
    /// provider not yet registered at startup), so missing counters are retried
    /// on every read instead of being disabled for the probe's lifetime.
    /// @return true if at least one wildcard counter is active
    bool ensureCounters()
    {
        if (utilizationCounter == nullptr)
        {
            addWildcardCounter(UTILIZATION_COUNTER_PATH, utilizationCounter);
            if (utilizationCounter != nullptr)
            {
                // A freshly added utilization counter needs two collects before
                // it can produce rate values - re-enter warm-up.
                warmedUp = false;
            }
        }
        if (dedicatedMemoryCounter == nullptr)
        {
            addWildcardCounter(DEDICATED_MEMORY_COUNTER_PATH, dedicatedMemoryCounter);
        }
        if (sharedMemoryCounter == nullptr)
        {
            addWildcardCounter(SHARED_MEMORY_COUNTER_PATH, sharedMemoryCounter);
        }
        return utilizationCounter != nullptr || dedicatedMemoryCounter != nullptr || sharedMemoryCounter != nullptr;
    }

    /// @brief Look up (or parse and cache) instance metadata for a wide instance name
    const CachedInstance& instanceFor(std::wstring_view name)
    {
        if (const auto it = instanceCache.find(name); it != instanceCache.end())
        {
            return it->second;
        }

        if (instanceCache.size() > MAX_INSTANCE_CACHE_ENTRIES)
        {
            instanceCache.clear();
        }

        std::wstring wide(name);
        const auto parsed = PDHGPUProbeImplDetail::parseInstanceName(PDHGPUProbeImplDetail::wideToUtf8Fallback(wide));
        CachedInstance cached;
        cached.pid = parsed.pid;
        cached.engineType = parsed.engineType.empty() ? std::string{} : PDHGPUProbeImplDetail::normalizeEngineType(parsed.engineType);
        cached.gpuLuid = parsed.gpuLuid;
        cached.valid = parsed.valid;
        return instanceCache.emplace(std::move(wide), std::move(cached)).first->second;
    }

    /// @brief Read all expanded instances of a wildcard counter into the reused scratch buffer
    /// @return Pointer to the item array (valid until the next call), or nullptr on failure
    PPDH_FMT_COUNTERVALUE_ITEM_W readCounterArray(PDH_HCOUNTER counter, DWORD format, DWORD& itemCount)
    {
        itemCount = 0;
        if (counter == nullptr)
        {
            return nullptr;
        }

        auto bufferSize = static_cast<DWORD>(arrayBuffer.size());
        for (int attempt = 0; attempt < 3; ++attempt)
        {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) - PDH writes item structs into a raw byte buffer
            auto* items = arrayBuffer.empty() ? nullptr : reinterpret_cast<PPDH_FMT_COUNTERVALUE_ITEM_W>(arrayBuffer.data());
            const PDH_STATUS status = pdhGetFormattedCounterArray(counter, format, &bufferSize, &itemCount, items);
            if (status == ERROR_SUCCESS)
            {
                return items;
            }
            if (static_cast<unsigned long>(status) != PDH_MORE_DATA)
            {
                spdlog::debug("PDHGPUProbe: PdhGetFormattedCounterArray failed: 0x{:x}", static_cast<unsigned>(status));
                itemCount = 0;
                return nullptr;
            }
            arrayBuffer.resize(bufferSize);
            itemCount = 0;
        }
        return nullptr;
    }

    void shutdown()
    {
        // Counter handles are owned by the query; PdhCloseQuery releases them.
        utilizationCounter = nullptr;
        dedicatedMemoryCounter = nullptr;
        sharedMemoryCounter = nullptr;

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

} // namespace Platform
