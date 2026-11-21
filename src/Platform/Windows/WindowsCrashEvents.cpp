#include "WindowsCrashEvents.h"

#include "WinString.h"
#include "WindowsEventLogHandle.h"

// clang-format off
#include <windows.h>
#include <winevt.h>
// clang-format on

#pragma comment(lib, "wevtapi.lib")

#include <cstdint>
#include <expected>
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
/// skips, so the walk goes on. False at the end of the log (ERROR_NO_MORE_ITEMS); any other failure of EvtNext,
/// a timeout (ERROR_TIMEOUT) among them, is its error (#4375).
[[nodiscard]] std::expected<bool, std::uint32_t> nextEventXml(void* query, std::string& xml)
{
    Windows::UniqueEvtHandle event; // closed however this returns
    DWORD returned = 0;
    if (EvtNext(query, 1, event.put(), NEXT_TIMEOUT_MS, 0, &returned) == FALSE)
    {
        const DWORD error = GetLastError();
        if (error == ERROR_NO_MORE_ITEMS)
        {
            return false;
        }
        return std::unexpected(static_cast<std::uint32_t>(error));
    }
    if (returned == 0 || !event)
    {
        return false; // succeeded with nothing: nothing more to read
    }
    DWORD used = 0;
    DWORD properties = 0;
    std::vector<wchar_t> buffer;
    if (EvtRender(nullptr, event.get(), EvtRenderEventXml, 0, nullptr, &used, &properties) == FALSE &&
        GetLastError() == ERROR_INSUFFICIENT_BUFFER)
    {
        buffer.resize((used / sizeof(wchar_t)) + 1);
        const auto bytes = static_cast<DWORD>(buffer.size() * sizeof(wchar_t));
        if (EvtRender(nullptr, event.get(), EvtRenderEventXml, bytes, buffer.data(), &used, &properties) == FALSE)
        {
            buffer.clear();
        }
    }
    event.reset();
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
