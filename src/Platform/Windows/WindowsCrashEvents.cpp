#include "WindowsCrashEvents.h"

#include "WinString.h"

// clang-format off
#include <windows.h>
#include <winevt.h>
// clang-format on

#pragma comment(lib, "wevtapi.lib")

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Platform::WindowsCrashEvents
{

namespace
{

// How long one EvtNext may wait for the Event Log service; the read runs off the UI thread.
constexpr DWORD NEXT_TIMEOUT_MS = 10'000;

/// EvtQuery on the Application channel, newest first. Reads only.
[[nodiscard]] void* openQuery(const std::string& xpath, std::uint32_t& error)
{
    const std::wstring wide = WinString::utf8ToWide(xpath);
    EVT_HANDLE query = EvtQuery(
        nullptr, L"Application", wide.c_str(), static_cast<DWORD>(EvtQueryChannelPath) | static_cast<DWORD>(EvtQueryReverseDirection));
    if (query == nullptr)
    {
        error = GetLastError();
    }
    return query;
}

/// The next event, rendered as XML. An event that won't render comes back as empty text, which the parser
/// skips, so the walk goes on.
[[nodiscard]] bool nextEventXml(void* query, std::string& xml)
{
    EVT_HANDLE event = nullptr;
    DWORD returned = 0;
    if (EvtNext(query, 1, &event, NEXT_TIMEOUT_MS, 0, &returned) == FALSE || returned == 0 || event == nullptr)
    {
        return false;
    }
    DWORD used = 0;
    DWORD properties = 0;
    std::vector<wchar_t> buffer;
    if (EvtRender(nullptr, event, EvtRenderEventXml, 0, nullptr, &used, &properties) == FALSE &&
        GetLastError() == ERROR_INSUFFICIENT_BUFFER)
    {
        buffer.resize((used / sizeof(wchar_t)) + 1);
        const auto bytes = static_cast<DWORD>(buffer.size() * sizeof(wchar_t));
        if (EvtRender(nullptr, event, EvtRenderEventXml, bytes, buffer.data(), &used, &properties) == FALSE)
        {
            buffer.clear();
        }
    }
    EvtClose(event);
    xml = buffer.empty() ? std::string{} : WinString::wideToUtf8(std::wstring_view(buffer.data(), wcsnlen(buffer.data(), buffer.size())));
    return true;
}

void closeQuery(void* query)
{
    EvtClose(query);
}

} // namespace

Functions systemFunctions()
{
    return {.openQuery = &openQuery, .nextEventXml = &nextEventXml, .closeQuery = &closeQuery};
}

} // namespace Platform::WindowsCrashEvents
