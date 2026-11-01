/// @file test_LinuxHttpsClient.cpp
/// @brief The update check's HTTPS client on Linux (#3238): what it refuses before sending, the body cap, a
/// stop already requested, and a request that can't connect, which reaches libcurl without the network (a
/// local port nothing listens on). Never the real network.

#include "Platform/Factory.h"
#include "Platform/IHttpsClient.h"
#include "Platform/Linux/LinuxHttpsClient.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>

namespace Platform
{
namespace
{

using namespace std::chrono_literals;

/// Sets an environment variable for the scope, putting back what was there.
class ScopedEnv
{
  public:
    ScopedEnv(const char* name, const char* value) : m_Name(name)
    {
        if (const char* const before = std::getenv(name); before != nullptr) // NOLINT(concurrency-mt-unsafe) - test thread
        {
            m_Before = before;
        }
        ::setenv(name, value, 1); // NOLINT(concurrency-mt-unsafe) - test thread
    }
    ~ScopedEnv()
    {
        if (m_Before.has_value())
        {
            ::setenv(m_Name, m_Before->c_str(), 1); // NOLINT(concurrency-mt-unsafe) - test thread
        }
        else
        {
            ::unsetenv(m_Name); // NOLINT(concurrency-mt-unsafe) - test thread
        }
    }
    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;
    ScopedEnv(ScopedEnv&&) = delete;
    ScopedEnv& operator=(ScopedEnv&&) = delete;

  private:
    const char* m_Name;
    std::optional<std::string> m_Before;
};

[[nodiscard]] HttpsRequest validRequest()
{
    return {.host = "api.github.com",
            .path = "/repos/mgradwohl/tasksmack/releases/latest",
            .userAgent = "TaskSmack/test",
            .accept = "application/vnd.github+json",
            .timeout = 2000ms,
            .maxBodyBytes = 1024};
}

TEST(LinuxHttpsClientTest, ARequestThatCouldChangeTheUrlOrMissesAFieldIsRefused)
{
    EXPECT_EQ(LinuxHttpsDetail::refusal(validRequest()), std::nullopt);
    for (std::string HttpsRequest::* const field : {&HttpsRequest::host, &HttpsRequest::path, &HttpsRequest::userAgent})
    {
        HttpsRequest request = validRequest();
        (request.*field).clear();
        ASSERT_TRUE(LinuxHttpsDetail::refusal(request).has_value());
        EXPECT_TRUE(LinuxHttpsDetail::refusal(request)->contains("empty"));
    }
    HttpsRequest relative = validRequest();
    relative.path = "repos/x";
    EXPECT_TRUE(LinuxHttpsDetail::refusal(relative).has_value());
    for (const char* host : {"evil.com/x", "a@b.com", "a:444", "a b", "h\nx"})
    {
        HttpsRequest request = validRequest();
        request.host = host;
        EXPECT_TRUE(LinuxHttpsDetail::refusal(request).has_value()) << host;
    }
    for (const char* path : {"/a#b", "/a b", "/a\r\nX: y"})
    {
        HttpsRequest request = validRequest();
        request.path = path;
        EXPECT_TRUE(LinuxHttpsDetail::refusal(request).has_value()) << path;
    }
}

TEST(LinuxHttpsClientTest, TheBodyStopsAtItsCap)
{
    std::string body;
    EXPECT_TRUE(LinuxHttpsDetail::appendCapped(body, "abc", 5));
    EXPECT_TRUE(LinuxHttpsDetail::appendCapped(body, "de", 5));
    EXPECT_FALSE(LinuxHttpsDetail::appendCapped(body, "f", 5));
    EXPECT_EQ(body, "abcde");
    std::string empty;
    EXPECT_FALSE(LinuxHttpsDetail::appendCapped(empty, "toolong", 3));
    EXPECT_TRUE(empty.empty());
}

TEST(LinuxHttpsClientTest, AStopAlreadyRequestedOrARefusedRequestSendsNothing)
{
    LinuxHttpsClient client;
    std::stop_source stop;
    static_cast<void>(stop.request_stop());
    const auto stopped = client.get(validRequest(), stop.get_token());
    ASSERT_FALSE(stopped.has_value());
    if (LinuxHttpsClient::built())
    {
        EXPECT_EQ(stopped.error(), "Stopped before the request was sent");
        HttpsRequest bad = validRequest();
        bad.host = "a/b";
        const auto refused = client.get(bad, std::stop_source{}.get_token());
        ASSERT_FALSE(refused.has_value());
        EXPECT_EQ(refused.error(), "The request's host isn't a plain host name");
    }
    else
    {
        EXPECT_TRUE(stopped.error().contains("without libcurl")) << stopped.error();
    }
}

// Review of #3269: a redirect is followed only to https on port 443, without credentials, to a host and path the
// client would request itself; libcurl's own following couldn't keep it to the port.
TEST(LinuxHttpsClientTest, ARedirectIsFollowedOnlyToHttpsOnPort443)
{
    using LinuxHttpsDetail::redirectRefusal;
    EXPECT_EQ(redirectRefusal("https://api.github.com/repositories/1/releases/latest"), std::nullopt);
    EXPECT_EQ(redirectRefusal("HTTPS://github.com:443/mgradwohl/tasksmack/releases/tag/v0.10.0"), std::nullopt);
    EXPECT_EQ(redirectRefusal("https://github.com"), std::nullopt) << "no path: the root";
    EXPECT_EQ(redirectRefusal("https://github.com?x=1"), std::nullopt);
    for (const char* refused : {"http://github.com/",
                                "ftp://github.com/",
                                "https://github.com:8443/",
                                "https://github.com:/",
                                "https://user:pass@github.com/",
                                "https://evil.example@github.com/",
                                "https://[::1]/",
                                "https:///path",
                                "https://github.com/a b",
                                "//github.com/",
                                ""})
    {
        EXPECT_NE(redirectRefusal(refused), std::nullopt) << refused;
    }
}

// A request to a local port nothing listens on fails in libcurl, quickly and without the network, and says so.
TEST(LinuxHttpsClientTest, ARequestThatCantConnectSaysWhyAndTheFactoryFollowsTheBuild)
{
    const std::shared_ptr<IHttpsClient> made = makeHttpsClient();
    if (!LinuxHttpsClient::usable())
    {
        EXPECT_EQ(made, nullptr) << "no libcurl that can serve the worker: no update check";
        GTEST_SKIP() << (LinuxHttpsClient::built() ? "libcurl isn't thread-safe or has no async resolver" : "built without libcurl");
    }
    ASSERT_NE(made, nullptr);
    // Straight to the local port, whatever proxy the machine has set (a proxy would answer instead).
    const ScopedEnv noProxy("NO_PROXY", "*");
    const ScopedEnv noProxyLower("no_proxy", "*");
    HttpsRequest request = validRequest();
    request.host = "127.0.0.1"; // port 443, where a test machine runs no server
    request.accept.clear();
    const auto response = made->get(request, std::stop_source{}.get_token());
    ASSERT_FALSE(response.has_value());
    EXPECT_TRUE(response.error().starts_with("libcurl: ")) << response.error();
}

} // namespace
} // namespace Platform
