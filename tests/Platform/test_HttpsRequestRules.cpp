/// @file test_HttpsRequestRules.cpp
/// @brief Platform::requestRefusal() (#3870): what every platform's HTTPS client refuses to send, checked once here
/// for both. A request that could change the URL, break a header line or misses a field is refused; a good one
/// passes; and no reason quotes a header's value, which may be a secret.

#include "Platform/HttpsRequestRules.h"
#include "Platform/IHttpsClient.h"

#include <gtest/gtest.h>

#include <chrono>
#include <optional>
#include <string>

namespace Platform
{
namespace
{

using namespace std::chrono_literals;

constexpr const char* SECRET = "dummy-key-0123456789";

[[nodiscard]] HttpsRequest validRequest()
{
    return {.host = "api.github.com",
            .path = "/repos/mgradwohl/tasksmack/releases/latest",
            .userAgent = "TaskSmack/test",
            .accept = "application/vnd.github+json",
            .timeout = 2000ms,
            .maxBodyBytes = 1024,
            .headers = {{"x-apikey", SECRET}}};
}

TEST(HttpsRequestRulesTest, AGoodRequestPasses)
{
    EXPECT_EQ(requestRefusal(validRequest()), std::nullopt);
    HttpsRequest noAccept = validRequest();
    noAccept.accept.clear();
    noAccept.headers.clear();
    EXPECT_EQ(requestRefusal(noAccept), std::nullopt) << "Accept and extra headers are optional";
}

TEST(HttpsRequestRulesTest, AMissingFieldOrARelativePathIsRefused)
{
    for (std::string HttpsRequest::* const field : {&HttpsRequest::host, &HttpsRequest::path, &HttpsRequest::userAgent})
    {
        HttpsRequest request = validRequest();
        (request.*field).clear();
        const std::optional<std::string> refused = requestRefusal(request);
        ASSERT_TRUE(refused.has_value());
        EXPECT_TRUE(refused->contains("empty")) << *refused;
    }
    HttpsRequest relative = validRequest();
    relative.path = "repos/x";
    EXPECT_TRUE(requestRefusal(relative).has_value());
}

TEST(HttpsRequestRulesTest, AHostOrPathThatCouldChangeTheUrlIsRefused)
{
    for (const char* host : {"evil.com/x", "a@b.com", "a:444", "a b", "h\nx", "h\rx"})
    {
        HttpsRequest request = validRequest();
        request.host = host;
        EXPECT_TRUE(requestRefusal(request).has_value()) << host;
    }
    for (const char* path : {"/a#b", "/a b", "/a\r\nX: y"})
    {
        HttpsRequest request = validRequest();
        request.path = path;
        EXPECT_TRUE(requestRefusal(request).has_value()) << path;
    }
}

// The checks the two clients used to split between them: Windows checked Accept, Linux the host and path.
TEST(HttpsRequestRulesTest, ALineBreakInAnyHeaderIsRefused)
{
    HttpsRequest accept = validRequest();
    accept.accept = "application/json\r\nX-Injected: 1";
    EXPECT_EQ(requestRefusal(accept), "The request's Accept header holds a line break");

    HttpsRequest agent = validRequest();
    agent.userAgent = "TaskSmack/1\nX-Injected: 1";
    EXPECT_EQ(requestRefusal(agent), "The request's User-Agent holds a line break");

    for (const std::string& value : {std::string(SECRET) + "\r\n", std::string(SECRET) + "\n", std::string("a\0b", 3)})
    {
        HttpsRequest request = validRequest();
        request.headers = {{"x-apikey", value}};
        const std::optional<std::string> refused = requestRefusal(request);
        ASSERT_TRUE(refused.has_value());
        EXPECT_EQ(*refused, "A request header's value holds a line break");
        EXPECT_FALSE(refused->contains(SECRET)) << "a reason never quotes a value";
    }
    for (const char* name : {"", "x apikey", "x:apikey", "x\r\napikey"})
    {
        HttpsRequest request = validRequest();
        request.headers = {{name, "v"}};
        EXPECT_EQ(requestRefusal(request), "A request header's name isn't a header token") << name;
    }
}

} // namespace
} // namespace Platform
