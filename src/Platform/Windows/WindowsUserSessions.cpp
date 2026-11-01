#include "WindowsUserSessions.h"

#include "Platform/BrokerProtocol.h"
#include "Platform/IServiceActions.h"
#include "Platform/IUserSessions.h"
#include "Platform/Windows/WindowsUserSessionsMath.h"
#include "WinString.h"

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wtsapi32.h>
// clang-format on

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Platform::Windows
{

namespace
{

using UserSessionsMath::WtsSessionFacts;

/// What WTS allocated, given back with WTSFreeMemory.
struct WtsFree
{
    void operator()(void* memory) const noexcept
    {
        WTSFreeMemory(memory);
    }
};

/// A fixed WCHAR array of WTSINFOW as UTF-8, up to its first NUL (or its end, if it has none).
[[nodiscard]] std::string fixedText(std::span<const WCHAR> text)
{
    const auto end = std::ranges::find(text, L'\0');
    return WinString::wideToUtf8(std::wstring_view(text.data(), static_cast<std::size_t>(end - text.begin())));
}

/// Session @p sessionId's @p infoClass, or null when WTS gives none or fewer than @p minimumBytes.
[[nodiscard]] std::unique_ptr<void, WtsFree> querySession(DWORD sessionId, WTS_INFO_CLASS infoClass, std::size_t minimumBytes)
{
    LPWSTR buffer = nullptr;
    DWORD bytes = 0;
    if (WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, sessionId, infoClass, &buffer, &bytes) == FALSE || buffer == nullptr)
    {
        return nullptr;
    }
    std::unique_ptr<void, WtsFree> owned(buffer);
    if (bytes < minimumBytes)
    {
        return nullptr;
    }
    return owned;
}

/// A LARGE_INTEGER time from WTSINFOW as FILETIME ticks; 0 for a negative one.
[[nodiscard]] std::uint64_t ticksOf(const LARGE_INTEGER& time) noexcept
{
    return time.QuadPart > 0 ? static_cast<std::uint64_t>(time.QuadPart) : 0;
}

/// Session @p listed with what WTSQuerySessionInformationW adds to it. A session that ended between the
/// list and the query keeps only the list's facts.
[[nodiscard]] WtsSessionFacts readSession(const WTS_SESSION_INFOW& listed)
{
    WtsSessionFacts facts;
    facts.sessionId = listed.SessionId;
    facts.state = static_cast<std::uint32_t>(listed.State);
    facts.winStation = listed.pWinStationName != nullptr ? WinString::wideToUtf8(listed.pWinStationName) : std::string{};
    if (const auto info = querySession(listed.SessionId, WTSSessionInfo, sizeof(WTSINFOW)); info != nullptr)
    {
        const auto* const session = static_cast<const WTSINFOW*>(info.get());
        facts.state = static_cast<std::uint32_t>(session->State);
        facts.user = fixedText(session->UserName);
        facts.domain = fixedText(session->Domain);
        facts.logonTime = ticksOf(session->LogonTime);
        facts.lastInputTime = ticksOf(session->LastInputTime);
        facts.currentTime = ticksOf(session->CurrentTime);
    }
    if (const auto client = querySession(listed.SessionId, WTSClientName, sizeof(WCHAR)); client != nullptr)
    {
        facts.clientName = WinString::wideToUtf8(static_cast<const wchar_t*>(client.get()));
    }
    if (const auto protocol = querySession(listed.SessionId, WTSClientProtocolType, sizeof(USHORT)); protocol != nullptr)
    {
        facts.protocol = *static_cast<const USHORT*>(protocol.get());
    }
    return facts;
}

/// FormatMessage's words for @p error, without the trailing period and line break; empty when there are none.
[[nodiscard]] std::string describeError(std::uint32_t error)
{
    std::array<wchar_t, 512> text{};
    const DWORD length = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                        nullptr,
                                        error,
                                        0,
                                        text.data(),
                                        static_cast<DWORD>(text.size()),
                                        nullptr);
    std::wstring_view words(text.data(), std::min<std::size_t>(length, text.size()));
    while (!words.empty() && (words.back() == L'\n' || words.back() == L'\r' || words.back() == L'.' || words.back() == L' '))
    {
        words.remove_suffix(1);
    }
    return WinString::wideToUtf8(words);
}

/// Every session WTS lists on this machine.
[[nodiscard]] std::expected<std::vector<WtsSessionFacts>, std::string> enumerateSessions()
{
    PWTS_SESSION_INFOW list = nullptr;
    DWORD count = 0;
    if (WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &list, &count) == FALSE)
    {
        const DWORD error = GetLastError();
        return std::unexpected("Windows couldn't list the sessions: " + describeError(error));
    }
    const std::unique_ptr<void, WtsFree> owned(list);
    std::vector<WtsSessionFacts> sessions;
    sessions.reserve(count);
    for (const WTS_SESSION_INFOW& listed : std::span<const WTS_SESSION_INFOW>(list, count))
    {
        sessions.push_back(readSession(listed));
    }
    return sessions;
}

/// The session this process runs in; 0 (the services' session, which is never acted on) if Windows can't say.
[[nodiscard]] std::uint32_t ownSessionId() noexcept
{
    DWORD session = 0;
    if (ProcessIdToSessionId(GetCurrentProcessId(), &session) == FALSE)
    {
        return UserSessionsMath::SERVICES_SESSION;
    }
    return session;
}

/// 0 when @p done, else the thread's last error (never 0 then).
[[nodiscard]] std::uint32_t resultOf(BOOL done) noexcept
{
    if (done != FALSE)
    {
        return 0;
    }
    const DWORD error = GetLastError();
    return error != 0 ? error : ERROR_GEN_FAILURE;
}

} // namespace

WtsCalls realWtsCalls()
{
    return WtsCalls{
        .enumerate = &enumerateSessions,
        .currentSessionId = ownSessionId(),
        // Not waiting (bWait FALSE): the session's new state comes with the next read, and one's own Sign out
        // ends this process before a wait could return.
        .disconnect = [](std::uint32_t session) { return resultOf(WTSDisconnectSession(WTS_CURRENT_SERVER_HANDLE, session, FALSE)); },
        .logoff = [](std::uint32_t session) { return resultOf(WTSLogoffSession(WTS_CURRENT_SERVER_HANDLE, session, FALSE)); },
        .describe = &describeError,
    };
}

UserSessionsRead readUserSessions(const WtsCalls& calls)
{
    std::expected<std::vector<WtsSessionFacts>, std::string> listed = calls.enumerate();
    if (!listed.has_value())
    {
        return {.status = UserSessionsStatus::Failed, .sessions = {}, .detail = std::move(listed.error())};
    }
    UserSessionsRead read{.status = UserSessionsStatus::Ok, .sessions = {}, .detail = {}};
    for (const WtsSessionFacts& facts : *listed)
    {
        if (std::optional<UserSession> session = UserSessionsMath::toUserSession(facts, calls.currentSessionId); session.has_value())
        {
            read.sessions.push_back(std::move(*session));
        }
    }
    return read;
}

ServiceActionResult actOnSession(const WtsCalls& calls, std::string_view id, SessionAction action)
{
    const std::expected<std::vector<WtsSessionFacts>, std::string> listed = calls.enumerate();
    if (!listed.has_value())
    {
        return ServiceActionResult::failed(listed.error());
    }
    const std::expected<std::uint32_t, std::string> session = UserSessionsMath::checkSessionAction(id, action, *listed);
    if (!session.has_value())
    {
        return ServiceActionResult::failed(session.error());
    }
    const std::uint32_t error = action == SessionAction::Disconnect ? calls.disconnect(*session) : calls.logoff(*session);
    if (error == 0)
    {
        return ServiceActionResult::succeeded();
    }
    return UserSessionsMath::refusalOf(error, calls.describe ? calls.describe(error) : std::string{});
}

ServiceActionResult handleHelperSessionAction(const Broker::SessionActionRequest& request, const WtsCalls& calls)
{
    // The helper is elevated, so a refusal from it is never one an administrator could finish.
    ServiceActionResult result = actOnSession(calls, std::to_string(request.sessionId), request.action);
    result.needsAuthorization = false;
    return result;
}

WindowsUserSessions::WindowsUserSessions() : m_Calls(realWtsCalls())
{}

WindowsUserSessions::WindowsUserSessions(WtsCalls calls) : m_Calls(std::move(calls))
{}

} // namespace Platform::Windows
