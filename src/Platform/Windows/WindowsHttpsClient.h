#pragma once

// One HTTPS GET through WinHTTP (#3093), the transport of the opt-in update check: the system's TLS,
// certificate store and proxy settings (WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY), and no dependency beyond
// winhttp.dll. get() blocks its caller, the update check's worker, but drives WinHTTP in its asynchronous
// mode (WINHTTP_FLAG_ASYNC): each call is started and then waited for, so a stop or the deadline can end the
// one in flight. Each get() opens its own session and closes it, so constructing the client opens nothing
// and sends nothing.
//
// The request carries the User-Agent and Accept headers it is given, and the request's extra headers
// (WinHttpAddRequestHeaders, #3646: VirusTotal's x-apikey), and nothing else of TaskSmack's. An extra header's
// name must be a header token and no value may hold CR, LF or NUL, which could end the header and add another,
// or the request is refused before anything is opened; a header's value is never logged or put in an error.
// Otherwise nothing of TaskSmack's goes with it:
// cookies and automatic authentication are off (WINHTTP_DISABLE_COOKIES, WINHTTP_DISABLE_AUTHENTICATION),
// and WinHTTP's default redirect policy never follows HTTPS to HTTP. The whole request gets the request's
// timeout. A stop wakes the worker, which closes the request handle itself: WinHTTP lets an asynchronous
// request be ended by closing its handle, but never a synchronous call's (#3333), so nothing closes a handle
// another thread is using.
//
// The WinHTTP calls are a table of function pointers, the system's by default, so tests drive a request,
// its cancellation and its teardown with fakes and no network (tests/Platform/test_WindowsHttpsClient.cpp).

#include "Platform/IHttpsClient.h"

// NOLINTBEGIN(misc-include-cleaner) - windows.h must come before winhttp.h, which uses its types
// clang-format off
#include <windows.h>
#include <winhttp.h>
// clang-format on

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <expected>
#include <mutex>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace Platform
{

namespace WindowsHttpsDetail
{

/// The WinHTTP calls get() makes: winhttp.dll's, or a test's fakes.
struct WinHttpFunctions
{
    HINTERNET(WINAPI* open)(LPCWSTR, DWORD, LPCWSTR, LPCWSTR, DWORD) = &::WinHttpOpen;
    HINTERNET(WINAPI* connect)(HINTERNET, LPCWSTR, INTERNET_PORT, DWORD) = &::WinHttpConnect;
    HINTERNET(WINAPI* openRequest)(HINTERNET, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR*, DWORD) = &::WinHttpOpenRequest;
    BOOL(WINAPI* setOption)(HINTERNET, DWORD, LPVOID, DWORD) = &::WinHttpSetOption;
    WINHTTP_STATUS_CALLBACK(WINAPI* setStatusCallback)(HINTERNET, WINHTTP_STATUS_CALLBACK, DWORD, DWORD_PTR) = &::WinHttpSetStatusCallback;
    BOOL(WINAPI* setTimeouts)(HINTERNET, int, int, int, int) = &::WinHttpSetTimeouts;
    BOOL(WINAPI* addRequestHeaders)(HINTERNET, LPCWSTR, DWORD, DWORD) = &::WinHttpAddRequestHeaders;
    BOOL(WINAPI* sendRequest)(HINTERNET, LPCWSTR, DWORD, LPVOID, DWORD, DWORD, DWORD_PTR) = &::WinHttpSendRequest;
    BOOL(WINAPI* receiveResponse)(HINTERNET, LPVOID) = &::WinHttpReceiveResponse;
    BOOL(WINAPI* queryHeaders)(HINTERNET, DWORD, LPCWSTR, LPVOID, LPDWORD, LPDWORD) = &::WinHttpQueryHeaders;
    BOOL(WINAPI* queryDataAvailable)(HINTERNET, LPDWORD) = &::WinHttpQueryDataAvailable;
    BOOL(WINAPI* readData)(HINTERNET, LPVOID, DWORD, LPDWORD) = &::WinHttpReadData;
    BOOL(WINAPI* closeHandle)(HINTERNET) = &::WinHttpCloseHandle;
};

// NOLINTEND(misc-include-cleaner)

/// Why the WinHTTP call @p step failed with @p error (GetLastError()), for the debug log:
/// "WinHttpSendRequest: the request timed out (12002)".
[[nodiscard]] std::string errorText(std::string_view step, std::uint32_t error);

/// The request's extra headers (#3646) as the lines WinHttpAddRequestHeaders takes, "name: value" each, in order;
/// or why the request can't be sent: a name that isn't an RFC 9110 token, a value (an extra header's, or Accept's)
/// holding CR, LF or NUL, which could end the header and start another, or one that isn't UTF-8. The reason names
/// no value: an extra header may be a secret (VirusTotal's API key).
[[nodiscard]] std::expected<std::vector<std::wstring>, std::string> headerLines(const HttpsRequest& request);

/// One asynchronous WinHTTP call's completion, as the request's status callback reports it to the worker
/// waiting for it (#3333). The callback runs on a WinHTTP thread, or on the worker inside the call itself.
class Completion
{
  public:
    enum class Wait : std::uint8_t
    {
        Completed, ///< The call finished: error() and bytes() say how
        Stopped,   ///< A stop was requested
        TimedOut,  ///< The deadline passed
    };

    /// Before each call: forgets the last one's completion.
    void begin();
    /// From the callback: the call finished, with @p error (0 for none) and @p bytes (what it counted, if anything).
    void complete(std::uint32_t error, std::uint32_t bytes);
    /// From the callback: WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING, the handle's last callback.
    void handleClosed();

    /// Until the call completes, a stop is requested or @p deadline passes. A stop or a passed deadline wins
    /// over a completion, even one that came before the wait began: a stopped request goes no further.
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

class WindowsHttpsClient final : public IHttpsClient
{
  public:
    WindowsHttpsClient() = default;
    /// For tests: @p functions in place of winhttp.dll's (fakes, or the real ones wrapped to count), and
    /// @p port in place of 443 (a loopback listener's).
    explicit WindowsHttpsClient(const WindowsHttpsDetail::WinHttpFunctions& functions,
                                INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT) noexcept
        : m_Fns(functions), m_Port(port)
    {}
    ~WindowsHttpsClient() override = default;

    WindowsHttpsClient(const WindowsHttpsClient&) = delete;
    WindowsHttpsClient& operator=(const WindowsHttpsClient&) = delete;
    WindowsHttpsClient(WindowsHttpsClient&&) = delete;
    WindowsHttpsClient& operator=(WindowsHttpsClient&&) = delete;

    [[nodiscard]] std::expected<HttpsResponse, std::string> get(const HttpsRequest& request, const std::stop_token& stop) override;

  private:
    WindowsHttpsDetail::WinHttpFunctions m_Fns;
    INTERNET_PORT m_Port = INTERNET_DEFAULT_HTTPS_PORT;
};

} // namespace Platform
