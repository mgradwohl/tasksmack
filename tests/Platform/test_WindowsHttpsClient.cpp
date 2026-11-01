/// @file test_WindowsHttpsClient.cpp
/// @brief The update check's WinHTTP client (#3093), without the network: what it refuses before it opens
/// a session (a stop already requested, a request with no host, path or User-Agent), the factory, the
/// wording of its errors, and how an asynchronous call's completion is waited for (#3333). No test here sends
/// a request.

#include "Platform/Factory.h"
#include "Platform/IHttpsClient.h"
#include "Platform/Windows/WindowsHttpsClient.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
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

} // namespace
} // namespace Platform
