#include "LinuxHttpsClient.h"

#include "Platform/IHttpsClient.h"

#include <algorithm>
#include <cstddef>
#include <expected>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>

#ifdef TASKSMACK_HAS_CURL
#include <array>
#include <format>
#include <memory>
#include <mutex>
#include <utility>

#include <curl/curl.h>
#include <curl/curlver.h>
#include <curl/easy.h>
#include <curl/system.h>
#endif

namespace Platform
{

namespace LinuxHttpsDetail
{

std::optional<std::string> refusal(const HttpsRequest& request)
{
    if (request.host.empty() || request.path.empty() || request.userAgent.empty())
    {
        return "The request's host, path or User-Agent is empty";
    }
    if (!request.path.starts_with('/'))
    {
        return "The request's path doesn't start with '/'";
    }
    const auto dnsCharacter = [](char c)
    {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-';
    };
    if (!std::ranges::all_of(request.host, dnsCharacter))
    {
        return "The request's host isn't a plain host name";
    }
    // The path goes into the URL as it is: nothing in it may start a fragment or break the line.
    if (std::ranges::any_of(request.path, [](char c) { return c == '#' || c == ' ' || c == '\r' || c == '\n' || c == '\0'; }))
    {
        return "The request's path holds a character a URL path can't";
    }
    return std::nullopt;
}

bool appendCapped(std::string& body, std::string_view chunk, std::size_t maxBytes)
{
    if (chunk.size() > maxBytes || body.size() > maxBytes - chunk.size())
    {
        return false;
    }
    body.append(chunk);
    return true;
}

} // namespace LinuxHttpsDetail

#ifdef TASKSMACK_HAS_CURL

namespace
{

/// What a transfer's callbacks share: the body so far, its cap, and the stop that ends it.
struct Transfer
{
    std::string body;
    std::size_t maxBytes = 0;
    bool tooLong = false;
    const std::stop_token* stop = nullptr;
};

std::size_t onData(char* data, std::size_t size, std::size_t count, void* user)
{
    auto* transfer = static_cast<Transfer*>(user);
    const std::size_t bytes = size * count;
    if (!LinuxHttpsDetail::appendCapped(transfer->body, std::string_view(data, bytes), transfer->maxBytes))
    {
        transfer->tooLong = true;
        return 0; // libcurl ends the transfer with CURLE_WRITE_ERROR
    }
    return bytes;
}

int onProgress(void* user, curl_off_t /*dlTotal*/, curl_off_t /*dlNow*/, curl_off_t /*ulTotal*/, curl_off_t /*ulNow*/)
{
    const auto* transfer = static_cast<const Transfer*>(user);
    return transfer->stop->stop_requested() ? 1 : 0; // non-zero: libcurl ends it with CURLE_ABORTED_BY_CALLBACK
}

/// curl_global_init() once, before the first handle: it isn't thread-safe, and the update check's worker is the
/// only caller.
void initialiseCurl()
{
    static std::once_flag once;
    std::call_once(once, [] { static_cast<void>(curl_global_init(CURL_GLOBAL_DEFAULT)); });
}

struct EasyDeleter
{
    void operator()(CURL* curl) const noexcept
    {
        curl_easy_cleanup(curl);
    }
};

struct ListDeleter
{
    void operator()(curl_slist* list) const noexcept
    {
        curl_slist_free_all(list);
    }
};

} // namespace

bool LinuxHttpsClient::built() noexcept
{
    return true;
}

std::expected<HttpsResponse, std::string> LinuxHttpsClient::get(const HttpsRequest& request, const std::stop_token& stop)
{
    if (stop.stop_requested())
    {
        return std::unexpected(std::string("Stopped before the request was sent"));
    }
    if (std::optional<std::string> refused = LinuxHttpsDetail::refusal(request))
    {
        return std::unexpected(std::move(*refused));
    }
    initialiseCurl();
    const std::unique_ptr<CURL, EasyDeleter> curl(curl_easy_init());
    if (curl == nullptr)
    {
        return std::unexpected(std::string("libcurl couldn't start a request"));
    }
    const std::string url = "https://" + request.host + request.path;
    // Only the request's own headers: an empty "Accept:" removes libcurl's default "Accept: */*".
    curl_slist* headers = curl_slist_append(nullptr, request.accept.empty() ? "Accept:" : ("Accept: " + request.accept).c_str());
    const std::unique_ptr<curl_slist, ListDeleter> headerList(headers);
    Transfer transfer{.body = {}, .maxBytes = request.maxBodyBytes, .tooLong = false, .stop = &stop};
    std::array<char, CURL_ERROR_SIZE> detail{};
    const long timeoutMs = std::max<long>(1, static_cast<long>(request.timeout.count()));

    // NOLINTBEGIN(cppcoreguidelines-pro-type-vararg) - curl_easy_setopt is libcurl's variadic C API
    CURL* handle = curl.get();
    curl_easy_setopt(handle, CURLOPT_URL, url.c_str());
#if LIBCURL_VERSION_NUM >= 0x075500 // 7.85: the protocol lists as strings
    curl_easy_setopt(handle, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(handle, CURLOPT_REDIR_PROTOCOLS_STR, "https");
#else
    curl_easy_setopt(handle, CURLOPT_PROTOCOLS, static_cast<long>(CURLPROTO_HTTPS));
    curl_easy_setopt(handle, CURLOPT_REDIR_PROTOCOLS, static_cast<long>(CURLPROTO_HTTPS));
#endif
    curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 1L); // as WinHTTP does, and only ever to https (above)
    curl_easy_setopt(handle, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(handle, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(handle, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(handle, CURLOPT_NETRC, static_cast<long>(CURL_NETRC_IGNORED));
    curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L); // a worker thread: no SIGALRM for name-lookup timeouts
    curl_easy_setopt(handle, CURLOPT_USERAGENT, request.userAgent.c_str());
    curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headerList.get());
    curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT_MS, timeoutMs);
    curl_easy_setopt(handle, CURLOPT_TIMEOUT_MS, timeoutMs); // the whole request, as WinHTTP's steps share one budget
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, &onData);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, &transfer);
    curl_easy_setopt(handle, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(handle, CURLOPT_XFERINFOFUNCTION, &onProgress);
    curl_easy_setopt(handle, CURLOPT_XFERINFODATA, &transfer);
    curl_easy_setopt(handle, CURLOPT_ERRORBUFFER, detail.data());
    // NOLINTEND(cppcoreguidelines-pro-type-vararg)

    const CURLcode result = curl_easy_perform(handle);
    if (transfer.tooLong)
    {
        return std::unexpected(std::format("The reply was longer than {} bytes", request.maxBodyBytes));
    }
    if (result == CURLE_ABORTED_BY_CALLBACK && stop.stop_requested())
    {
        return std::unexpected(std::string("Stopped"));
    }
    if (result != CURLE_OK)
    {
        const std::string_view why(detail.data());
        return std::unexpected(why.empty() ? std::format("libcurl: {}", curl_easy_strerror(result))
                                           : std::format("libcurl: {}: {}", curl_easy_strerror(result), why));
    }
    long status = 0;
    curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &status); // NOLINT(cppcoreguidelines-pro-type-vararg) - libcurl's C API
    return HttpsResponse{.body = std::move(transfer.body), .status = static_cast<int>(status)};
}

#else

bool LinuxHttpsClient::built() noexcept
{
    return false;
}

std::expected<HttpsResponse, std::string> LinuxHttpsClient::get(const HttpsRequest& /*request*/, const std::stop_token& /*stop*/)
{
    return std::unexpected(std::string("This build has no HTTPS client (it was built without libcurl)"));
}

#endif

} // namespace Platform
