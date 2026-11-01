#include "WindowsHttpsClient.h"

#include "Platform/IHttpsClient.h"
#include "WinString.h"

// NOLINTBEGIN(misc-include-cleaner) - windows.h must come before winhttp.h, which uses its types
// clang-format off
#include <windows.h>
#include <winhttp.h>
// clang-format on
// NOLINTEND(misc-include-cleaner)

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <limits>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <string_view>

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

void Completion::begin()
{
    const std::scoped_lock lock(m_Mutex);
    m_Completed = false;
    m_Error = 0;
    m_Bytes = 0;
}

void Completion::complete(std::uint32_t error, std::uint32_t bytes)
{
    // Notified under the lock: once the waiter sees the change it may destroy this.
    const std::scoped_lock lock(m_Mutex);
    m_Completed = true;
    m_Error = error;
    m_Bytes = bytes;
    m_Changed.notify_all();
}

void Completion::handleClosed()
{
    const std::scoped_lock lock(m_Mutex);
    m_Closed = true;
    m_Changed.notify_all();
}

Completion::Wait Completion::wait(const std::stop_token& stop, std::chrono::steady_clock::time_point deadline)
{
    std::unique_lock lock(m_Mutex);
    const bool completed = m_Changed.wait_until(lock, stop, deadline, [this] { return m_Completed; });
    // Checked before the completion is taken: one that came inline, or before the wait, doesn't carry a
    // stopped or expired request on to its next step.
    if (stop.stop_requested())
    {
        return Wait::Stopped;
    }
    if (!completed || std::chrono::steady_clock::now() >= deadline)
    {
        return Wait::TimedOut;
    }
    return Wait::Completed;
}

void Completion::waitForClose()
{
    std::unique_lock lock(m_Mutex);
    m_Changed.wait(lock, [this] { return m_Closed; });
}

std::uint32_t Completion::error() const
{
    const std::scoped_lock lock(m_Mutex);
    return m_Error;
}

std::uint32_t Completion::bytes() const
{
    const std::scoped_lock lock(m_Mutex);
    return m_Bytes;
}

} // namespace WindowsHttpsDetail

namespace
{

using WindowsHttpsDetail::WinHttpFunctions;

[[nodiscard]] std::unexpected<std::string> lastError(std::string_view step)
{
    return std::unexpected(WindowsHttpsDetail::errorText(step, ::GetLastError()));
}

/// A session or connection handle, closed when it goes.
class InternetHandle
{
  public:
    InternetHandle(const WinHttpFunctions& fns, HINTERNET handle) noexcept : m_Fns(&fns), m_Handle(handle)
    {}
    ~InternetHandle()
    {
        if (m_Handle != nullptr)
        {
            m_Fns->closeHandle(m_Handle);
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
    const WinHttpFunctions* m_Fns;
    HINTERNET m_Handle;
};

/// What WinHTTP reaches through the request's callback context: the completion it signals and the buffer
/// a read fills. On the heap, so that if the handle can't be closed it can be left to WinHTTP (#3333).
struct RequestState
{
    WindowsHttpsDetail::Completion completion;
    std::string readBuffer;
};

void CALLBACK onStatus(HINTERNET /*handle*/, DWORD_PTR context, DWORD status, LPVOID info, DWORD infoLength)
{
    // NOLINTNEXTLINE(performance-no-int-to-ptr, cppcoreguidelines-pro-type-reinterpret-cast) - the context AsyncRequest bound
    auto* const state = reinterpret_cast<RequestState*>(context);
    if (state == nullptr)
    {
        return;
    }
    WindowsHttpsDetail::Completion& completion = state->completion;
    switch (status)
    {
    case WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE:
    case WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE:
        completion.complete(0, 0);
        break;
    case WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE:
        completion.complete(0, *static_cast<const DWORD*>(info));
        break;
    case WINHTTP_CALLBACK_STATUS_READ_COMPLETE:
        completion.complete(0, infoLength);
        break;
    case WINHTTP_CALLBACK_STATUS_REQUEST_ERROR:
    {
        const DWORD error = static_cast<const WINHTTP_ASYNC_RESULT*>(info)->dwError;
        completion.complete(error != 0 ? error : ERROR_WINHTTP_INTERNAL_ERROR, 0);
        break;
    }
    case WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING:
        completion.handleClosed();
        break;
    default:
        break;
    }
}

/// The request handle in WinHTTP's asynchronous mode (#3333). Only the worker calls WinHTTP with it or
/// closes it: a stop or the deadline only wakes the worker's wait, and the worker then closes the handle,
/// which ends the call in flight. WinHttpCloseHandle's documentation allows that for an asynchronous request
/// and forbids it for a synchronous one
/// (https://learn.microsoft.com/windows/win32/api/winhttp/nf-winhttp-winhttpclosehandle). After the close
/// it waits for WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING, the handle's last callback, so the callback's
/// context and the buffer a read was given outlive WinHTTP's use of them.
class AsyncRequest
{
  public:
    /// Opens the request on @p connection. The state is made first, so if that throws nothing is open yet.
    AsyncRequest(const WinHttpFunctions& fns, HINTERNET connection, LPCWSTR path)
        : m_Fns(&fns),
          m_State(std::make_unique<RequestState>()),
          m_Handle(
              fns.openRequest(connection, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE))
    {}
    ~AsyncRequest()
    {
        close();
    }
    AsyncRequest(const AsyncRequest&) = delete;
    AsyncRequest& operator=(const AsyncRequest&) = delete;
    AsyncRequest(AsyncRequest&&) = delete;
    AsyncRequest& operator=(AsyncRequest&&) = delete;

    [[nodiscard]] HINTERNET get() const noexcept
    {
        return m_Handle;
    }

    /// What WinHTTP hands the callback as its context: the state.
    [[nodiscard]] DWORD_PTR context() const noexcept
    {
        return reinterpret_cast<DWORD_PTR>(
            m_State.get()); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast) - WinHTTP's context is an integer
    }

    /// Binds the state as the handle's context and installs the callback. @return false, with GetLastError()
    /// set, if either fails: then no callback comes and close() doesn't wait for one.
    [[nodiscard]] bool attach() noexcept
    {
        DWORD_PTR value = context();
        if (m_Fns->setOption(m_Handle, WINHTTP_OPTION_CONTEXT_VALUE, &value, sizeof(value)) == FALSE)
        {
            return false;
        }
        if (m_Fns->setStatusCallback(m_Handle, &onStatus, WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS | WINHTTP_CALLBACK_FLAG_HANDLES, 0) ==
            WINHTTP_INVALID_STATUS_CALLBACK)
        {
            return false;
        }
        m_CallbackSet = true;
        return true;
    }

    /// Before each asynchronous call.
    void begin()
    {
        m_State->completion.begin();
    }

    /// Waits for the call @p step just started. A stop or the deadline closes the handle, ending the call,
    /// and returns only once WinHTTP has let go of it. @return what the call counted (bytes), or why it failed.
    [[nodiscard]] std::expected<DWORD, std::string>
    wait(std::string_view step, const std::stop_token& stop, std::chrono::steady_clock::time_point deadline)
    {
        WindowsHttpsDetail::Completion& completion = m_State->completion;
        switch (completion.wait(stop, deadline))
        {
        case WindowsHttpsDetail::Completion::Wait::Completed:
            break;
        case WindowsHttpsDetail::Completion::Wait::Stopped:
            close();
            return std::unexpected(std::string("Stopped"));
        case WindowsHttpsDetail::Completion::Wait::TimedOut:
            close();
            return std::unexpected(WindowsHttpsDetail::errorText(step, ERROR_WINHTTP_TIMEOUT));
        }
        if (const std::uint32_t error = completion.error(); error != 0)
        {
            return std::unexpected(WindowsHttpsDetail::errorText(step, error));
        }
        return completion.bytes();
    }

    /// Reads up to @p available bytes into the state's buffer. @return what was read (empty at the end),
    /// valid until the next read, or why there is nothing.
    [[nodiscard]] std::expected<std::string_view, std::string>
    read(DWORD available, const std::stop_token& stop, std::chrono::steady_clock::time_point deadline)
    {
        std::string& buffer = m_State->readBuffer;
        buffer.resize(available);
        begin();
        // Asynchronous: the count comes to the callback, not through the out-parameter (null).
        if (m_Fns->readData(m_Handle, buffer.data(), available, nullptr) == FALSE)
        {
            return lastError("WinHttpReadData");
        }
        const auto read = wait("WinHttpReadData", stop, deadline);
        if (!read.has_value())
        {
            return std::unexpected(read.error());
        }
        return std::string_view(m_State->readBuffer).substr(0, std::min<std::size_t>(*read, available));
    }

    /// Closes the handle, once. Once the close succeeds, waits for the handle's last callback if one was
    /// installed, so the state can go.
    void close() noexcept
    {
        if (m_Handle == nullptr)
        {
            return;
        }
        if (m_Fns->closeHandle(m_Handle) == FALSE)
        {
            // Never expected. The handle may still be live, so WinHTTP may still call back with the state or
            // write into its read buffer, and HANDLE_CLOSING may never come: waiting could hang shutdown,
            // and freeing could be a use-after-free. The state is left to WinHTTP instead, never freed: a
            // bounded leak on a path that shouldn't happen, rather than either.
            const DWORD error = ::GetLastError();
            m_Handle = nullptr;
            const RequestState* const leaked = m_State.release(); // NOLINT(clang-analyzer-cplusplus.NewDeleteLeaks) - deliberately, above
            static_cast<void>(leaked);
            try
            {
                spdlog::warn("Update check: WinHttpCloseHandle failed ({}); its request state is left allocated", error);
            }
            catch (...) // NOLINT(bugprone-empty-catch) - the log is all this path does; nothing to do if it fails
            {}
            return;
        }
        m_Handle = nullptr;
        if (m_CallbackSet)
        {
            m_State->completion.waitForClose();
        }
    }

  private:
    // In this order: the state is made before the handle is opened, and outlives it.
    const WinHttpFunctions* m_Fns;
    std::unique_ptr<RequestState> m_State;
    HINTERNET m_Handle;
    bool m_CallbackSet = false;
};

/// What is left of the request's time, as WinHTTP's millisecond timeout: at least 1 ms.
[[nodiscard]] int remainingMs(std::chrono::steady_clock::time_point deadline)
{
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
    return static_cast<int>(std::clamp<std::int64_t>(left, 1, std::numeric_limits<int>::max()));
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

    const auto deadline = std::chrono::steady_clock::now() + request.timeout;
    const InternetHandle session(
        m_Fns,
        m_Fns.open(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC));
    if (session.get() == nullptr)
    {
        return lastError("WinHttpOpen");
    }
    const InternetHandle connection(m_Fns, m_Fns.connect(session.get(), host.c_str(), m_Port, 0));
    if (connection.get() == nullptr)
    {
        return lastError("WinHttpConnect");
    }
    // After the session and connection, so it closes (and its last callback has come) before they do.
    AsyncRequest handle(m_Fns, connection.get(), path.c_str());
    if (handle.get() == nullptr)
    {
        return lastError("WinHttpOpenRequest");
    }
    if (!handle.attach())
    {
        return lastError("WinHttpSetStatusCallback");
    }

    // Nothing of the user's goes with the request: no cookies, no Windows credentials.
    DWORD disabled = WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_AUTHENTICATION;
    if (m_Fns.setOption(handle.get(), WINHTTP_OPTION_DISABLE_FEATURE, &disabled, sizeof(disabled)) == FALSE)
    {
        return lastError("WinHttpSetOption");
    }
    // WinHTTP's own timeouts, a backstop to the deadline each wait below keeps.
    if (const int ms = remainingMs(deadline); m_Fns.setTimeouts(handle.get(), ms, ms, ms, ms) == FALSE)
    {
        return lastError("WinHttpSetTimeouts");
    }

    std::wstring headers;
    if (!request.accept.empty())
    {
        headers = L"Accept: " + WinString::utf8ToWide(request.accept);
    }
    handle.begin();
    if (m_Fns.sendRequest(handle.get(),
                          headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                          headers.empty() ? 0 : static_cast<DWORD>(headers.size()),
                          WINHTTP_NO_REQUEST_DATA,
                          0,
                          0,
                          handle.context()) == FALSE)
    {
        return lastError("WinHttpSendRequest");
    }
    if (const auto sent = handle.wait("WinHttpSendRequest", stop, deadline); !sent.has_value())
    {
        return std::unexpected(sent.error());
    }
    handle.begin();
    if (m_Fns.receiveResponse(handle.get(), nullptr) == FALSE)
    {
        return lastError("WinHttpReceiveResponse");
    }
    if (const auto received = handle.wait("WinHttpReceiveResponse", stop, deadline); !received.has_value())
    {
        return std::unexpected(received.error());
    }

    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    if (m_Fns.queryHeaders(handle.get(),
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
        // Asynchronous: the count comes to the callback, not through the out-parameter (null).
        handle.begin();
        if (m_Fns.queryDataAvailable(handle.get(), nullptr) == FALSE)
        {
            return lastError("WinHttpQueryDataAvailable");
        }
        const auto available = handle.wait("WinHttpQueryDataAvailable", stop, deadline);
        if (!available.has_value())
        {
            return std::unexpected(available.error());
        }
        if (*available == 0)
        {
            break; // the whole body
        }
        const std::size_t had = response.body.size();
        if (*available > request.maxBodyBytes - std::min(had, request.maxBodyBytes))
        {
            return std::unexpected(std::format("The reply is longer than {} bytes", request.maxBodyBytes));
        }
        const auto read = handle.read(*available, stop, deadline);
        if (!read.has_value())
        {
            return std::unexpected(read.error());
        }
        if (read->empty())
        {
            break;
        }
        response.body.append(*read);
    }
    handle.close();
    if (stop.stop_requested())
    {
        return std::unexpected(std::string("Stopped"));
    }
    return response;
}

} // namespace Platform
