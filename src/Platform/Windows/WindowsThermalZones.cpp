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
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace Platform::WindowsThermalZones
{

namespace
{

/// Every instance's formatted value of @p counter, after one collection. @p buffer is grown to fit
/// and kept by the caller, so a reader that calls this every tick allocates only when it must grow.
[[nodiscard]] PDH_STATUS
readCounterArray(PDH_HCOUNTER counter, std::vector<PDH_FMT_COUNTERVALUE_ITEM_W>& buffer, std::vector<ZoneSample>& zones)
{
    auto bytes = static_cast<DWORD>(buffer.size() * sizeof(PDH_FMT_COUNTERVALUE_ITEM_W));
    DWORD count = 0;
    PDH_STATUS status = PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &bytes, &count, buffer.empty() ? nullptr : buffer.data());
    if (static_cast<DWORD>(status) == PDH_MORE_DATA)
    {
        // The items come first, then their names, in the one buffer.
        buffer.resize((bytes + sizeof(PDH_FMT_COUNTERVALUE_ITEM_W) - 1) / sizeof(PDH_FMT_COUNTERVALUE_ITEM_W));
        status = PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &bytes, &count, buffer.data());
    }
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

/// One PDH query on the thermal zone counter, opened on first use and kept open until destroyed.
class ZoneQuery
{
  public:
    ZoneQuery() = default;
    ~ZoneQuery()
    {
        if (m_Query != nullptr)
        {
            PdhCloseQuery(m_Query);
        }
    }
    ZoneQuery(const ZoneQuery&) = delete;
    ZoneQuery& operator=(const ZoneQuery&) = delete;
    ZoneQuery(ZoneQuery&&) = delete;
    ZoneQuery& operator=(ZoneQuery&&) = delete;

    /// One collection. Temperature is an instantaneous value, so one sample is enough.
    [[nodiscard]] std::uint32_t read(std::vector<ZoneSample>& zones)
    {
        zones.clear();
        if (m_Query == nullptr)
        {
            if (const PDH_STATUS status = open(); status != ERROR_SUCCESS)
            {
                return static_cast<std::uint32_t>(status);
            }
        }
        PDH_STATUS status = PdhCollectQueryData(m_Query);
        if (status == ERROR_SUCCESS)
        {
            status = readCounterArray(m_Counter, m_Buffer, zones);
        }
        return static_cast<std::uint32_t>(status);
    }

  private:
    [[nodiscard]] PDH_STATUS open()
    {
        PDH_HQUERY query = nullptr;
        PDH_STATUS status = PdhOpenQueryW(nullptr, 0, &query);
        if (status != ERROR_SUCCESS)
        {
            return status;
        }
        const std::wstring path(COUNTER_PATH);
        status = PdhAddEnglishCounterW(query, path.c_str(), 0, &m_Counter);
        if (status != ERROR_SUCCESS)
        {
            PdhCloseQuery(query);
            return status; // tried again on the next read
        }
        m_Query = query;
        return ERROR_SUCCESS;
    }

    PDH_HQUERY m_Query = nullptr;
    PDH_HCOUNTER m_Counter = nullptr;
    std::vector<PDH_FMT_COUNTERVALUE_ITEM_W> m_Buffer;
};

/// One collection of the thermal zone counter, on a query of its own.
[[nodiscard]] std::uint32_t readZones(std::vector<ZoneSample>& zones)
{
    ZoneQuery query;
    return query.read(zones);
}

} // namespace

Functions systemFunctions()
{
    return {.readZones = &readZones};
}

ZoneReader makeZoneReader()
{
    // Shared, so the std::function stays copyable; the query closes with the last copy.
    return [query = std::make_shared<ZoneQuery>()](std::vector<ZoneSample>& zones)
    {
        return query->read(zones);
    };
}

} // namespace Platform::WindowsThermalZones
