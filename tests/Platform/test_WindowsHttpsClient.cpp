/// @file test_WindowsHttpsClient.cpp
/// @brief The update check's WinHTTP client (#3093), without the network: what it refuses before it opens
/// a session (a stop already requested, a request with no host, path or User-Agent), the factory, and the
/// wording of its errors. No test here sends a request.

#include "Platform/Factory.h"
#include "Platform/IHttpsClient.h"
#include "Platform/Windows/WindowsHttpsClient.h"

#include <gtest/gtest.h>

#include <chrono>
#include <expected>
#include <memory>
#include <stop_token>
#include <string>

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

} // namespace
} // namespace Platform
