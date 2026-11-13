/// @file test_VirusTotalLookup.cpp
/// @brief Look up on VirusTotal's worker half (#3094): hash, ask with the key, read; with a fake client and actions.

#include "App/Panels/VirusTotalLookup.h"
#include "Mocks/MockProbes.h"

#include <gtest/gtest.h>

#include <expected>
#include <format>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace App::VirusTotalLookupTest
{

/// Answers every request with one reply and keeps what was asked.
class FakeHttpsClient final : public Platform::IHttpsClient
{
  public:
    explicit FakeHttpsClient(std::expected<Platform::HttpsResponse, std::string> reply) : m_Reply(std::move(reply))
    {}

    [[nodiscard]] std::expected<Platform::HttpsResponse, std::string> get(const Platform::HttpsRequest& request,
                                                                          const std::stop_token& /*stop*/) override
    {
        const std::scoped_lock lock(m_Mutex);
        m_Requests.push_back(request);
        return m_Reply;
    }

    [[nodiscard]] std::vector<Platform::HttpsRequest> requests() const
    {
        const std::scoped_lock lock(m_Mutex);
        return m_Requests;
    }

  private:
    mutable std::mutex m_Mutex;
    std::expected<Platform::HttpsResponse, std::string> m_Reply;
    std::vector<Platform::HttpsRequest> m_Requests;
};

namespace
{

const std::string KEY(64, 'a');
const std::string SHA = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
const Platform::ProcessTarget TARGET{.pid = 42, .startTimeTicks = 7};

[[nodiscard]] std::string report(int malicious, int suspicious)
{
    return std::format(
        R"({{"data":{{"attributes":{{"last_analysis_stats":{{"malicious":{},"suspicious":{},"undetected":60,"harmless":0}}}}}}}})",
        malicious,
        suspicious);
}

TEST(VirusTotalLookupTest, OfferedOnlyWithAHashingPlatformAClientAndAKey)
{
    const auto client = std::make_shared<FakeHttpsClient>(Platform::HttpsResponse{});
    Platform::ProcessActionCapabilities hashes;
    hashes.canHashImage = true;
    EXPECT_TRUE(VirusTotalLookup::offered(hashes, {.client = client, .apiKey = KEY, .userAgent = "UA"}));
    EXPECT_FALSE(VirusTotalLookup::offered({}, {.client = client, .apiKey = KEY, .userAgent = "UA"})) << "Windows until #3646";
    EXPECT_FALSE(VirusTotalLookup::offered(hashes, {.client = nullptr, .apiKey = KEY, .userAgent = "UA"})) << "a test or synthetic run";
    EXPECT_FALSE(VirusTotalLookup::offered(hashes, {.client = client, .apiKey = "", .userAgent = "UA"})) << "no key: off";
    EXPECT_FALSE(VirusTotalLookup::offered(hashes, {.client = client, .apiKey = "typo", .userAgent = "UA"}));
}

TEST(VirusTotalLookupTest, SendsOnlyTheHashWithTheKeyAndReadsTheVerdict)
{
    TestMocks::MockProcessActions actions;
    actions.setImageSha256Result(SHA);
    const auto client = std::make_shared<FakeHttpsClient>(Platform::HttpsResponse{.body = report(0, 0), .status = 200});
    const Platform::ProcessActionResult clean =
        VirusTotalLookup::lookUp(actions, {.client = client, .apiKey = KEY, .userAgent = "TaskSmack/1"}, TARGET, "bash", {});
    EXPECT_TRUE(clean.success);
    EXPECT_EQ(clean.successNote, "VirusTotal: no engine flags bash (0 of 60) (https://www.virustotal.com/gui/file/" + SHA + ")");

    ASSERT_EQ(client->requests().size(), 1U);
    const Platform::HttpsRequest request = client->requests().front(); // a copy: requests() returns by value
    EXPECT_EQ(request.host, "www.virustotal.com");
    EXPECT_EQ(request.path, "/api/v3/files/" + SHA) << "the hash, and nothing about the file or the process";
    EXPECT_EQ(request.userAgent, "TaskSmack/1");
    ASSERT_EQ(request.headers.size(), 1U);
    EXPECT_EQ(request.headers.front(), (std::pair<std::string, std::string>{"x-apikey", KEY}));
    EXPECT_EQ(actions.imageSha256Count(), 1);
}

TEST(VirusTotalLookupTest, AFlaggedFileReadsAsAWarning)
{
    TestMocks::MockProcessActions actions;
    actions.setImageSha256Result(SHA);
    const auto client = std::make_shared<FakeHttpsClient>(Platform::HttpsResponse{.body = report(2, 1), .status = 200});
    const Platform::ProcessActionResult flagged =
        VirusTotalLookup::lookUp(actions, {.client = client, .apiKey = KEY, .userAgent = "UA"}, TARGET, "odd.exe", {});
    EXPECT_FALSE(flagged.success) << "drawn in the error colour";
    EXPECT_TRUE(flagged.errorMessage.starts_with("VirusTotal: 3 of 63 engines flag odd.exe (2 malicious, 1 suspicious)"));
    EXPECT_EQ(VirusTotalLookup::format(flagged).text, flagged.errorMessage);
    EXPECT_FALSE(VirusTotalLookup::format(flagged).ok);
}

TEST(VirusTotalLookupTest, NothingIsSentWhenTheHashOrKeyIsMissing)
{
    TestMocks::MockProcessActions actions;
    const auto client = std::make_shared<FakeHttpsClient>(Platform::HttpsResponse{.body = report(0, 0), .status = 200});
    actions.setImageSha256Result(std::unexpected(std::string("Its executable can't be read: it belongs to another user")));
    const Platform::ProcessActionResult unread =
        VirusTotalLookup::lookUp(actions, {.client = client, .apiKey = KEY, .userAgent = "UA"}, TARGET, "sshd", {});
    EXPECT_FALSE(unread.success);
    EXPECT_EQ(unread.errorMessage, "Couldn't hash sshd's executable: Its executable can't be read: it belongs to another user");

    actions.setImageSha256Result(SHA);
    const Platform::ProcessActionResult noKey =
        VirusTotalLookup::lookUp(actions, {.client = client, .apiKey = "", .userAgent = "UA"}, TARGET, "sshd", {});
    EXPECT_FALSE(noKey.success);
    EXPECT_TRUE(client->requests().empty()) << "no request without a hash or a key";
}

TEST(VirusTotalLookupTest, AnUnknownFileARefusedKeyAndAnUnreachableServiceSaySo)
{
    TestMocks::MockProcessActions actions;
    actions.setImageSha256Result(SHA);
    const VirusTotalLookup::Context context{.client = nullptr, .apiKey = KEY, .userAgent = "UA"};
    const auto run = [&actions, &context](std::expected<Platform::HttpsResponse, std::string> reply)
    {
        VirusTotalLookup::Context withClient = context;
        withClient.client = std::make_shared<FakeHttpsClient>(std::move(reply));
        return VirusTotalLookup::lookUp(actions, withClient, TARGET, "bash", {});
    };
    const Platform::ProcessActionResult unknown = run(Platform::HttpsResponse{.body = "{}", .status = 404});
    EXPECT_TRUE(unknown.success);
    EXPECT_TRUE(unknown.successNote.starts_with("VirusTotal doesn't know bash's file"));
    const Platform::ProcessActionResult refused = run(Platform::HttpsResponse{.body = "", .status = 401});
    EXPECT_FALSE(refused.success);
    EXPECT_EQ(refused.errorMessage, "VirusTotal refused the API key: check it in Settings");
    const Platform::ProcessActionResult offline = run(std::unexpected(std::string("libcurl: Couldn't resolve host name")));
    EXPECT_FALSE(offline.success);
    EXPECT_EQ(offline.errorMessage, "Couldn't reach VirusTotal: libcurl: Couldn't resolve host name");
}

} // namespace
} // namespace App::VirusTotalLookupTest
