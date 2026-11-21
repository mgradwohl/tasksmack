#include "WindowsBootPerformance.h"

#include "WinString.h"
#include "WindowsEventLogHandle.h"

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winevt.h>
// clang-format on

#pragma comment(lib, "wevtapi.lib")

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Platform::WindowsBootPerformance
{

namespace
{

// How long EvtNext may wait for the Event Log service; the read runs off the UI thread.
constexpr DWORD NEXT_TIMEOUT_MS = 10'000;

/// The newest matching event in the Diagnostics-Performance log, rendered as XML. Reads only.
[[nodiscard]] bool newestEventXml(const std::string& xpath, std::string& xml, std::uint32_t& error)
{
    const std::wstring channel = WinString::utf8ToWide(CHANNEL);
    const std::wstring query = WinString::utf8ToWide(xpath);
    // Both closed however this returns, the event before the query.
    Windows::UniqueEvtHandle results(EvtQuery(
        nullptr, channel.c_str(), query.c_str(), static_cast<DWORD>(EvtQueryChannelPath) | static_cast<DWORD>(EvtQueryReverseDirection)));
    if (!results)
    {
        error = GetLastError();
        return false;
    }
    Windows::UniqueEvtHandle event;
    DWORD returned = 0;
    if (EvtNext(results.get(), 1, event.put(), NEXT_TIMEOUT_MS, 0, &returned) == FALSE || returned == 0 || !event)
    {
        const DWORD last = GetLastError(); // ERROR_NO_MORE_ITEMS when nothing matched
        error = last == ERROR_SUCCESS ? ERROR_NO_MORE_ITEMS : last;
        return false;
    }
    DWORD used = 0;
    DWORD properties = 0;
    std::vector<wchar_t> buffer;
    bool rendered = false;
    if (EvtRender(nullptr, event.get(), EvtRenderEventXml, 0, nullptr, &used, &properties) == FALSE &&
        GetLastError() == ERROR_INSUFFICIENT_BUFFER)
    {
        buffer.resize((used / sizeof(wchar_t)) + 1);
        const auto bytes = static_cast<DWORD>(buffer.size() * sizeof(wchar_t));
        rendered = EvtRender(nullptr, event.get(), EvtRenderEventXml, bytes, buffer.data(), &used, &properties) != FALSE;
    }
    error = rendered ? 0 : GetLastError();
    event.reset();
    results.reset();
    if (!rendered)
    {
        return false;
    }
    xml = WinString::wideToUtf8(std::wstring_view(buffer.data(), wcsnlen(buffer.data(), buffer.size())));
    return true;
}

} // namespace

Functions systemFunctions()
{
    return {.newestEventXml = &newestEventXml};
}

} // namespace Platform::WindowsBootPerformance
