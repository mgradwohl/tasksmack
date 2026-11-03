/// @file test_WindowsHttpsClient.cpp
/// @brief The update check's WinHTTP client (#3093), without the network: what it refuses before it opens
/// a session (a stop already requested, a request with no host, path or User-Agent), the factory, the
/// wording of its errors, how an asynchronous call's completion is waited for, a whole request through fake
/// WinHTTP calls (stopped while a call is still running, or unwound by a throw mid-flight, #3342), and real
/// WinHTTP stopped or timed out against a loopback listener that never answers (#3333). No test reaches past
/// 127.0.0.1.

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
#include <future>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// clang-format off
#include <winsock2.h>
#include <windows.h>
#include <winhttp.h>
// clang-format on

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
    /// The step whose call doesn't return until the test sets releaseCall: a call still running in WinHTTP
    /// (#3333, #3342).
    Step blockInCallAt = Step::None;
    bool releaseCall = false;
    /// The step whose call starts (the read is given its buffer) and then throws std::bad_alloc (#3342).
    Step throwAt = Step::None;
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
    bool inCall = false;      ///< A blocked call is running
    int closesDuringCall = 0; ///< WinHttpCloseHandle on the request while a call on it was still running
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

/// Records @p step; holds the call until released if it is the blocked one, throws if it is the throwing one,
/// and, unless it is the pending one, completes it with @p status.
void reach(Step step, DWORD status, LPVOID info, DWORD length)
{
    FakeHttp& fake = fakeHttp();
    bool pending = false;
    bool throws = false;
    {
        std::unique_lock lock(fake.mutex);
        fake.reached = step;
        pending = fake.pendingAt == step;
        throws = fake.throwAt == step;
        if (fake.blockInCallAt == step)
        {
            fake.inCall = true;
            fake.changed.notify_all();
            fake.changed.wait(lock, [&fake] { return fake.releaseCall; });
            fake.inCall = false;
        }
    }
    fake.changed.notify_all();
    if (throws)
    {
        throw std::bad_alloc{};
    }
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
        if (fake.inCall)
        {
            ++fake.closesDuringCall;
        }
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
    // Fatal: a get() that returned has freed the request state, which the writes and callbacks below would touch.
    ASSERT_FALSE(returned.load()) << "get() returned before WinHTTP let go of the handle";
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

/// Stops a request while the call at @p step is still running inside WinHTTP (#3333, #3342): it hasn't
/// returned yet. Nothing may close the handle under it; the worker closes it, once, after the call returns.
void stopInsideCall(Step step)
{
    FakeHttpScope scope;
    FakeHttp& fake = scope.fake();
    fake.blockInCallAt = step;
    fake.pendingAt = step; // and no completion after it returns: only the stop ends the wait
    WindowsHttpsClient client(fakeFunctions());
    std::stop_source stop;
    std::thread::id workerId;
    std::expected<HttpsResponse, std::string> result = std::unexpected(std::string("never ran"));
    std::jthread worker(
        [&]
        {
            workerId = std::this_thread::get_id();
            result = client.get(validRequest(), stop.get_token());
        });
    fake.waitUntil([&fake, step] { return fake.reached == step && fake.inCall; });
    static_cast<void>(stop.request_stop());
    std::this_thread::sleep_for(std::chrono::milliseconds(50)); // time for a close from the stopping side
    {
        const std::scoped_lock lock(fake.mutex);
        EXPECT_EQ(fake.requestCloses, 0) << "closed while the call was still using the handle";
        fake.releaseCall = true;
    }
    fake.changed.notify_all();
    worker.join();

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), "Stopped");
    EXPECT_EQ(fake.requestCloses, 1);
    EXPECT_EQ(fake.closesDuringCall, 0);
    EXPECT_EQ(fake.closeThread, workerId);
}

TEST(WindowsHttpsFakeRequestTest, AStopWhileTheSendIsStillRunningClosesOnlyOnceItReturns)
{
    stopInsideCall(Step::Send);
}

TEST(WindowsHttpsFakeRequestTest, AStopWhileTheResponseCallIsStillRunningClosesOnlyOnceItReturns)
{
    stopInsideCall(Step::Receive);
}

TEST(WindowsHttpsFakeRequestTest, AStopWhileTheDataQueryIsStillRunningClosesOnlyOnceItReturns)
{
    stopInsideCall(Step::QueryData);
}

TEST(WindowsHttpsFakeRequestTest, AStopWhileAReadIsStillRunningClosesOnlyOnceItReturns)
{
    stopInsideCall(Step::Read);
}

/// Throws on the worker while the call at @p step is in flight (#3342): the call started, and (a read) was
/// given its buffer, then a throw unwinds get(). The handle is still closed once, on the worker, and get()
/// doesn't let the exception out until HANDLE_CLOSING has come, so the context and the buffer outlive
/// WinHTTP's use of them.
void throwDuring(Step step)
{
    FakeHttpScope scope;
    FakeHttp& fake = scope.fake();
    fake.pendingAt = step;
    fake.throwAt = step;
    fake.closingOnClose = false; // the test sends HANDLE_CLOSING, later
    WindowsHttpsClient client(fakeFunctions());
    const std::stop_source stop;
    std::thread::id workerId;
    std::atomic<bool> threw = false;
    std::atomic<bool> returned = false;
    std::jthread worker(
        [&]
        {
            workerId = std::this_thread::get_id();
            try
            {
                [[maybe_unused]] const auto response = client.get(validRequest(), stop.get_token());
            }
            catch (const std::bad_alloc&)
            {
                threw = true;
            }
            returned = true;
        });
    fake.waitUntil([&fake] { return fake.requestCloses > 0; });

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    // Fatal: a get() that returned has freed the request state, which the writes and callbacks below would touch.
    ASSERT_FALSE(returned.load()) << "the exception came out before WinHTTP let go of the handle";
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

    EXPECT_TRUE(threw.load()) << "the throw reaches get()'s caller";
    EXPECT_EQ(fake.requestCloses, 1);
    EXPECT_EQ(fake.closeThread, workerId);
}

TEST(WindowsHttpsFakeRequestTest, AThrowDuringTheSendClosesOnTheWorkerAndWaitsForHandleClosing)
{
    throwDuring(Step::Send);
}

TEST(WindowsHttpsFakeRequestTest, AThrowDuringAReadClosesOnTheWorkerAndKeepsTheBufferUntilHandleClosing)
{
    throwDuring(Step::Read);
}

// Real WinHTTP (#3333): winhttp.dll's own calls, close counted, against a TCP listener on 127.0.0.1 only
// (no firewall prompt, nothing leaves the machine). The listener accepts and never answers, so the TLS
// handshake stalls in a real asynchronous call; only a stop or the deadline can end it, and get() returns
// only once real WinHTTP has sent HANDLE_CLOSING after the close.

[[nodiscard]] bool isSocket(SOCKET socket) noexcept
{
    return socket != INVALID_SOCKET; // NOLINT(modernize-use-integer-sign-comparison) - INVALID_SOCKET is (SOCKET)~0
}

/// A listener on 127.0.0.1 at a port Windows picks. Holds each connection it accepts open, saying
/// nothing, or closes it at once.
class LoopbackListener
{
  public:
    explicit LoopbackListener(bool closeAtOnce) : m_CloseAtOnce(closeAtOnce)
    {
        WSADATA data{};
        m_Started = ::WSAStartup(MAKEWORD(2, 2), &data) == 0;
        if (!m_Started)
        {
            return;
        }
        m_Socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (!isSocket(m_Socket))
        {
            return;
        }
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        int length = sizeof(address);
        // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast) - Winsock's sockaddr casts
        if (::bind(m_Socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 || ::listen(m_Socket, SOMAXCONN) != 0 ||
            ::getsockname(m_Socket, reinterpret_cast<sockaddr*>(&address), &length) != 0)
        {
            return;
        }
        // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
        // Non-blocking, so the accept loop polls with a short timeout and sees a stop: a socket is never
        // closed while that thread is blocked in a call on it.
        u_long nonBlocking = 1;
        if (::ioctlsocket(m_Socket, static_cast<long>(FIONBIO), &nonBlocking) != 0)
        {
            return;
        }
        m_Port = ::ntohs(address.sin_port);
        m_Thread = std::thread([this] { acceptAll(); });
    }
    ~LoopbackListener()
    {
        m_Stop = true;
        if (m_Thread.joinable())
        {
            m_Thread.join(); // within one poll interval
        }
        for (const SOCKET accepted : m_Accepted)
        {
            ::closesocket(accepted);
        }
        if (isSocket(m_Socket))
        {
            ::closesocket(m_Socket);
        }
        if (m_Started)
        {
            ::WSACleanup();
        }
    }
    LoopbackListener(const LoopbackListener&) = delete;
    LoopbackListener& operator=(const LoopbackListener&) = delete;
    LoopbackListener(LoopbackListener&&) = delete;
    LoopbackListener& operator=(LoopbackListener&&) = delete;

    /// 0 if it couldn't listen.
    [[nodiscard]] INTERNET_PORT port() const noexcept
    {
        return m_Port;
    }

  private:
    void acceptAll()
    {
        constexpr INT POLL_MS = 10;
        while (!m_Stop)
        {
            WSAPOLLFD listening{.fd = m_Socket, .events = POLLRDNORM, .revents = 0};
            const int ready = ::WSAPoll(&listening, 1, POLL_MS);
            if (ready < 0)
            {
                return;
            }
            if (ready == 0)
            {
                continue; // nothing to accept yet; look at the stop again
            }
            const SOCKET accepted = ::accept(m_Socket, nullptr, nullptr);
            if (!isSocket(accepted))
            {
                continue; // WSAEWOULDBLOCK: the connection went before it was accepted
            }
            if (m_CloseAtOnce)
            {
                ::closesocket(accepted);
            }
            else
            {
                m_Accepted.push_back(accepted); // only this thread, until the join
            }
        }
    }

    bool m_CloseAtOnce;
    std::atomic<bool> m_Stop = false;
    bool m_Started = false;
    SOCKET m_Socket = INVALID_SOCKET;
    INTERNET_PORT m_Port = 0;
    std::vector<SOCKET> m_Accepted;
    std::thread m_Thread;
};

/// What the counting wrappers saw: the request handle real WinHTTP opened and how often it was closed.
struct RealCounts
{
    std::atomic<HINTERNET> request = nullptr;
    std::atomic<int> requestCloses = 0;
};

RealCounts& realCounts()
{
    static RealCounts counts;
    return counts;
}

HINTERNET WINAPI
countingOpenRequest(HINTERNET connection, LPCWSTR verb, LPCWSTR path, LPCWSTR version, LPCWSTR referrer, LPCWSTR* acceptTypes, DWORD flags)
{
    HINTERNET request = ::WinHttpOpenRequest(connection, verb, path, version, referrer, acceptTypes, flags);
    realCounts().request = request;
    return request;
}

BOOL WINAPI countingCloseHandle(HINTERNET handle)
{
    if (handle != nullptr && handle == realCounts().request.load())
    {
        ++realCounts().requestCloses;
    }
    return ::WinHttpCloseHandle(handle);
}

/// The real WinHttpOpen with no proxy, whatever access type the client asks for: the machine's proxy
/// settings (WPAD, PAC) never send a loopback test's request elsewhere or slow it. The flags, WINHTTP_FLAG_ASYNC
/// among them, are the client's.
HINTERNET WINAPI noProxyOpen(LPCWSTR agent, DWORD /*access*/, LPCWSTR /*proxy*/, LPCWSTR /*bypass*/, DWORD flags)
{
    return ::WinHttpOpen(agent, WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, flags);
}

/// winhttp.dll's calls, with no proxy and the request's open and close counted.
WindowsHttpsDetail::WinHttpFunctions countingRealFunctions()
{
    realCounts().request = nullptr;
    realCounts().requestCloses = 0;
    WindowsHttpsDetail::WinHttpFunctions fns;
    fns.open = &noProxyOpen;
    fns.openRequest = &countingOpenRequest;
    fns.closeHandle = &countingCloseHandle;
    return fns;
}

using HttpsResult = std::expected<HttpsResponse, std::string>;

/// One real request on its own thread, which shares ownership of everything it uses: if get() hangs, the
/// test fails at the watchdog instead of hanging the run, and the stuck thread touches nothing freed.
struct RealRun
{
    std::shared_ptr<LoopbackListener> listener;
    std::shared_ptr<WindowsHttpsClient> client;
    std::shared_ptr<std::stop_source> stop = std::make_shared<std::stop_source>();
    std::future<HttpsResult> result;

    RealRun(bool closeAtOnce, std::chrono::milliseconds timeout) : listener(std::make_shared<LoopbackListener>(closeAtOnce))
    {
        client = std::make_shared<WindowsHttpsClient>(countingRealFunctions(), listener->port());
        HttpsRequest request = validRequest();
        request.host = "127.0.0.1";
        request.path = "/";
        request.timeout = timeout;
        const auto promise = std::make_shared<std::promise<HttpsResult>>();
        result = promise->get_future();
        std::thread([listener = listener, client = client, stop = stop, promise, request = std::move(request)]
                    { promise->set_value(client->get(request, stop->get_token())); })
            .detach();
    }

    /// The result, or nullopt if get() hasn't returned within @p limit.
    [[nodiscard]] std::optional<HttpsResult> waitFor(std::chrono::milliseconds limit)
    {
        if (result.wait_for(limit) != std::future_status::ready)
        {
            return std::nullopt;
        }
        return result.get();
    }
};

constexpr auto WATCHDOG = std::chrono::seconds(5);

TEST(WindowsHttpsRealLoopbackTest, AStopEndsARealStalledRequestPromptly)
{
    RealRun run(false, std::chrono::milliseconds(60'000));
    ASSERT_NE(run.listener->port(), 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const auto stopped = std::chrono::steady_clock::now();
    static_cast<void>(run.stop->request_stop());
    const std::optional<HttpsResult> result = run.waitFor(WATCHDOG);
    ASSERT_TRUE(result.has_value()) << "get() didn't return within 5 s of the stop: HANDLE_CLOSING never came?";
    EXPECT_LT(std::chrono::steady_clock::now() - stopped, std::chrono::seconds(2));
    ASSERT_FALSE(result->has_value());     // NOLINT(bugprone-unchecked-optional-access) - asserted above
    EXPECT_EQ(result->error(), "Stopped"); // NOLINT(bugprone-unchecked-optional-access) - asserted above
    EXPECT_EQ(realCounts().requestCloses.load(), 1);
}

TEST(WindowsHttpsRealLoopbackTest, TheDeadlineEndsARealStalledRequestPromptly)
{
    const auto started = std::chrono::steady_clock::now();
    RealRun run(false, std::chrono::milliseconds(300));
    ASSERT_NE(run.listener->port(), 0);
    const std::optional<HttpsResult> result = run.waitFor(WATCHDOG);
    ASSERT_TRUE(result.has_value()) << "get() didn't return within 5 s of a 300 ms deadline";
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(2));
    ASSERT_FALSE(result->has_value());                                     // NOLINT(bugprone-unchecked-optional-access) - asserted above
    EXPECT_TRUE(result->error().contains("timed out")) << result->error(); // NOLINT(bugprone-unchecked-optional-access) - asserted above
    EXPECT_EQ(realCounts().requestCloses.load(), 1);
}

TEST(WindowsHttpsRealLoopbackTest, AConnectionTheServerDropsFailsPromptly)
{
    const auto started = std::chrono::steady_clock::now();
    RealRun run(true, std::chrono::milliseconds(60'000));
    ASSERT_NE(run.listener->port(), 0);
    const std::optional<HttpsResult> result = run.waitFor(WATCHDOG);
    ASSERT_TRUE(result.has_value()) << "get() didn't return within 5 s of the server dropping the connection";
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(2));
    ASSERT_FALSE(result->has_value());                                      // NOLINT(bugprone-unchecked-optional-access) - asserted above
    EXPECT_TRUE(result->error().starts_with("WinHttp")) << result->error(); // NOLINT(bugprone-unchecked-optional-access) - asserted above
    EXPECT_EQ(realCounts().requestCloses.load(), 1);
}

} // namespace
} // namespace Platform
