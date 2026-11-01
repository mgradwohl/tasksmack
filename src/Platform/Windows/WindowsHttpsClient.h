#pragma once

// One HTTPS GET through WinHTTP (#3093), the transport of the opt-in update check: the system's TLS,
// certificate store and proxy settings (WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY), and no dependency beyond
// winhttp.dll. get() blocks its caller, the update check's worker, but drives WinHTTP in its asynchronous
// mode (WINHTTP_FLAG_ASYNC): each call is started and then waited for, so a stop or the deadline can end the
// one in flight. Each get() opens its own session and closes it, so constructing the client opens nothing
// and sends nothing.
//
// The request carries the User-Agent and Accept headers it is given and nothing else of TaskSmack's:
// cookies and automatic authentication are off (WINHTTP_DISABLE_COOKIES, WINHTTP_DISABLE_AUTHENTICATION),
// and WinHTTP's default redirect policy never follows HTTPS to HTTP. The whole request gets the request's
// timeout. A stop wakes the worker, which closes the request handle itself: WinHTTP lets an asynchronous
// request be ended by closing its handle, but never a synchronous call's (#3333), so nothing closes a handle
// another thread is using.

#include "Platform/IHttpsClient.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <expected>
#include <mutex>
#include <stop_token>
#include <string>
#include <string_view>

namespace Platform
{

class WindowsHttpsClient final : public IHttpsClient
{
  public:
    WindowsHttpsClient() = default;
    ~WindowsHttpsClient() override = default;

    WindowsHttpsClient(const WindowsHttpsClient&) = delete;
    WindowsHttpsClient& operator=(const WindowsHttpsClient&) = delete;
    WindowsHttpsClient(WindowsHttpsClient&&) = delete;
    WindowsHttpsClient& operator=(WindowsHttpsClient&&) = delete;

    [[nodiscard]] std::expected<HttpsResponse, std::string> get(const HttpsRequest& request, const std::stop_token& stop) override;
};

namespace WindowsHttpsDetail
{
/// Why the WinHTTP call @p step failed with @p error (GetLastError()), for the debug log:
/// "WinHttpSendRequest: the request timed out (12002)".
[[nodiscard]] std::string errorText(std::string_view step, std::uint32_t error);

/// One asynchronous WinHTTP call's completion, as the request's status callback reports it to the worker
/// waiting for it (#3333). The callback runs on a WinHTTP thread, or on the worker inside the call itself.
class Completion
{
  public:
    enum class Wait : std::uint8_t
    {
        Completed, ///< The call finished: error() and bytes() say how
        Stopped,   ///< A stop was requested first
        TimedOut,  ///< The deadline came first
    };

    /// Before each call: forgets the last one's completion.
    void begin();
    /// From the callback: the call finished, with @p error (0 for none) and @p bytes (what it counted, if anything).
    void complete(std::uint32_t error, std::uint32_t bytes);
    /// From the callback: WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING, the handle's last callback.
    void handleClosed();

    /// Until the call completes, a stop is requested or @p deadline passes, whichever is first.
    [[nodiscard]] Wait wait(const std::stop_token& stop, std::chrono::steady_clock::time_point deadline);
    /// Until handleClosed(): after it, WinHTTP no longer uses the handle, its context or a buffer it was given.
    void waitForClose();

    [[nodiscard]] std::uint32_t error() const;
    [[nodiscard]] std::uint32_t bytes() const;

  private:
    mutable std::mutex m_Mutex;
    std::condition_variable_any m_Changed;
    bool m_Completed = false;
    bool m_Closed = false;
    std::uint32_t m_Error = 0;
    std::uint32_t m_Bytes = 0;
};

} // namespace WindowsHttpsDetail

} // namespace Platform
