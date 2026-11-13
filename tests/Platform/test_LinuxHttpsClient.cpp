/// @file test_LinuxHttpsClient.cpp
/// @brief The update check's HTTPS client on Linux (#3238): what it refuses before sending, the body cap, a
/// stop already requested, and a request that can't connect, which reaches libcurl without the network (a
/// local port nothing listens on). Never the real network.

#include "Platform/Factory.h"
#include "Platform/IHttpsClient.h"
#include "Platform/Linux/LinuxHttpsClient.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <format>
#include <initializer_list>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

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
            .maxBodyBytes = 1024,
            .headers = {}};
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

// #3094: an extra header goes out only with a token name and a value that can't end the line.
TEST(LinuxHttpsClientTest, AnExtraHeaderThatCouldBreakTheRequestIsRefused)
{
    HttpsRequest keyed = validRequest();
    keyed.headers = {{"x-apikey", std::string(64, 'a')}};
    EXPECT_EQ(LinuxHttpsDetail::refusal(keyed), std::nullopt);
    for (const auto& [name, value] : std::initializer_list<std::pair<const char*, const char*>>{
             {"", "v"}, {"x apikey", "v"}, {"x:apikey", "v"}, {"x\napikey", "v"}, {"x-apikey", "a\r\nHost: evil"}, {"x-apikey", "a\nb"}})
    {
        HttpsRequest request = validRequest();
        request.headers = {{name, value}};
        EXPECT_TRUE(LinuxHttpsDetail::refusal(request).has_value()) << name << " / " << value;
    }
    HttpsRequest nul = validRequest();
    nul.headers = {{"x-apikey", std::string("a\0b", 3)}};
    EXPECT_TRUE(LinuxHttpsDetail::refusal(nul).has_value());
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

// --- followRedirects(): get()'s redirect chain, through a fake transfer (#3374) ---

using Step = LinuxHttpsDetail::TransferStep;

[[nodiscard]] Step replyStep(int status, std::optional<std::string> location = std::nullopt, std::string body = {})
{
    return Step{.end = Step::End::Reply, .failure = {}, .status = status, .location = std::move(location), .body = std::move(body)};
}

/// A transfer that answers from @p script in turn, recording each URL it was asked for.
struct ScriptedTransfer
{
    std::vector<Step> script;
    std::vector<std::string> urls;
    std::vector<std::chrono::milliseconds> lefts;

    [[nodiscard]] LinuxHttpsDetail::TransferFunction function()
    {
        return [this](const std::string& url, std::chrono::milliseconds left)
        {
            urls.push_back(url);
            lefts.push_back(left);
            return urls.size() <= script.size() ? script[urls.size() - 1] : replyStep(200);
        };
    }
};

[[nodiscard]] std::chrono::steady_clock::time_point inAMinute()
{
    return std::chrono::steady_clock::now() + std::chrono::minutes(1);
}

TEST(LinuxHttpsClientTest, RedirectsAreFollowedToTheReplyThatIsntOne)
{
    ScriptedTransfer transfer{.script = {replyStep(301, "https://api.example.com/v2/latest"),
                                         replyStep(302, "https://cdn.example.com:443/latest.json"),
                                         replyStep(200, std::nullopt, "{}")},
                              .urls = {},
                              .lefts = {}};
    const auto result = LinuxHttpsDetail::followRedirects("https://api.example.com/latest", inAMinute(), 1024, transfer.function());
    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_EQ(result->status, 200);
    EXPECT_EQ(result->body, "{}");
    EXPECT_EQ(transfer.urls,
              (std::vector<std::string>{
                  "https://api.example.com/latest", "https://api.example.com/v2/latest", "https://cdn.example.com:443/latest.json"}));
    for (const std::chrono::milliseconds left : transfer.lefts)
    {
        EXPECT_GT(left.count(), 0);
        EXPECT_LE(left, std::chrono::minutes(1)) << "each gets the time left, not a fresh budget";
    }
}

TEST(LinuxHttpsClientTest, AThreeHundredWithoutALocationOrAnErrorStatusIsTheReply)
{
    ScriptedTransfer notModified{.script = {replyStep(304, std::nullopt, "")}, .urls = {}, .lefts = {}};
    const auto unchanged = LinuxHttpsDetail::followRedirects("https://api.example.com/", inAMinute(), 1024, notModified.function());
    ASSERT_TRUE(unchanged.has_value());
    EXPECT_EQ(unchanged->status, 304);

    ScriptedTransfer forbidden{.script = {replyStep(403, "https://api.example.com/other", "rate limited")}, .urls = {}, .lefts = {}};
    const auto refused = LinuxHttpsDetail::followRedirects("https://api.example.com/", inAMinute(), 1024, forbidden.function());
    ASSERT_TRUE(refused.has_value());
    EXPECT_EQ(refused->status, 403) << "a location on a non-3xx reply isn't followed";
    EXPECT_EQ(forbidden.urls.size(), 1U);
}

TEST(LinuxHttpsClientTest, TooManyRedirectsOrARefusedOneEndsTheRequest)
{
    ScriptedTransfer loop{.script = {}, .urls = {}, .lefts = {}};
    for (int i = 0; i <= LinuxHttpsDetail::MAX_REDIRECTS; ++i)
    {
        loop.script.push_back(replyStep(302, "https://api.example.com/again"));
    }
    const auto looped = LinuxHttpsDetail::followRedirects("https://api.example.com/", inAMinute(), 1024, loop.function());
    ASSERT_FALSE(looped.has_value());
    EXPECT_EQ(looped.error(), std::format("More than {} redirects", LinuxHttpsDetail::MAX_REDIRECTS));
    EXPECT_EQ(loop.urls.size(), static_cast<std::size_t>(LinuxHttpsDetail::MAX_REDIRECTS) + 1U);

    ScriptedTransfer downgrade{.script = {replyStep(301, "http://api.example.com/")}, .urls = {}, .lefts = {}};
    const auto plain = LinuxHttpsDetail::followRedirects("https://api.example.com/", inAMinute(), 1024, downgrade.function());
    ASSERT_FALSE(plain.has_value());
    EXPECT_EQ(plain.error(), "A redirect to anything but https isn't followed");
    EXPECT_EQ(downgrade.urls.size(), 1U) << "the refused one isn't requested";
}

TEST(LinuxHttpsClientTest, ATooLongBodyAStopOrAFailedTransferEndsTheRequestWithWhy)
{
    ScriptedTransfer tooLong{.script = {Step{.end = Step::End::TooLong, .failure = {}, .status = 0, .location = std::nullopt, .body = {}}},
                             .urls = {},
                             .lefts = {}};
    const auto big = LinuxHttpsDetail::followRedirects("https://api.example.com/", inAMinute(), 4096, tooLong.function());
    ASSERT_FALSE(big.has_value());
    EXPECT_EQ(big.error(), "The reply was longer than 4096 bytes");

    ScriptedTransfer stopped{.script = {Step{.end = Step::End::Stopped, .failure = {}, .status = 0, .location = std::nullopt, .body = {}}},
                             .urls = {},
                             .lefts = {}};
    const auto halted = LinuxHttpsDetail::followRedirects("https://api.example.com/", inAMinute(), 4096, stopped.function());
    ASSERT_FALSE(halted.has_value());
    EXPECT_EQ(halted.error(), "Stopped");

    ScriptedTransfer failed{.script = {replyStep(302, "https://api.example.com/next"),
                                       Step{.end = Step::End::Failed,
                                            .failure = "libcurl: Couldn't resolve host name",
                                            .status = 0,
                                            .location = std::nullopt,
                                            .body = {}}},
                            .urls = {},
                            .lefts = {}};
    const auto broken = LinuxHttpsDetail::followRedirects("https://api.example.com/", inAMinute(), 4096, failed.function());
    ASSERT_FALSE(broken.has_value());
    EXPECT_EQ(broken.error(), "libcurl: Couldn't resolve host name") << "a redirect's failure is the request's";
}

TEST(LinuxHttpsClientTest, NoTransferStartsOnceTheDeadlineHasPassed)
{
    ScriptedTransfer transfer{.script = {}, .urls = {}, .lefts = {}};
    const auto late = LinuxHttpsDetail::followRedirects(
        "https://api.example.com/", std::chrono::steady_clock::now() - std::chrono::milliseconds(1), 1024, transfer.function());
    ASSERT_FALSE(late.has_value());
    EXPECT_EQ(late.error(), "libcurl: Timeout was reached");
    EXPECT_TRUE(transfer.urls.empty());
}

} // namespace
} // namespace Platform
