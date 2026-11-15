#pragma once

// One HTTPS GET (#3093): the transport behind the opt-in update check (Domain/UpdateCheck.h). Raw: it
// sends the request it is given and hands back the status and the body, and decides nothing about them.
// Windows uses WinHTTP (the system's TLS and proxy settings; WindowsHttpsClient); Linux libcurl (LinuxHttpsClient,
// #3238), and a Linux build without libcurl has none, so Platform::makeHttpsClient() gives null and the update
// check is not offered.
//
// Blocking: called on a worker (App::UpdateChecker), never on the UI thread (#2731).

#include <chrono>
#include <cstddef>
#include <expected>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Platform
{

/// What to ask for: https://<host><path>, on port 443.
struct HttpsRequest
{
    std::string host;                  ///< "api.github.com"
    std::string path;                  ///< "/repos/mgradwohl/tasksmack/releases/latest"
    std::string userAgent;             ///< The User-Agent header: "TaskSmack/0.10.0"
    std::string accept;                ///< The Accept header, or empty for none
    std::chrono::milliseconds timeout; ///< The most each step (name lookup, connect, send, receive) may take
    std::size_t maxBodyBytes = 0;      ///< A longer body is a failure, and no more of it is read
    /// More headers, name then value (#3094: VirusTotal's x-apikey). A name must be a header token and a value may
    /// hold no CR, LF or NUL, or the request isn't sent. Values are never logged. A request that carries any follows
    /// no redirect (#3761): a redirect reply ends it with HEADERS_REDIRECT_REFUSAL, so the headers never reach
    /// another host.
    std::vector<std::pair<std::string, std::string>> headers;
};

/// Why a request carrying extra headers ended at a redirect (#3761), on every platform.
inline constexpr std::string_view HEADERS_REDIRECT_REFUSAL = "A redirect isn't followed for a request carrying extra headers";

/// What came back.
struct HttpsResponse
{
    std::string body;
    int status = 0; ///< The HTTP status: 200, 403, 404, ...
};

class IHttpsClient
{
  public:
    IHttpsClient() = default;
    virtual ~IHttpsClient() = default;

    IHttpsClient(const IHttpsClient&) = delete;
    IHttpsClient& operator=(const IHttpsClient&) = delete;
    IHttpsClient(IHttpsClient&&) = delete;
    IHttpsClient& operator=(IHttpsClient&&) = delete;

    /// Sends @p request and reads the reply. No cookies are kept or sent, and no credentials: the request
    /// carries only the headers named in it. A stop requested through @p stop abandons the request (the
    /// worker's owner is going away). @return the status and body, or why there is none (no network, a
    /// timeout, a TLS failure, a body over the limit, a stop), for the debug log.
    [[nodiscard]] virtual std::expected<HttpsResponse, std::string> get(const HttpsRequest& request, const std::stop_token& stop) = 0;
};

} // namespace Platform
