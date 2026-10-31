/// @file test_UpdateCheck.cpp
/// @brief The opt-in update check's decisions (#3093), all pure: SemVer parsing and precedence (pre-release
/// suffixes, 'v' prefixes, malformed tags), the release JSON's top-level fields (malformed, nested,
/// escaped and huge inputs, and arbitrary bytes), what an answer means, the release-page URL check, the
/// once-a-day schedule on an injected clock, and what the notice offers.

#include "Domain/SamplingConfig.h"
#include "Domain/UpdateCheck.h"

#include <gtest/gtest.h>

#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

namespace Domain::UpdateCheck
{
namespace
{

using namespace std::chrono_literals;

Version version(std::string_view text)
{
    const std::optional<Version> parsed = parseVersion(text);
    EXPECT_TRUE(parsed.has_value()) << text;
    return parsed.value_or(Version{});
}

// ---------------------------------------------------------------------------------------------------
// Versions
// ---------------------------------------------------------------------------------------------------

TEST(UpdateCheckVersionTest, ParsesReleasesWithOrWithoutAVPrefix)
{
    struct Case
    {
        std::string_view text;
        std::uint32_t major;
        std::uint32_t minor;
        std::uint32_t patch;
        std::string_view normalized;
    };
    const std::vector<Case> cases = {
        {.text = "0.10.0", .major = 0, .minor = 10, .patch = 0, .normalized = "0.10.0"},
        {.text = "v0.10.0", .major = 0, .minor = 10, .patch = 0, .normalized = "0.10.0"},
        {.text = "V1.2.3", .major = 1, .minor = 2, .patch = 3, .normalized = "1.2.3"},
        {.text = "v4294967295.0.0", .major = 4294967295U, .minor = 0, .patch = 0, .normalized = "4294967295.0.0"},
        {.text = "1.2.3-rc.1", .major = 1, .minor = 2, .patch = 3, .normalized = "1.2.3-rc.1"},
        {.text = "1.2.3-alpha-2.0.x", .major = 1, .minor = 2, .patch = 3, .normalized = "1.2.3-alpha-2.0.x"},
        {.text = "1.2.3+build.7", .major = 1, .minor = 2, .patch = 3, .normalized = "1.2.3"},
        {.text = "v1.2.3-beta+exp.sha.5114f85", .major = 1, .minor = 2, .patch = 3, .normalized = "1.2.3-beta"},
        {.text = "1.2.3+001", .major = 1, .minor = 2, .patch = 3, .normalized = "1.2.3"}, // a build may lead with 0
    };
    for (const Case& c : cases)
    {
        const std::optional<Version> parsedRead = parseVersion(c.text);
        ASSERT_TRUE(parsedRead.has_value()) << c.text;
        const Version parsed = parsedRead.value_or(Version{});
        EXPECT_EQ(parsed.major, c.major) << c.text;
        EXPECT_EQ(parsed.minor, c.minor) << c.text;
        EXPECT_EQ(parsed.patch, c.patch) << c.text;
        EXPECT_EQ(parsed.text(), c.normalized) << c.text;
    }
    EXPECT_EQ(version("1.2.3+b.1").build, "b.1");
}

TEST(UpdateCheckVersionTest, RefusesMalformedTags)
{
    const std::vector<std::string_view> malformed = {
        "",
        "v",
        "vv1.2.3",
        "latest",
        "1",
        "1.2",
        "1.2.",
        "1..3",
        "1.2.3.4",
        " 1.2.3",
        "1.2.3 ",
        "1.2.3\n",
        "01.2.3",
        "1.02.3",
        "1.2.03",
        "-1.2.3",
        "1.2.3-",
        "1.2.3-.",
        "1.2.3-rc.",
        "1.2.3-rc..1",
        "1.2.3-01",
        "1.2.3-rc.01",
        "1.2.3-rc_1",
        "1.2.3+",
        "1.2.3+b..1",
        "1.2.3+b!",
        "1.2.3x",
        "4294967296.0.0", // past 32 bits
        "99999999999999999999.0.0",
        "1.2.3-rc+",
        "\xC2\xB9.2.3", // a superscript one isn't a digit
    };
    for (const std::string_view text : malformed)
    {
        EXPECT_FALSE(parseVersion(text).has_value()) << '"' << text << '"';
    }
}

TEST(UpdateCheckVersionTest, OrdersBySemVerPrecedence)
{
    // SemVer 2.0.0's own example, lowest first, then a few of TaskSmack's.
    const std::vector<std::string_view> ascending = {
        "1.0.0-alpha",
        "1.0.0-alpha.1",
        "1.0.0-alpha.beta",
        "1.0.0-beta",
        "1.0.0-beta.2",
        "1.0.0-beta.11",
        "1.0.0-rc.1",
        "1.0.0",
        "1.0.1",
        "1.1.0",
        "1.10.0",
        "2.0.0",
    };
    for (std::size_t i = 0; i < ascending.size(); ++i)
    {
        for (std::size_t j = 0; j < ascending.size(); ++j)
        {
            const std::strong_ordering expected = i <=> j;
            EXPECT_EQ(compare(version(ascending[i]), version(ascending[j])), expected) << ascending[i] << " vs " << ascending[j];
        }
    }
}

TEST(UpdateCheckVersionTest, IgnoresTheBuildAndThePrefix)
{
    EXPECT_EQ(compare(version("v0.10.0"), version("0.10.0")), std::strong_ordering::equal);
    EXPECT_EQ(compare(version("1.2.3+a"), version("1.2.3+b")), std::strong_ordering::equal);
    EXPECT_EQ(compare(version("0.9.0"), version("0.10.0")), std::strong_ordering::less); // numbers, not text
    EXPECT_EQ(compare(version("1.0.0-rc.9"), version("1.0.0-rc.10")), std::strong_ordering::less);
    EXPECT_EQ(compare(version("1.0.0-11"), version("1.0.0-9")), std::strong_ordering::greater);
    EXPECT_EQ(compare(version("1.0.0-9"), version("1.0.0-a")), std::strong_ordering::less); // numeric below alphanumeric
    EXPECT_EQ(compare(version("1.0.0-B"), version("1.0.0-a")), std::strong_ordering::less); // ASCII order
}

RC_GTEST_PROP(UpdateCheckVersionPropertiesTest,
              AReleaseRoundTripsThroughItsText,
              (std::uint32_t major, std::uint32_t minor, std::uint32_t patch))
{
    Version original;
    original.major = major;
    original.minor = minor;
    original.patch = patch;
    const std::optional<Version> parsed = parseVersion("v" + original.text());
    RC_ASSERT(parsed.has_value());
    const Version read = parsed.value_or(Version{});
    RC_ASSERT(compare(read, original) == std::strong_ordering::equal);
    RC_ASSERT(read.text() == original.text());
}

RC_GTEST_PROP(UpdateCheckVersionPropertiesTest, AnyTextParsesOrNotWithoutHarm, (const std::string& text))
{
    if (const std::optional<Version> parsed = parseVersion(text); parsed.has_value())
    {
        // What parses compares equal to itself and re-parses from its own text.
        const Version& read = *parsed;
        RC_ASSERT(compare(read, read) == std::strong_ordering::equal);
        const std::optional<Version> again = parseVersion(read.text());
        RC_ASSERT(again.has_value());
        RC_ASSERT(compare(again.value_or(Version{}), read) == std::strong_ordering::equal);
    }
}

TEST(UpdateCheckTest, TheUserAgentNamesTheVersion)
{
    EXPECT_EQ(userAgent("0.10.0"), "TaskSmack/0.10.0");
}

// ---------------------------------------------------------------------------------------------------
// The reply
// ---------------------------------------------------------------------------------------------------

// A trimmed copy of what GitHub's /releases/latest returns: the author's and the asset's html_url come
// before and after the release's own, and the body is long, escaped markdown.
constexpr std::string_view GITHUB_REPLY = R"({
  "url": "https://api.github.com/repos/mgradwohl/tasksmack/releases/1",
  "author": {
    "login": "mgradwohl",
    "html_url": "https://github.com/mgradwohl",
    "site_admin": false
  },
  "html_url": "https://github.com/mgradwohl/tasksmack/releases/tag/v0.10.0",
  "id": 123456789,
  "tag_name": "v0.10.0",
  "target_commitish": "main",
  "name": "TaskSmack 0.10.0",
  "draft": false,
  "prerelease": false,
  "created_at": "2026-10-28T12:00:00Z",
  "assets": [
    {
      "name": "TaskSmack-0.10.0-win64.msi",
      "size": 12345678,
      "uploader": { "html_url": "https://github.com/web-flow" },
      "browser_download_url": "https://github.com/mgradwohl/tasksmack/releases/download/v0.10.0/TaskSmack.msi"
    }
  ],
  "reactions": {"+1": 3, "total_count": 3.5e0, "rate": -0.25E+1},
  "mentions_count": null,
  "body": "## What's new\r\n\n* \"Quoted\" and \\ escaped \u00e9 \ud83d\ude00 \/ text\t\b\f"
})";

TEST(UpdateCheckReplyTest, ReadsTheReleasesOwnTopLevelFields)
{
    const std::optional<ReleaseFields> fieldsRead = readReleaseFields(GITHUB_REPLY);
    ASSERT_TRUE(fieldsRead.has_value());
    const ReleaseFields fields = fieldsRead.value_or(ReleaseFields{});
    EXPECT_EQ(fields.tagName, std::optional<std::string>("v0.10.0"));
    EXPECT_EQ(fields.htmlUrl, std::optional<std::string>("https://github.com/mgradwohl/tasksmack/releases/tag/v0.10.0"));
    EXPECT_EQ(fields.draft, std::optional<bool>(false));
    EXPECT_EQ(fields.prerelease, std::optional<bool>(false));
}

TEST(UpdateCheckReplyTest, NestedHtmlUrlsAreNeverTheReleases)
{
    const std::optional<ReleaseFields> fields =
        readReleaseFields(R"({"author":{"html_url":"https://evil.example/","tag_name":"v99.0.0"},"assets":[{"html_url":"x"}]})");
    ASSERT_TRUE(fields.has_value());
    EXPECT_FALSE(fields.value_or(ReleaseFields{}).htmlUrl.has_value());
    EXPECT_FALSE(fields.value_or(ReleaseFields{}).tagName.has_value());
}

TEST(UpdateCheckReplyTest, DecodesEscapesInTheFieldsItReads)
{
    const std::optional<ReleaseFields> fieldsRead =
        readReleaseFields(R"({"tag\u005fname":"v1.2.3-\u0072c.1","html_url":"a\/b\u00e9\ud83d\ude00"})");
    ASSERT_TRUE(fieldsRead.has_value());
    const ReleaseFields fields = fieldsRead.value_or(ReleaseFields{});
    EXPECT_EQ(fields.tagName, std::optional<std::string>("v1.2.3-rc.1"));
    EXPECT_EQ(fields.htmlUrl, std::optional<std::string>("a/b\xC3\xA9\xF0\x9F\x98\x80"));
}

TEST(UpdateCheckReplyTest, DecodesAnEscapeAboveTheSurrogatesToThreeBytes)
{
    // U+20AC (the euro sign) and U+E000, the first code point past the surrogates: three UTF-8 bytes each.
    const std::optional<ReleaseFields> fieldsRead = readReleaseFields(R"({"tag_name":"\u20ac\ue000"})");
    ASSERT_TRUE(fieldsRead.has_value());
    EXPECT_EQ(fieldsRead.value_or(ReleaseFields{}).tagName, std::optional<std::string>("\xE2\x82\xAC\xEE\x80\x80"));
}

TEST(UpdateCheckReplyTest, RefusesEscapesCutShortByTheEndOrPairedWithTheWrongHalf)
{
    const std::vector<std::string_view> malformed = {
        R"({"tag_name":"\u12)",           // fewer than four hex digits before the end
        R"({"tag_name":"\)",              // a backslash at the very end
        R"({"tag_name":"\ud83d\n"})",     // a high surrogate followed by another escape
        R"({"tag_name":"\ud83d\u0041"})", // ... by a code point below the low surrogates
        R"({"tag_name":"\ud83d\ue000"})", // ... by one above them
        R"({"tag_name":"\ud83d\ud83d"})", // ... by another high surrogate
        R"({"tag_name":"\ud83d\uZZZZ"})", // ... by no hex digits at all
        R"({"tag_name":"\ud83d\u00)",     // ... by a low half cut short by the end
    };
    for (const std::string_view text : malformed)
    {
        EXPECT_FALSE(readReleaseFields(text).has_value()) << text;
    }
}

TEST(UpdateCheckReplyTest, AFieldOfTheWrongTypeIsLeftUnsetAndARepeatedOneKeepsItsLast)
{
    const std::optional<ReleaseFields> wrongRead = readReleaseFields(R"({"tag_name":10,"html_url":null,"draft":"no","prerelease":[true]})");
    ASSERT_TRUE(wrongRead.has_value());
    const ReleaseFields wrong = wrongRead.value_or(ReleaseFields{});
    EXPECT_FALSE(wrong.tagName.has_value());
    EXPECT_FALSE(wrong.htmlUrl.has_value());
    EXPECT_FALSE(wrong.draft.has_value());
    EXPECT_FALSE(wrong.prerelease.has_value());

    const std::optional<ReleaseFields> repeated =
        readReleaseFields(R"({"tag_name":"v1.0.0","draft":true,"tag_name":"v2.0.0","draft":false})");
    ASSERT_TRUE(repeated.has_value());
    EXPECT_EQ(repeated.value_or(ReleaseFields{}).tagName, std::optional<std::string>("v2.0.0"));
    EXPECT_EQ(repeated.value_or(ReleaseFields{}).draft, std::optional<bool>(false));

    const std::optional<ReleaseFields> retypedRead = readReleaseFields(R"({"tag_name":"v1.0.0","tag_name":1})");
    ASSERT_TRUE(retypedRead.has_value());
    const ReleaseFields retyped = retypedRead.value_or(ReleaseFields{});
    EXPECT_FALSE(retyped.tagName.has_value());
}

TEST(UpdateCheckReplyTest, AcceptsAnEmptyObjectAndSurroundingWhitespace)
{
    const std::optional<ReleaseFields> emptyRead = readReleaseFields(" \r\n\t{ }\n ");
    ASSERT_TRUE(emptyRead.has_value());
    const ReleaseFields empty = emptyRead.value_or(ReleaseFields{});
    EXPECT_FALSE(empty.tagName.has_value());
    EXPECT_TRUE(readReleaseFields(R"({"a":{},"b":[],"c":[[],{}],"d":0,"e":-0.5e-3})").has_value());
}

TEST(UpdateCheckReplyTest, RefusesMalformedJson)
{
    const std::vector<std::string_view> malformed = {
        "",
        " ",
        "null",
        "[]",
        R"("tag_name")",
        "{",
        "}",
        R"({"tag_name":"v1.0.0")",
        R"({"tag_name":"v1.0.0"} x)",
        R"({"tag_name":"v1.0.0"}{})",
        R"({"tag_name" "v1.0.0"})",
        R"({"tag_name":})",
        R"({tag_name:"v1.0.0"})",
        R"({'tag_name':'v1.0.0'})",
        R"({"tag_name":"v1.0.0",})",
        R"({,"tag_name":"v1.0.0"})",
        R"({"tag_name":"v1.0.0" "x":1})",
        R"({"tag_name":"unterminated})",
        "{\"tag_name\":\"raw\ncontrol\"}",
        R"({"tag_name":"bad \x escape"})",
        R"({"tag_name":"\u12"})",
        R"({"tag_name":"\uZZZZ"})",
        R"({"tag_name":"\ud83d"})",  // a high surrogate alone
        R"({"tag_name":"\ud83dA"})", // ... followed by no low one
        R"({"tag_name":"\ude00"})",  // a low surrogate alone
        R"({"a":[1,2})",
        R"({"a":[1,2,]})",
        R"({"a":{"b":1]})",
        R"({"a":{"b"}})",
        R"({"a":{"b":1,}})",
        R"({"a":01})",
        R"({"a":1.})",
        R"({"a":.5})",
        R"({"a":1e})",
        R"({"a":-})",
        R"({"a":+1})",
        R"({"a":tru})",
        R"({"a":nul})",
        R"({"a":True})",
        R"({"draft":tru})",
        R"({"a":"x"]})",
    };
    for (const std::string_view text : malformed)
    {
        EXPECT_FALSE(readReleaseFields(text).has_value()) << text;
    }
}

TEST(UpdateCheckReplyTest, DeepNestingIsRefusedWithoutRecursion)
{
    // Far deeper than a recursive reader's stack would survive; refused once past MAX_NESTING, with no recursion.
    const std::size_t depth = 200'000;
    std::string deep = R"({"a":)";
    deep.append(depth, '[');
    deep.append(depth, ']');
    deep += '}';
    EXPECT_FALSE(readReleaseFields(deep).has_value());

    std::string shallow = R"({"a":)";
    shallow.append(32, '[');
    shallow.append(32, ']');
    shallow += R"(,"tag_name":"v1.0.0"})";
    const std::optional<ReleaseFields> fieldsRead = readReleaseFields(shallow);
    ASSERT_TRUE(fieldsRead.has_value());
    const ReleaseFields fields = fieldsRead.value_or(ReleaseFields{});
    EXPECT_EQ(fields.tagName, std::optional<std::string>("v1.0.0"));
}

TEST(UpdateCheckReplyTest, AReplyOverTheLimitIsRefusedAndOneUnderItIsRead)
{
    std::string padded = R"({"tag_name":"v1.0.0","body":")";
    padded.append(MAX_RESPONSE_BYTES - padded.size() - 2, 'x');
    padded += "\"}";
    ASSERT_EQ(padded.size(), MAX_RESPONSE_BYTES);
    const std::optional<ReleaseFields> fieldsRead = readReleaseFields(padded);
    ASSERT_TRUE(fieldsRead.has_value());
    const ReleaseFields fields = fieldsRead.value_or(ReleaseFields{});
    EXPECT_EQ(fields.tagName, std::optional<std::string>("v1.0.0"));

    padded.insert(padded.size() - 2, "x");
    EXPECT_FALSE(readReleaseFields(padded).has_value());
}

RC_GTEST_PROP(UpdateCheckReplyPropertiesTest, ArbitraryBytesNeverCrashTheReader, (const std::string& text))
{
    static_cast<void>(readReleaseFields(text));
    static_cast<void>(readReleaseFields("{\"tag_name\":" + text + "}"));
    static_cast<void>(evaluateResponse(200, text, Version{}));
}

RC_GTEST_PROP(UpdateCheckReplyPropertiesTest, AnyTagWrittenAsAJsonStringIsReadBack, (const std::string& tag))
{
    // Escaped as JSON writes it: quote, backslash and control characters.
    std::string escaped;
    for (const char c : tag)
    {
        const auto byte = static_cast<unsigned char>(c);
        if (c == '"' || c == '\\')
        {
            escaped += '\\';
            escaped += c;
        }
        else if (byte < 0x20U)
        {
            constexpr std::string_view HEX = "0123456789abcdef";
            escaped += "\\u00";
            escaped += HEX[byte >> 4U];
            escaped += HEX[byte & 0xFU];
        }
        else
        {
            escaped += c;
        }
    }
    const std::optional<ReleaseFields> fields = readReleaseFields(R"({"x":[1,{"y":null}],"tag_name":")" + escaped + "\"}");
    RC_ASSERT(fields.has_value());
    RC_ASSERT(fields.value_or(ReleaseFields{}).tagName == std::optional<std::string>(tag));
}

TEST(UpdateCheckReplyTest, OnlyPagesUnderTheReleasesAreLinked)
{
    EXPECT_TRUE(isReleasePageUrl("https://github.com/mgradwohl/tasksmack/releases/tag/v0.10.0"));
    EXPECT_TRUE(isReleasePageUrl("https://github.com/mgradwohl/tasksmack/releases/latest"));
    EXPECT_TRUE(isReleasePageUrl("https://github.com/mgradwohl/tasksmack/releases/tag/v1.2.3-rc.1%2Bx"));

    const std::vector<std::string_view> refused = {
        "",
        "https://github.com/mgradwohl/tasksmack/releases/",
        "https://github.com/mgradwohl/tasksmack/releases",
        "http://github.com/mgradwohl/tasksmack/releases/tag/v0.10.0",
        "https://github.com/mgradwohl/tasksmack/releases/../../../evil",
        "https://github.com/mgradwohl/tasksmack/releases/tag/v1 x",
        "https://github.com/mgradwohl/tasksmack/releases/tag/v1\"",
        "https://github.com/mgradwohl/tasksmack/releases/tag/v1?x=1",
        "https://github.com/mgradwohl/tasksmack/releases/tag/v1#frag",
        "https://github.com.evil.example/mgradwohl/tasksmack/releases/tag/v1",
        "https://evil.example/https://github.com/mgradwohl/tasksmack/releases/tag/v1",
        "HTTPS://GITHUB.COM/mgradwohl/tasksmack/releases/tag/v1",
        "https://github.com/mgradwohl/tasksmack/releases/tag/v1\xC3\xA9",
    };
    for (const std::string_view url : refused)
    {
        EXPECT_FALSE(isReleasePageUrl(url)) << url;
    }
    std::string longUrl(RELEASES_PAGE_PREFIX);
    longUrl.append(MAX_URL_BYTES - longUrl.size(), 'a');
    EXPECT_TRUE(isReleasePageUrl(longUrl));
    longUrl += 'a';
    EXPECT_FALSE(isReleasePageUrl(longUrl));
}

// ---------------------------------------------------------------------------------------------------
// What an answer means
// ---------------------------------------------------------------------------------------------------

TEST(UpdateCheckOutcomeTest, ANewerReleaseIsAvailableWithItsPage)
{
    const Outcome outcome = evaluateResponse(200, GITHUB_REPLY, version("0.9.0"));
    EXPECT_EQ(outcome.kind, Outcome::Kind::Available);
    EXPECT_EQ(outcome.latestVersion, "0.10.0");
    EXPECT_EQ(outcome.releaseUrl, "https://github.com/mgradwohl/tasksmack/releases/tag/v0.10.0");
    EXPECT_TRUE(outcome.detail.empty());
}

TEST(UpdateCheckOutcomeTest, TheSameOrAnOlderReleaseIsUpToDate)
{
    for (const std::string_view current : {"0.10.0", "0.10.1", "1.0.0", "0.10.0+local"})
    {
        const Outcome outcome = evaluateResponse(200, GITHUB_REPLY, version(current));
        EXPECT_EQ(outcome.kind, Outcome::Kind::UpToDate) << current;
        EXPECT_EQ(outcome.latestVersion, "0.10.0") << current;
    }
    // A pre-release of the release that is out is older than it.
    EXPECT_EQ(evaluateResponse(200, GITHUB_REPLY, version("0.10.0-rc.1")).kind, Outcome::Kind::Available);
}

TEST(UpdateCheckOutcomeTest, AForeignPageIsReplacedByTheLatestReleasePage)
{
    const Outcome outcome = evaluateResponse(200, R"({"tag_name":"v9.0.0","html_url":"https://evil.example/"})", version("0.9.0"));
    EXPECT_EQ(outcome.kind, Outcome::Kind::Available);
    EXPECT_EQ(outcome.releaseUrl, LATEST_RELEASE_PAGE);
    EXPECT_EQ(evaluateResponse(200, R"({"tag_name":"v9.0.0"})", version("0.9.0")).releaseUrl, LATEST_RELEASE_PAGE);
}

TEST(UpdateCheckOutcomeTest, AnythingElseFailsWithAReason)
{
    const Version current = version("0.9.0");
    struct Case
    {
        int status;
        std::string_view body;
    };
    const std::vector<Case> cases = {
        {.status = 403, .body = GITHUB_REPLY}, // rate-limited
        {.status = 429, .body = ""},
        {.status = 404, .body = R"({"message":"Not Found"})"},
        {.status = 500, .body = ""},
        {.status = 0, .body = ""},
        {.status = 200, .body = ""},
        {.status = 200, .body = "<html>captive portal</html>"},
        {.status = 200, .body = R"({"message":"no tag"})"},
        {.status = 200, .body = R"({"tag_name":"nightly"})"},
        {.status = 200, .body = R"({"tag_name":"v99.0.0-rc.1"})"},              // a pre-release tag
        {.status = 200, .body = R"({"tag_name":"v99.0.0","prerelease":true})"}, // flagged a pre-release
        {.status = 200, .body = R"({"tag_name":"v99.0.0","draft":true})"},      // a draft
    };
    for (const Case& c : cases)
    {
        const Outcome outcome = evaluateResponse(c.status, c.body, current);
        EXPECT_EQ(outcome.kind, Outcome::Kind::Failed) << c.status << ' ' << c.body;
        EXPECT_FALSE(outcome.detail.empty()) << c.body;
        EXPECT_TRUE(outcome.latestVersion.empty()) << c.body;
        EXPECT_TRUE(outcome.releaseUrl.empty()) << c.body;
    }
    EXPECT_EQ(evaluateResponse(403, "", current).detail, "GitHub answered HTTP 403");
    EXPECT_EQ(failed("offline").detail, "offline");
    EXPECT_EQ(failed("offline").kind, Outcome::Kind::Failed);
}

// ---------------------------------------------------------------------------------------------------
// The schedule, on an injected clock
// ---------------------------------------------------------------------------------------------------

TEST(UpdateCheckScheduleTest, UnixSecondsRoundTripAndZeroMeansNever)
{
    const auto when = std::chrono::system_clock::time_point(std::chrono::seconds(1'761'652'800));
    EXPECT_EQ(toUnixSeconds(when), 1'761'652'800);
    EXPECT_EQ(fromUnixSeconds(1'761'652'800), std::optional(when));
    EXPECT_FALSE(fromUnixSeconds(0).has_value());
    EXPECT_FALSE(fromUnixSeconds(-5).has_value());
    EXPECT_FALSE(fromUnixSeconds(INT64_MAX).has_value()); // no overflow; treated as never
    EXPECT_TRUE(fromUnixSeconds(253'402'300'799).has_value());
}

TEST(UpdateCheckScheduleTest, AtMostOnceADayAndNeverWhileOff)
{
    const auto last = std::chrono::system_clock::time_point(std::chrono::seconds(1'761'652'800));
    const auto day = std::chrono::hours(Sampling::UPDATE_CHECK_INTERVAL_HOURS);
    ASSERT_EQ(day, 24h);

    EXPECT_TRUE(isCheckDue(true, std::nullopt, last)); // never checked
    EXPECT_FALSE(isCheckDue(true, last, last));
    EXPECT_FALSE(isCheckDue(true, last, last + 1h));
    EXPECT_FALSE(isCheckDue(true, last, last + day - 1s));
    EXPECT_TRUE(isCheckDue(true, last, last + day));
    EXPECT_TRUE(isCheckDue(true, last, last + (day * 30)));
    EXPECT_TRUE(isCheckDue(true, last, last - 1s)); // the clock was set back: don't wait for it to catch up

    // Off, nothing is ever due.
    EXPECT_FALSE(isCheckDue(false, std::nullopt, last));
    EXPECT_FALSE(isCheckDue(false, last, last + (day * 30)));
    EXPECT_FALSE(isCheckDue(false, last, last - 1s));
}

// ---------------------------------------------------------------------------------------------------
// The notice
// ---------------------------------------------------------------------------------------------------

Persisted seen(std::string latest, std::string url = "https://github.com/mgradwohl/tasksmack/releases/tag/v0.10.0")
{
    Persisted state;
    state.enabled = true;
    state.latestVersion = std::move(latest);
    state.latestUrl = std::move(url);
    return state;
}

TEST(UpdateCheckNoticeTest, OffersANewerReleaseWhileChecksAreOn)
{
    const std::optional<Notice> noticeRead = noticeFor(seen("0.10.0"), version("0.9.0"));
    ASSERT_TRUE(noticeRead.has_value());
    const Notice notice = noticeRead.value_or(Notice{});
    EXPECT_EQ(notice.version, "0.10.0");
    EXPECT_EQ(notice.url, "https://github.com/mgradwohl/tasksmack/releases/tag/v0.10.0");
    EXPECT_EQ(noticeText(notice.version), "TaskSmack v0.10.0 is available");

    Persisted off = seen("0.10.0");
    off.enabled = false;
    EXPECT_FALSE(noticeFor(off, version("0.9.0")).has_value());
}

TEST(UpdateCheckNoticeTest, NothingForTheSameAnOlderOrAnUnreadableRelease)
{
    const Version current = version("0.10.0");
    EXPECT_FALSE(noticeFor(seen("0.10.0"), current).has_value());
    EXPECT_FALSE(noticeFor(seen("0.9.0"), current).has_value());
    EXPECT_FALSE(noticeFor(seen(""), current).has_value());
    EXPECT_FALSE(noticeFor(seen("garbage"), current).has_value());
    EXPECT_FALSE(noticeFor(seen("0.11.0-rc.1"), current).has_value()); // a pre-release, edited in by hand
}

TEST(UpdateCheckNoticeTest, ADismissedVersionStaysHiddenUntilANewerOne)
{
    Persisted state = seen("0.10.0");
    state.dismissedVersion = "0.10.0";
    EXPECT_FALSE(noticeFor(state, version("0.9.0")).has_value());
    state.latestVersion = "0.10.1";
    EXPECT_TRUE(noticeFor(state, version("0.9.0")).has_value());
    state.dismissedVersion = "garbage"; // unreadable: as if none was dismissed
    EXPECT_TRUE(noticeFor(state, version("0.9.0")).has_value());
}

TEST(UpdateCheckNoticeTest, AForeignStoredPageLinksToTheLatestReleasePage)
{
    const std::optional<Notice> noticeRead = noticeFor(seen("0.10.0", "https://evil.example/"), version("0.9.0"));
    ASSERT_TRUE(noticeRead.has_value());
    const Notice notice = noticeRead.value_or(Notice{});
    EXPECT_EQ(notice.url, LATEST_RELEASE_PAGE);
}

} // namespace
} // namespace Domain::UpdateCheck
