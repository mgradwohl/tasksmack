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
#include <cerrno>
#include <chrono>
#include <ctime>
#include <format>
#include <memory>
#include <mutex>
#include <utility>

#include <curl/curl.h>
#include <curl/curlver.h>
#include <curl/easy.h>
#include <curl/system.h>
// NOLINTBEGIN(misc-include-cleaner) - POSIX headers: include-cleaner lacks mappings for the signal-mask calls
#include <csignal>

#include <pthread.h>
// NOLINTEND(misc-include-cleaner)
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
    // Extra headers (#3094): a name is an RFC 9110 token, and a value can't end the header or the request.
    const auto tokenCharacter = [](char c)
    {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               std::string_view("!#$%&'*+-.^_`|~").contains(c);
    };
    for (const auto& [name, value] : request.headers)
    {
        if (name.empty() || !std::ranges::all_of(name, tokenCharacter))
        {
            return "A request header's name isn't a header token";
        }
        if (std::ranges::any_of(value, [](char c) { return c == '\r' || c == '\n' || c == '\0'; }))
        {
            return "A request header's value holds a line break";
        }
    }
    return std::nullopt;
}

std::optional<std::string> redirectRefusal(std::string_view location)
{
    constexpr std::string_view SCHEME = "https://";
    const auto lower = [](char c)
    {
        return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    };
    if (location.size() < SCHEME.size() || !std::ranges::equal(location.substr(0, SCHEME.size()), SCHEME, {}, lower))
    {
        return "A redirect to anything but https isn't followed";
    }
    const std::string_view rest = location.substr(SCHEME.size());
    const std::size_t authorityEnd = rest.find_first_of("/?#");
    const std::string_view authority = rest.substr(0, authorityEnd);
    if (authority.contains('@'))
    {
        return "A redirect carrying credentials isn't followed";
    }
    std::string_view host = authority;
    if (const std::size_t colon = authority.rfind(':'); colon != std::string_view::npos)
    {
        if (authority.substr(colon + 1) != "443")
        {
            return "A redirect to a port other than 443 isn't followed";
        }
        host = authority.substr(0, colon);
    }
    std::string path(authorityEnd == std::string_view::npos ? std::string_view("/") : rest.substr(authorityEnd));
    if (path.front() != '/')
    {
        path.insert(path.begin(), '/');
    }
    if (const std::optional<std::string> refused = refusal(
            {.host = std::string(host), .path = path, .userAgent = "-", .accept = {}, .timeout = {}, .maxBodyBytes = 0, .headers = {}}))
    {
        return "A redirect to an address the client wouldn't request isn't followed: " + *refused;
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

// NOLINTNEXTLINE(misc-const-correctness) - curl_write_callback's signature: char* and void*, not const
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

// NOLINTNEXTLINE(misc-const-correctness) - curl_xferinfo_callback's signature: void*, not const
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

// NOLINTBEGIN(misc-include-cleaner) - sigset_t and the sig*() calls are POSIX <signal.h>, unmapped
/// Holds SIGPIPE off this thread for its life: libcurl's own sends don't raise it, but with CURLOPT_NOSIGNAL
/// it doesn't guard a TLS library's, and the default action would end TaskSmack. One that arrives meanwhile is
/// taken before the mask is put back, so it never reaches the process.
class SigpipeBlock
{
  public:
    SigpipeBlock() noexcept
    {
        sigset_t pipe{};
        sigemptyset(&pipe);
        sigaddset(&pipe, SIGPIPE);
        sigset_t pending{};
        sigpending(&pending);
        m_WasPending = sigismember(&pending, SIGPIPE) == 1;
        m_Blocked = pthread_sigmask(SIG_BLOCK, &pipe, &m_Previous) == 0;
    }
    ~SigpipeBlock()
    {
        if (!m_Blocked)
        {
            return;
        }
        sigset_t pending{};
        sigpending(&pending);
        if (!m_WasPending && sigismember(&pending, SIGPIPE) == 1)
        {
            sigset_t pipe{};
            sigemptyset(&pipe);
            sigaddset(&pipe, SIGPIPE);
            const timespec now{.tv_sec = 0, .tv_nsec = 0};
            while (sigtimedwait(&pipe, nullptr, &now) == -1 && errno == EINTR)
            {
            }
        }
        pthread_sigmask(SIG_SETMASK, &m_Previous, nullptr);
    }
    SigpipeBlock(const SigpipeBlock&) = delete;
    SigpipeBlock& operator=(const SigpipeBlock&) = delete;
    SigpipeBlock(SigpipeBlock&&) = delete;
    SigpipeBlock& operator=(SigpipeBlock&&) = delete;

  private:
    sigset_t m_Previous{};
    bool m_WasPending = false;
    bool m_Blocked = false;
};

// NOLINTEND(misc-include-cleaner)

/// The most redirects one request follows, each checked by redirectRefusal() first.
constexpr int MAX_REDIRECTS = 5;

} // namespace

bool LinuxHttpsClient::built() noexcept
{
    return true;
}

bool LinuxHttpsClient::usable() noexcept
{
    const curl_version_info_data* const info = curl_version_info(CURLVERSION_NOW);
    if (info == nullptr)
    {
        return false;
    }
#ifdef CURL_VERSION_THREADSAFE
    const bool threadSafe = (info->features & CURL_VERSION_THREADSAFE) != 0;
#else
    const bool threadSafe = false; // headers older than 7.84 can't ask
#endif
    return threadSafe && (info->features & CURL_VERSION_ASYNCHDNS) != 0;
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
    const SigpipeBlock noSigpipe; // before the handle, so it covers the cleanup too
    initialiseCurl();
    const std::unique_ptr<CURL, EasyDeleter> curl(curl_easy_init());
    if (curl == nullptr)
    {
        return std::unexpected(std::string("libcurl couldn't start a request"));
    }
    std::string url = "https://" + request.host + request.path;
    // Only the request's own headers: an empty "Accept:" removes libcurl's default "Accept: */*".
    curl_slist* headers = curl_slist_append(nullptr, request.accept.empty() ? "Accept:" : ("Accept: " + request.accept).c_str());
    const std::unique_ptr<curl_slist, ListDeleter> headerList(headers);
    for (const auto& [name, value] : request.headers)
    {
        // Each appended to the list headerList owns; refusal() has checked them. Never logged: a value can be a key.
        std::string line = name;
        line += ": ";
        line += value;
        if (const curl_slist* const grown = curl_slist_append(headerList.get(), line.c_str()); grown == nullptr)
        {
            return std::unexpected(std::string("libcurl couldn't add a request header"));
        }
    }
    Transfer transfer{.body = {}, .maxBytes = request.maxBodyBytes, .tooLong = false, .stop = &stop};
    std::array<char, CURL_ERROR_SIZE> detail{};
    const auto deadline = std::chrono::steady_clock::now() + request.timeout;

    // NOLINTBEGIN(cppcoreguidelines-pro-type-vararg) - curl_easy_setopt is libcurl's variadic C API
    CURL* handle = curl.get();
    // https only, and the request isn't sent if libcurl can't promise it (an older libcurl.so.4 than the
    // headers, which doesn't know the option).
#if LIBCURL_VERSION_NUM >= 0x075500 // 7.85: the protocol list as a string
    const CURLcode httpsOnly = curl_easy_setopt(handle, CURLOPT_PROTOCOLS_STR, "https");
#else
    const CURLcode httpsOnly = curl_easy_setopt(handle, CURLOPT_PROTOCOLS, static_cast<long>(CURLPROTO_HTTPS));
#endif
    if (httpsOnly != CURLE_OK)
    {
        return std::unexpected(std::format("libcurl couldn't be limited to https: {}", curl_easy_strerror(httpsOnly)));
    }
    // Redirects are followed here, each checked first (redirectRefusal()): libcurl's own following can't
    // keep them to port 443.
    curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(handle, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(handle, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(handle, CURLOPT_NETRC, static_cast<long>(CURL_NETRC_IGNORED));
    curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L); // a worker thread: no SIGALRM (usable() requires async DNS)
    curl_easy_setopt(handle, CURLOPT_USERAGENT, request.userAgent.c_str());
    curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headerList.get());
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, &onData);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, &transfer);
    curl_easy_setopt(handle, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(handle, CURLOPT_XFERINFOFUNCTION, &onProgress);
    curl_easy_setopt(handle, CURLOPT_XFERINFODATA, &transfer);
    curl_easy_setopt(handle, CURLOPT_ERRORBUFFER, detail.data());
    // NOLINTEND(cppcoreguidelines-pro-type-vararg)

    int redirects = 0;
    for (;;)
    {
        // The whole request, every redirect included, shares one budget, as WinHTTP's steps do.
        const auto left = std::chrono::ceil<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0)
        {
            return std::unexpected(std::string("libcurl: Timeout was reached"));
        }
        const long leftMs = static_cast<long>(left.count());
        transfer.body.clear();
        detail.front() = '\0';
        // NOLINTBEGIN(cppcoreguidelines-pro-type-vararg) - libcurl's variadic C API
        curl_easy_setopt(handle, CURLOPT_URL, url.c_str());
        curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT_MS, leftMs);
        curl_easy_setopt(handle, CURLOPT_TIMEOUT_MS, leftMs);
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
        const char* location = nullptr;
        // NOLINTBEGIN(cppcoreguidelines-pro-type-vararg) - libcurl's variadic C API
        curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &status);
        curl_easy_getinfo(handle, CURLINFO_REDIRECT_URL, &location);
        // NOLINTEND(cppcoreguidelines-pro-type-vararg)
        if (status < 300 || status > 399 || location == nullptr)
        {
            return HttpsResponse{.body = std::move(transfer.body), .status = static_cast<int>(status)};
        }
        if (++redirects > MAX_REDIRECTS)
        {
            return std::unexpected(std::format("More than {} redirects", MAX_REDIRECTS));
        }
        if (std::optional<std::string> refused = LinuxHttpsDetail::redirectRefusal(location))
        {
            return std::unexpected(std::move(*refused));
        }
        url = location;
    }
}

#else

bool LinuxHttpsClient::built() noexcept
{
    return false;
}

bool LinuxHttpsClient::usable() noexcept
{
    return false;
}

std::expected<HttpsResponse, std::string> LinuxHttpsClient::get(const HttpsRequest& /*request*/, const std::stop_token& /*stop*/)
{
    return std::unexpected(std::string("This build has no HTTPS client (it was built without libcurl)"));
}

#endif

} // namespace Platform
