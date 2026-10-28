#include "WindowsHttpsClient.h"

#include "Platform/IHttpsClient.h"
#include "WinString.h"

// NOLINTBEGIN(misc-include-cleaner) - windows.h must come before winhttp.h, which uses its types
// clang-format off
#include <windows.h>
#include <winhttp.h>
// clang-format on
// NOLINTEND(misc-include-cleaner)

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <limits>
#include <mutex>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>

namespace Platform
{

namespace WindowsHttpsDetail
{

std::string errorText(std::string_view step, std::uint32_t error)
{
    std::string_view meaning;
    switch (error)
    {
    case ERROR_WINHTTP_TIMEOUT:
        meaning = "the request timed out";
        break;
    case ERROR_WINHTTP_NAME_NOT_RESOLVED:
        meaning = "the server's name couldn't be resolved";
        break;
    case ERROR_WINHTTP_CANNOT_CONNECT:
        meaning = "couldn't connect to the server";
        break;
    case ERROR_WINHTTP_CONNECTION_ERROR:
        meaning = "the connection was reset";
        break;
    case ERROR_WINHTTP_SECURE_FAILURE:
        meaning = "the server's certificate or TLS was refused";
        break;
    case ERROR_WINHTTP_OPERATION_CANCELLED:
    case ERROR_INVALID_HANDLE:
        meaning = "the request was cancelled";
        break;
    default:
        meaning = "WinHTTP failed";
        break;
    }
    return std::format("{}: {} ({})", step, meaning, error);
}

} // namespace WindowsHttpsDetail

namespace
{

/// A session or connection handle, closed when it goes.
class InternetHandle
{
  public:
    explicit InternetHandle(HINTERNET handle) noexcept : m_Handle(handle)
    {}
    ~InternetHandle()
    {
        if (m_Handle != nullptr)
        {
            ::WinHttpCloseHandle(m_Handle);
        }
    }
    InternetHandle(const InternetHandle&) = delete;
    InternetHandle& operator=(const InternetHandle&) = delete;
    InternetHandle(InternetHandle&&) = delete;
    InternetHandle& operator=(InternetHandle&&) = delete;

    [[nodiscard]] HINTERNET get() const noexcept
    {
        return m_Handle;
    }

  private:
    HINTERNET m_Handle;
};

/// The request handle, which a stop may close from another thread: closing it ends a WinHTTP call
/// blocked on it (ERROR_WINHTTP_OPERATION_CANCELLED), so a stop doesn't wait out the timeouts. The
/// worker reads the handle for each call; once closed it is null, and a call already given the old value
/// fails with ERROR_INVALID_HANDLE (only this worker opens WinHTTP handles, so the value isn't reused).
class CancellableRequest
{
  public:
    explicit CancellableRequest(HINTERNET handle) noexcept : m_Handle(handle)
    {}
    ~CancellableRequest()
    {
        cancel();
    }
    CancellableRequest(const CancellableRequest&) = delete;
    CancellableRequest& operator=(const CancellableRequest&) = delete;
    CancellableRequest(CancellableRequest&&) = delete;
    CancellableRequest& operator=(CancellableRequest&&) = delete;

    [[nodiscard]] HINTERNET get() const
    {
        const std::scoped_lock lock(m_Mutex);
        return m_Handle;
    }

    /// Closes the handle, once.
    void cancel() noexcept
    {
        HINTERNET handle = nullptr;
        {
            const std::scoped_lock lock(m_Mutex);
            handle = std::exchange(m_Handle, nullptr);
        }
        if (handle != nullptr)
        {
            ::WinHttpCloseHandle(handle);
        }
    }

  private:
    mutable std::mutex m_Mutex;
    HINTERNET m_Handle;
};

/// What is left of the request's time, as WinHTTP's millisecond timeout: at least 1 ms.
[[nodiscard]] int remainingMs(std::chrono::steady_clock::time_point deadline)
{
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
    return static_cast<int>(std::clamp<std::int64_t>(left, 1, std::numeric_limits<int>::max()));
}

[[nodiscard]] std::unexpected<std::string> lastError(std::string_view step)
{
    return std::unexpected(WindowsHttpsDetail::errorText(step, ::GetLastError()));
}

} // namespace

std::expected<HttpsResponse, std::string> WindowsHttpsClient::get(const HttpsRequest& request, const std::stop_token& stop)
{
    if (stop.stop_requested())
    {
        return std::unexpected(std::string("Stopped before the request was sent"));
    }
    const std::wstring host = WinString::utf8ToWide(request.host);
    const std::wstring path = WinString::utf8ToWide(request.path);
    const std::wstring agent = WinString::utf8ToWide(request.userAgent);
    if (host.empty() || path.empty() || agent.empty())
    {
        return std::unexpected(std::string("The request's host, path or User-Agent is empty or isn't UTF-8"));
    }

    // LCOV_EXCL_START - a real HTTPS request; no test may make one (#3093)
    const auto deadline = std::chrono::steady_clock::now() + request.timeout;
    const InternetHandle session(
        ::WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (session.get() == nullptr)
    {
        return lastError("WinHttpOpen");
    }
    const InternetHandle connection(::WinHttpConnect(session.get(), host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0));
    if (connection.get() == nullptr)
    {
        return lastError("WinHttpConnect");
    }
    CancellableRequest handle(::WinHttpOpenRequest(
        connection.get(), L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
    if (handle.get() == nullptr)
    {
        return lastError("WinHttpOpenRequest");
    }
    // After the handle, so it is gone (and a running callback finished) before the handle closes.
    const std::stop_callback cancelOnStop(stop, [&handle] { handle.cancel(); });

    // Nothing of the user's goes with the request: no cookies, no Windows credentials.
    DWORD disabled = WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_AUTHENTICATION;
    if (::WinHttpSetOption(handle.get(), WINHTTP_OPTION_DISABLE_FEATURE, &disabled, sizeof(disabled)) == FALSE)
    {
        return lastError("WinHttpSetOption");
    }

    // Each step may take only what is left of the request's time.
    const auto limitTo = [&handle](int ms)
    {
        return ::WinHttpSetTimeouts(handle.get(), ms, ms, ms, ms) != FALSE;
    };
    if (!limitTo(remainingMs(deadline)))
    {
        return lastError("WinHttpSetTimeouts");
    }
    std::wstring headers;
    if (!request.accept.empty())
    {
        headers = L"Accept: " + WinString::utf8ToWide(request.accept);
    }
    if (::WinHttpSendRequest(handle.get(),
                             headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                             headers.empty() ? 0 : static_cast<DWORD>(headers.size()),
                             WINHTTP_NO_REQUEST_DATA,
                             0,
                             0,
                             0) == FALSE)
    {
        return lastError("WinHttpSendRequest");
    }
    if (!limitTo(remainingMs(deadline)) || ::WinHttpReceiveResponse(handle.get(), nullptr) == FALSE)
    {
        return lastError("WinHttpReceiveResponse");
    }

    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    if (::WinHttpQueryHeaders(handle.get(),
                              WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                              WINHTTP_HEADER_NAME_BY_INDEX,
                              &status,
                              &statusSize,
                              WINHTTP_NO_HEADER_INDEX) == FALSE)
    {
        return lastError("WinHttpQueryHeaders");
    }

    HttpsResponse response;
    response.status = static_cast<int>(status);
    while (true)
    {
        if (!limitTo(remainingMs(deadline)))
        {
            return lastError("WinHttpSetTimeouts");
        }
        DWORD available = 0;
        if (::WinHttpQueryDataAvailable(handle.get(), &available) == FALSE)
        {
            return lastError("WinHttpQueryDataAvailable");
        }
        if (available == 0)
        {
            break; // the whole body
        }
        const std::size_t had = response.body.size();
        if (available > request.maxBodyBytes - std::min(had, request.maxBodyBytes))
        {
            return std::unexpected(std::format("The reply is longer than {} bytes", request.maxBodyBytes));
        }
        response.body.resize(had + available);
        DWORD read = 0;
        if (::WinHttpReadData(handle.get(), response.body.data() + had, available, &read) == FALSE)
        {
            return lastError("WinHttpReadData");
        }
        response.body.resize(had + read);
        if (read == 0)
        {
            break;
        }
    }
    if (stop.stop_requested())
    {
        return std::unexpected(std::string("Stopped"));
    }
    return response;
    // LCOV_EXCL_STOP
}

} // namespace Platform
