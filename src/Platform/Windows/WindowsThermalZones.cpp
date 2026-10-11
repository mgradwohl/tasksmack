#include "WindowsThermalZones.h"

#include "WinString.h"

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>
// clang-format on

#pragma comment(lib, "pdh.lib")

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace Platform::WindowsThermalZones
{

namespace
{

/// Every instance's formatted value of @p counter, after one collection.
[[nodiscard]] PDH_STATUS readCounterArray(PDH_HCOUNTER counter, std::vector<ZoneSample>& zones)
{
    DWORD bytes = 0;
    DWORD count = 0;
    PDH_STATUS status = PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &bytes, &count, nullptr);
    if (static_cast<DWORD>(status) != PDH_MORE_DATA)
    {
        return status; // ERROR_SUCCESS here means no instances
    }
    // The items come first, then their names, in the one buffer.
    std::vector<PDH_FMT_COUNTERVALUE_ITEM_W> buffer((bytes + sizeof(PDH_FMT_COUNTERVALUE_ITEM_W) - 1) /
                                                    sizeof(PDH_FMT_COUNTERVALUE_ITEM_W));
    status = PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &bytes, &count, buffer.data());
    if (status != ERROR_SUCCESS)
    {
        return status;
    }
    for (const PDH_FMT_COUNTERVALUE_ITEM_W& item : std::span(buffer.data(), std::min<std::size_t>(count, buffer.size())))
    {
        if (item.szName == nullptr || (item.FmtValue.CStatus != PDH_CSTATUS_VALID_DATA && item.FmtValue.CStatus != PDH_CSTATUS_NEW_DATA))
        {
            continue;
        }
        // PDH_FMT_DOUBLE fills the union's doubleValue.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-union-access)
        zones.push_back({.instance = WinString::wideToUtf8(item.szName), .kelvin = item.FmtValue.doubleValue});
    }
    return ERROR_SUCCESS;
}

/// One collection of the thermal zone counter. Temperature is an instantaneous value, so one sample is enough.
[[nodiscard]] std::uint32_t readZones(std::vector<ZoneSample>& zones)
{
    PDH_HQUERY query = nullptr;
    PDH_STATUS status = PdhOpenQueryW(nullptr, 0, &query);
    if (status != ERROR_SUCCESS)
    {
        return static_cast<std::uint32_t>(status);
    }
    const std::wstring path(COUNTER_PATH);
    PDH_HCOUNTER counter = nullptr;
    status = PdhAddEnglishCounterW(query, path.c_str(), 0, &counter);
    if (status == ERROR_SUCCESS)
    {
        status = PdhCollectQueryData(query);
    }
    if (status == ERROR_SUCCESS)
    {
        status = readCounterArray(counter, zones);
    }
    PdhCloseQuery(query);
    return static_cast<std::uint32_t>(status);
}

} // namespace

Functions systemFunctions()
{
    return {.readZones = &readZones};
}

} // namespace Platform::WindowsThermalZones
