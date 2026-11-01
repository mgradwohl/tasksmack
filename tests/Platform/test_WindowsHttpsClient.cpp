/// @file test_WindowsHttpsClient.cpp
/// @brief The update check's WinHTTP client (#3093), without the network: what it refuses before it opens
/// a session (a stop already requested, a request with no host, path or User-Agent), the factory, the
/// wording of its errors, and how an asynchronous call's completion is waited for (#3333). No test here sends
/// a request.

#include "Platform/Factory.h"
#include "Platform/IHttpsClient.h"
#include "Platform/Windows/WindowsHttpsClient.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <expected>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>

#include <windows.h>
#include <winhttp.h>

namespace Platform
{
namespace
{

HttpsRequest validRequest()
{
    return HttpsRequest{.host = "api.github.com",
                        .path = "/repos/mgradwohl/tasksmack/releases/latest",
                        .userAgent = "TaskSmack/0.0.0",
                        .accept = "application/vnd.github+json",
                        .timeout = std::chrono::milliseconds(10'000),
                        .maxBodyBytes = 1024};
}

TEST(WindowsHttpsClientTest, AStopAlreadyRequestedSendsNothing)
{
    WindowsHttpsClient client;
    std::stop_source stop;
    static_cast<void>(stop.request_stop());
    const auto response = client.get(validRequest(), stop.get_token());
    ASSERT_FALSE(response.has_value());
    EXPECT_EQ(response.error(), "Stopped before the request was sent");
}

TEST(WindowsHttpsClientTest, ARequestMissingItsHostPathOrUserAgentIsRefusedBeforeSending)
{
    WindowsHttpsClient client;
    const std::stop_source stop;
    for (std::string HttpsRequest::* const field : {&HttpsRequest::host, &HttpsRequest::path, &HttpsRequest::userAgent})
    {
        HttpsRequest request = validRequest();
        (request.*field).clear();
        const auto response = client.get(request, stop.get_token());
        ASSERT_FALSE(response.has_value());
        EXPECT_TRUE(response.error().contains("empty")) << response.error();
    }
}

TEST(WindowsHttpsClientTest, TheFactoryMakesTheWinHttpClient)
{
    const std::shared_ptr<IHttpsClient> client = makeHttpsClient();
    EXPECT_NE(dynamic_cast<WindowsHttpsClient*>(client.get()), nullptr);
}

TEST(WindowsHttpsClientTest, ErrorsSayWhatFailedForTheLog)
{
    EXPECT_EQ(WindowsHttpsDetail::errorText("WinHttpSendRequest", ERROR_WINHTTP_TIMEOUT),
              "WinHttpSendRequest: the request timed out (12002)");
    EXPECT_TRUE(WindowsHttpsDetail::errorText("x", ERROR_WINHTTP_NAME_NOT_RESOLVED).contains("couldn't be resolved"));
    EXPECT_TRUE(WindowsHttpsDetail::errorText("x", ERROR_WINHTTP_CANNOT_CONNECT).contains("couldn't connect"));
    EXPECT_TRUE(WindowsHttpsDetail::errorText("x", ERROR_WINHTTP_CONNECTION_ERROR).contains("reset"));
    EXPECT_TRUE(WindowsHttpsDetail::errorText("x", ERROR_WINHTTP_SECURE_FAILURE).contains("TLS"));
    EXPECT_TRUE(WindowsHttpsDetail::errorText("x", ERROR_WINHTTP_OPERATION_CANCELLED).contains("cancelled"));
    EXPECT_TRUE(WindowsHttpsDetail::errorText("x", ERROR_INVALID_HANDLE).contains("cancelled"));
    EXPECT_EQ(WindowsHttpsDetail::errorText("WinHttpOpen", 5), "WinHttpOpen: WinHTTP failed (5)");
}

// The asynchronous call's completion (#3333): what the status callback reports, from another thread, to the
// worker waiting for it. No WinHTTP handle is involved.
using WindowsHttpsDetail::Completion;

constexpr auto FAR_DEADLINE = std::chrono::hours(1);

TEST(WindowsHttpsCompletionTest, ACompletionFromAnotherThreadEndsTheWaitWithItsCount)
{
    Completion completion;
    completion.begin();
    const std::jthread callback([&completion] { completion.complete(0, 42); });
    const std::stop_source stop;
    EXPECT_EQ(completion.wait(stop.get_token(), std::chrono::steady_clock::now() + FAR_DEADLINE), Completion::Wait::Completed);
    EXPECT_EQ(completion.error(), 0U);
    EXPECT_EQ(completion.bytes(), 42U);
}

TEST(WindowsHttpsCompletionTest, AnErrorTheCallbackReportsIsKept)
{
    Completion completion;
    completion.begin();
    completion.complete(ERROR_WINHTTP_CANNOT_CONNECT, 0);
    const std::stop_source stop;
    EXPECT_EQ(completion.wait(stop.get_token(), std::chrono::steady_clock::now() + FAR_DEADLINE), Completion::Wait::Completed);
    EXPECT_EQ(completion.error(), static_cast<std::uint32_t>(ERROR_WINHTTP_CANNOT_CONNECT));
}

TEST(WindowsHttpsCompletionTest, BeginForgetsTheLastCallsCompletion)
{
    Completion completion;
    completion.begin();
    completion.complete(ERROR_WINHTTP_TIMEOUT, 7);
    completion.begin();
    EXPECT_EQ(completion.error(), 0U);
    EXPECT_EQ(completion.bytes(), 0U);
    const std::stop_source stop;
    EXPECT_EQ(completion.wait(stop.get_token(), std::chrono::steady_clock::now()), Completion::Wait::TimedOut);
}

TEST(WindowsHttpsCompletionTest, AStopFromAnotherThreadEndsTheWaitPromptly)
{
    Completion completion;
    completion.begin();
    std::stop_source stop;
    const auto started = std::chrono::steady_clock::now();
    const std::jthread stopper([&stop] { static_cast<void>(stop.request_stop()); });
    EXPECT_EQ(completion.wait(stop.get_token(), started + FAR_DEADLINE), Completion::Wait::Stopped);
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::minutes(1));
}

TEST(WindowsHttpsCompletionTest, TheDeadlinePassingEndsTheWait)
{
    Completion completion;
    completion.begin();
    const std::stop_source stop;
    EXPECT_EQ(completion.wait(stop.get_token(), std::chrono::steady_clock::now() + std::chrono::milliseconds(10)),
              Completion::Wait::TimedOut);
}

TEST(WindowsHttpsCompletionTest, WaitForCloseReturnsOnceTheHandleClosingCallbackCame)
{
    Completion completion;
    const std::jthread callback([&completion] { completion.handleClosed(); });
    completion.waitForClose();
    SUCCEED();
}

TEST(WindowsHttpsCompletionTest, AStopWinsOverACompletionThatCameFirst)
{
    Completion completion;
    completion.begin();
    completion.complete(0, 5);
    std::stop_source stop;
    static_cast<void>(stop.request_stop());
    EXPECT_EQ(completion.wait(stop.get_token(), std::chrono::steady_clock::now() + FAR_DEADLINE), Completion::Wait::Stopped);
}

TEST(WindowsHttpsCompletionTest, APassedDeadlineWinsOverACompletionThatCameFirst)
{
    Completion completion;
    completion.begin();
    completion.complete(0, 5);
    const std::stop_source stop;
    EXPECT_EQ(completion.wait(stop.get_token(), std::chrono::steady_clock::now() - std::chrono::seconds(1)), Completion::Wait::TimedOut);
}

// The whole request through fake WinHTTP calls (#3333): no network. Each fake call completes inline through
// the status callback, as WinHTTP may, except the one step a test leaves pending, as a call still waiting on
// the network would be.

enum class Step : std::uint8_t
{
    None,
    Send,
    Receive,
    QueryData,
    Read,
};

/// What the fakes answer, and what they were asked.
struct FakeHttp // NOLINT(clang-analyzer-optin.performance.Padding)
{
    std::mutex mutex;
    std::condition_variable changed;
    // Answers
    Step pendingAt = Step::None; ///< The step that gets no completion until a test delivers one
    bool closeFails = false;     ///< WinHttpCloseHandle on the request answers FALSE
    bool closingOnClose = true;  ///< WinHttpCloseHandle on the request sends HANDLE_CLOSING inline
    std::string body = R"({"tag_name":"v1.2.3"})";
    // Seen
    DWORD openFlags = 0;
    DWORD_PTR context = 0;
    WINHTTP_STATUS_CALLBACK callback = nullptr;
    DWORD callbackFlags = 0;
    Step reached = Step::None;
    bool bodyServed = false;
    void* readBuffer = nullptr;
    DWORD readLength = 0;
    int requestCloses = 0;
    std::thread::id closeThread;

    template<typename Predicate> void waitUntil(Predicate predicate)
    {
        std::unique_lock lock(mutex);
        changed.wait(lock, predicate);
    }

    /// Calls the request's callback as WinHTTP would, through the context it was given.
    void deliver(DWORD status, LPVOID info, DWORD length);
};

FakeHttp*& currentFake()
{
    static FakeHttp* fake = nullptr;
    return fake;
}

FakeHttp& fakeHttp()
{
    return *currentFake();
}

// A non-null stand-in for each handle.
HINTERNET sessionHandle()
{
    static int token = 0;
    return &token;
}

HINTERNET connectionHandle()
{
    static int token = 0;
    return &token;
}

HINTERNET requestHandle()
{
    static int token = 0;
    return &token;
}

void FakeHttp::deliver(DWORD status, LPVOID info, DWORD length)
{
    WINHTTP_STATUS_CALLBACK target = nullptr;
    DWORD_PTR value = 0;
    {
        const std::scoped_lock lock(mutex);
        target = callback;
        value = context;
    }
    ASSERT_NE(target, nullptr);
    target(requestHandle(), value, status, info, length);
}

/// Records @p step and, unless it is the pending one, completes it with @p status.
void reach(Step step, DWORD status, LPVOID info, DWORD length)
{
    FakeHttp& fake = fakeHttp();
    bool pending = false;
    {
        const std::scoped_lock lock(fake.mutex);
        fake.reached = step;
        pending = fake.pendingAt == step;
    }
    fake.changed.notify_all();
    if (!pending)
    {
        fake.deliver(status, info, length);
    }
}

HINTERNET WINAPI fakeOpen(LPCWSTR /*agent*/, DWORD /*access*/, LPCWSTR /*proxy*/, LPCWSTR /*bypass*/, DWORD flags)
{
    const std::scoped_lock lock(fakeHttp().mutex);
    fakeHttp().openFlags = flags;
    return sessionHandle();
}

HINTERNET WINAPI fakeConnect(HINTERNET /*session*/, LPCWSTR /*host*/, INTERNET_PORT /*port*/, DWORD /*reserved*/)
{
    return connectionHandle();
}

HINTERNET WINAPI fakeOpenRequest(HINTERNET /*connection*/,
                                 LPCWSTR /*verb*/,
                                 LPCWSTR /*path*/,
                                 LPCWSTR /*version*/,
                                 LPCWSTR /*referrer*/,
                                 LPCWSTR* /*acceptTypes*/,
                                 DWORD /*flags*/)
{
    return requestHandle();
}

BOOL WINAPI fakeSetOption(HINTERNET /*handle*/, DWORD option, LPVOID buffer, DWORD /*length*/)
{
    if (option == WINHTTP_OPTION_CONTEXT_VALUE)
    {
        const std::scoped_lock lock(fakeHttp().mutex);
        fakeHttp().context = *static_cast<const DWORD_PTR*>(buffer);
    }
    return TRUE;
}

WINHTTP_STATUS_CALLBACK WINAPI fakeSetStatusCallback(HINTERNET /*handle*/,
                                                     WINHTTP_STATUS_CALLBACK callback,
                                                     DWORD flags,
                                                     DWORD_PTR /*reserved*/)
{
    const std::scoped_lock lock(fakeHttp().mutex);
    fakeHttp().callback = callback;
    fakeHttp().callbackFlags = flags;
    return nullptr;
}

BOOL WINAPI fakeSetTimeouts(HINTERNET /*handle*/, int /*resolve*/, int /*connect*/, int /*send*/, int /*receive*/)
{
    return TRUE;
}

BOOL WINAPI fakeSendRequest(HINTERNET /*handle*/,
                            LPCWSTR /*headers*/,
                            DWORD /*headersLength*/,
                            LPVOID /*optional*/,
                            DWORD /*optionalLength*/,
                            DWORD /*totalLength*/,
                            DWORD_PTR /*context*/)
{
    reach(Step::Send, WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE, nullptr, 0);
    return TRUE;
}

BOOL WINAPI fakeReceiveResponse(HINTERNET /*handle*/, LPVOID /*reserved*/)
{
    reach(Step::Receive, WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE, nullptr, 0);
    return TRUE;
}

BOOL WINAPI fakeQueryHeaders(HINTERNET /*handle*/, DWORD /*level*/, LPCWSTR /*name*/, LPVOID buffer, LPDWORD length, LPDWORD /*index*/)
{
    *static_cast<DWORD*>(buffer) = 200;
    *length = sizeof(DWORD);
    return TRUE;
}

BOOL WINAPI fakeQueryDataAvailable(HINTERNET /*handle*/, LPDWORD available)
{
    EXPECT_EQ(available, nullptr); // asynchronous: the count comes to the callback
    DWORD count = 0;
    {
        const std::scoped_lock lock(fakeHttp().mutex);
        count = fakeHttp().bodyServed ? 0 : static_cast<DWORD>(fakeHttp().body.size());
    }
    reach(Step::QueryData, WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE, &count, sizeof(count));
    return TRUE;
}

BOOL WINAPI fakeReadData(HINTERNET /*handle*/, LPVOID buffer, DWORD length, LPDWORD read)
{
    EXPECT_EQ(read, nullptr); // asynchronous: the count comes to the callback
    FakeHttp& fake = fakeHttp();
    DWORD copied = 0;
    {
        const std::scoped_lock lock(fake.mutex);
        fake.readBuffer = buffer;
        fake.readLength = length;
        if (fake.pendingAt != Step::Read)
        {
            copied = std::min(length, static_cast<DWORD>(fake.body.size()));
            std::memcpy(buffer, fake.body.data(), copied);
            fake.bodyServed = true;
        }
    }
    reach(Step::Read, WINHTTP_CALLBACK_STATUS_READ_COMPLETE, buffer, copied);
    return TRUE;
}

BOOL WINAPI fakeCloseHandle(HINTERNET handle)
{
    if (handle != requestHandle())
    {
        return TRUE;
    }
    FakeHttp& fake = fakeHttp();
    bool fails = false;
    bool closing = false;
    {
        const std::scoped_lock lock(fake.mutex);
        ++fake.requestCloses;
        fake.closeThread = std::this_thread::get_id();
        fails = fake.closeFails;
        closing = fake.closingOnClose && !fails;
    }
    fake.changed.notify_all();
    if (fails)
    {
        ::SetLastError(ERROR_WINHTTP_INTERNAL_ERROR);
        return FALSE;
    }
    if (closing)
    {
        HINTERNET closed = requestHandle();
        fake.deliver(WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING, static_cast<LPVOID>(&closed), sizeof(closed));
    }
    return TRUE;
}

WindowsHttpsDetail::WinHttpFunctions fakeFunctions()
{
    WindowsHttpsDetail::WinHttpFunctions fns;
    fns.open = &fakeOpen;
    fns.connect = &fakeConnect;
    fns.openRequest = &fakeOpenRequest;
    fns.setOption = &fakeSetOption;
    fns.setStatusCallback = &fakeSetStatusCallback;
    fns.setTimeouts = &fakeSetTimeouts;
    fns.sendRequest = &fakeSendRequest;
    fns.receiveResponse = &fakeReceiveResponse;
    fns.queryHeaders = &fakeQueryHeaders;
    fns.queryDataAvailable = &fakeQueryDataAvailable;
    fns.readData = &fakeReadData;
    fns.closeHandle = &fakeCloseHandle;
    return fns;
}

/// Installs a fresh FakeHttp for one test.
class FakeHttpScope
{
  public:
    FakeHttpScope()
    {
        currentFake() = &m_Fake;
    }
    ~FakeHttpScope()
    {
        currentFake() = nullptr;
    }
    FakeHttpScope(const FakeHttpScope&) = delete;
    FakeHttpScope& operator=(const FakeHttpScope&) = delete;
    FakeHttpScope(FakeHttpScope&&) = delete;
    FakeHttpScope& operator=(FakeHttpScope&&) = delete;

    [[nodiscard]] FakeHttp& fake() noexcept
    {
        return m_Fake;
    }

  private:
    FakeHttp m_Fake;
};

TEST(WindowsHttpsFakeRequestTest, ARequestRunsAsynchronouslyAndClosesItsHandleOnce)
{
    FakeHttpScope scope;
    WindowsHttpsClient client(fakeFunctions());
    const std::stop_source stop;
    const auto response = client.get(validRequest(), stop.get_token());
    ASSERT_TRUE(response.has_value()) << response.error();
    EXPECT_EQ(response->status, 200);
    EXPECT_EQ(response->body, scope.fake().body);
    EXPECT_NE(scope.fake().openFlags & WINHTTP_FLAG_ASYNC, 0U);
    EXPECT_NE(scope.fake().context, 0U);
    EXPECT_NE(scope.fake().callbackFlags & WINHTTP_CALLBACK_FLAG_HANDLES, 0U);
    EXPECT_EQ(scope.fake().requestCloses, 1);
    EXPECT_EQ(scope.fake().closeThread, std::this_thread::get_id());
}

TEST(WindowsHttpsFakeRequestTest, AStepThatNeverCompletesTimesOut)
{
    FakeHttpScope scope;
    scope.fake().pendingAt = Step::Receive;
    WindowsHttpsClient client(fakeFunctions());
    HttpsRequest request = validRequest();
    request.timeout = std::chrono::milliseconds(20);
    const std::stop_source stop;
    const auto response = client.get(request, stop.get_token());
    ASSERT_FALSE(response.has_value());
    EXPECT_TRUE(response.error().contains("timed out")) << response.error();
    EXPECT_EQ(scope.fake().requestCloses, 1);
}

/// Stops a request while @p step is pending, and checks the worker closes the request handle once, itself,
/// and holds the callback's context and the read buffer until HANDLE_CLOSING comes.
void stopDuring(Step step)
{
    FakeHttpScope scope;
    FakeHttp& fake = scope.fake();
    fake.pendingAt = step;
    fake.closingOnClose = false; // the test sends HANDLE_CLOSING, later
    WindowsHttpsClient client(fakeFunctions());
    std::stop_source stop;
    std::thread::id workerId;
    std::expected<HttpsResponse, std::string> result = std::unexpected(std::string("never ran"));
    std::atomic<bool> returned = false;
    std::jthread worker(
        [&]
        {
            workerId = std::this_thread::get_id();
            result = client.get(validRequest(), stop.get_token());
            returned = true;
        });
    fake.waitUntil([&fake, step] { return fake.reached == step; });
    static_cast<void>(stop.request_stop());
    fake.waitUntil([&fake] { return fake.requestCloses > 0; });

    // Closed, but WinHTTP may still call back or write into the read buffer: the worker waits for HANDLE_CLOSING.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_FALSE(returned.load());
    if (step == Step::Read)
    {
        ASSERT_NE(fake.readBuffer, nullptr);
        std::memset(fake.readBuffer, 'x', fake.readLength); // still the request's
    }
    WINHTTP_ASYNC_RESULT cancelled{.dwResult = 0, .dwError = ERROR_WINHTTP_OPERATION_CANCELLED};
    fake.deliver(WINHTTP_CALLBACK_STATUS_REQUEST_ERROR, &cancelled, sizeof(cancelled)); // through the live context
    HINTERNET closed = requestHandle();
    fake.deliver(WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING, static_cast<LPVOID>(&closed), sizeof(closed));
    worker.join();

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), "Stopped");
    EXPECT_NE(fake.openFlags & WINHTTP_FLAG_ASYNC, 0U);
    EXPECT_EQ(fake.requestCloses, 1);
    EXPECT_EQ(fake.closeThread, workerId);
}

TEST(WindowsHttpsFakeRequestTest, AStopDuringTheSendClosesOnTheWorkerAndWaitsForHandleClosing)
{
    stopDuring(Step::Send);
}

TEST(WindowsHttpsFakeRequestTest, AStopDuringTheResponseWaitClosesOnTheWorkerAndWaitsForHandleClosing)
{
    stopDuring(Step::Receive);
}

TEST(WindowsHttpsFakeRequestTest, AStopDuringAReadClosesOnTheWorkerAndKeepsTheBufferUntilHandleClosing)
{
    stopDuring(Step::Read);
}

TEST(WindowsHttpsFakeRequestTest, AFailedCloseNeitherWaitsNorFreesWhatWinHttpMayStillUse)
{
    FakeHttpScope scope;
    FakeHttp& fake = scope.fake();
    fake.pendingAt = Step::Read;
    fake.closeFails = true;
    WindowsHttpsClient client(fakeFunctions());
    std::stop_source stop;
    std::expected<HttpsResponse, std::string> result = std::unexpected(std::string("never ran"));
    std::jthread worker([&] { result = client.get(validRequest(), stop.get_token()); });
    fake.waitUntil([&fake] { return fake.reached == Step::Read; });
    static_cast<void>(stop.request_stop());
    worker.join(); // returns with no HANDLE_CLOSING: it doesn't wait for one that may never come

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), "Stopped");
    EXPECT_EQ(fake.requestCloses, 1);
    // The context and the read buffer were left allocated: a late callback and a late write still land.
    ASSERT_NE(fake.readBuffer, nullptr);
    std::memset(fake.readBuffer, 'x', fake.readLength);
    HINTERNET closed = requestHandle();
    fake.deliver(WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING, static_cast<LPVOID>(&closed), sizeof(closed));
}

} // namespace
} // namespace Platform
