#pragma once

// One HTTPS GET through libcurl (#3238), the Linux transport of the opt-in update check (#3093), as
// WindowsHttpsClient is Windows'. A build dependency, found with pkg-config like libsystemd: without libcurl
// in the build (TASKSMACK_HAS_CURL undefined) there is no client, Platform::makeHttpsClient() gives null, and
// the update check isn't offered.
//
// The same contract as WinHTTP's: https on port 443 only (redirects too, each checked before it is followed),
// the system's CA store with certificate and host-name verification on, the User-Agent and Accept headers the
// request names and nothing else (no cookies, no .netrc, no credentials of TaskSmack's; a proxy the user set
// in https_proxy is used as they set it, credentials and all), the whole request bounded by its timeout, the
// body capped at maxBodyBytes, and a stop that ends a transfer in progress. Synchronous, on the update check's
// worker, so only a libcurl that is thread-safe and resolves names asynchronously is used (usable()).

#include "Platform/IHttpsClient.h"

#include <cstddef>
#include <expected>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>

namespace Platform
{

namespace LinuxHttpsDetail
{

/// Why @p request is refused before anything is sent, or nullopt when it can be sent: an empty host, path or
/// User-Agent, a path that doesn't start with '/', or a host that isn't a plain DNS name (letters, digits, '.'
/// and '-'), so nothing in it can change the URL's meaning.
[[nodiscard]] std::optional<std::string> refusal(const HttpsRequest& request);

/// Why a redirect to @p location (an absolute URL, from the reply's Location) isn't followed, or nullopt when
/// it may be: anything but https, a port other than 443, credentials in it, a host that isn't a plain DNS
/// name, or a path refusal() would refuse.
[[nodiscard]] std::optional<std::string> redirectRefusal(std::string_view location);

/// Appends @p chunk to @p body unless that would pass @p maxBytes. @return false (body unchanged) when it would.
[[nodiscard]] bool appendCapped(std::string& body, std::string_view chunk, std::size_t maxBytes);

} // namespace LinuxHttpsDetail

class LinuxHttpsClient final : public IHttpsClient
{
  public:
    /// Whether this build has libcurl: without it there is no client to make.
    [[nodiscard]] static bool built() noexcept;

    /// Whether the libcurl loaded can serve the worker: built() with it, and it reports itself thread-safe
    /// (curl_global_init() then runs safely on the worker) and resolving names asynchronously (so the timeout
    /// and a stop bound a slow lookup too). Otherwise there is no client, as without libcurl.
    [[nodiscard]] static bool usable() noexcept;

    [[nodiscard]] std::expected<HttpsResponse, std::string> get(const HttpsRequest& request, const std::stop_token& stop) override;
};

} // namespace Platform
