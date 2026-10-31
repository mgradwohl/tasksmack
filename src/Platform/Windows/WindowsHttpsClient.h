#pragma once

// One HTTPS GET through WinHTTP (#3093), the transport of the opt-in update check: the system's TLS,
// certificate store and proxy settings (WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY), and no dependency beyond
// winhttp.dll. Synchronous, on the update check's worker. Each get() opens its own session and closes
// it, so constructing the client opens nothing and sends nothing.
//
// The request carries the User-Agent and Accept headers it is given and nothing else of TaskSmack's:
// cookies and automatic authentication are off (WINHTTP_DISABLE_COOKIES, WINHTTP_DISABLE_AUTHENTICATION),
// and WinHTTP's default redirect policy never follows HTTPS to HTTP. Each step gets at most what is left of
// the request's timeout, so a request ends within about that long; a stop closes its request handle from the
// stopping thread, which ends a call blocked on it.

#include "Platform/IHttpsClient.h"

#include <cstdint>
#include <expected>
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
} // namespace WindowsHttpsDetail

} // namespace Platform
