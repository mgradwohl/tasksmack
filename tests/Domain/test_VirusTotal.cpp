/// @file test_VirusTotal.cpp
/// @brief Look up on VirusTotal's request, reply reader and result line (#3094).

#include "Domain/JsonReader.h"
#include "Domain/VirusTotal.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace Domain::VirusTotal
{
namespace
{

constexpr std::string_view EMPTY_SHA = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";

[[nodiscard]] std::string report(std::string_view stats)
{
    return std::string(R"({"data":{"id":"x","type":"file","links":{"self":"https://x/"},"attributes":{"names":["a","b"],)") +
           R"("last_analysis_results":{"E":{"category":"malicious","result":"Trojan","nested":[1,{"a":null}]}},)" +
           R"("last_analysis_stats":)" + std::string(stats) + "}}}";
}

TEST(VirusTotalTest, TheRequestIsTheHashAndTheKeyIsCheckedFirst)
{
    EXPECT_TRUE(isSha256Hex(EMPTY_SHA));
    EXPECT_FALSE(isSha256Hex("E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855")) << "lowercase only";
    EXPECT_FALSE(isSha256Hex(EMPTY_SHA.substr(1)));
    EXPECT_FALSE(isSha256Hex("../files/e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b8"));
    EXPECT_EQ(filePath(EMPTY_SHA), std::string("/api/v3/files/") + std::string(EMPTY_SHA));
    EXPECT_EQ(reportPage(EMPTY_SHA), std::string("https://www.virustotal.com/gui/file/") + std::string(EMPTY_SHA));

    EXPECT_TRUE(isApiKey(std::string(64, 'a')));
    EXPECT_TRUE(isApiKey(std::string(32, 'A') + std::string(32, '9')));
    EXPECT_FALSE(isApiKey(std::string(63, 'a')));
    EXPECT_FALSE(isApiKey(std::string(63, 'a') + "\n")) << "a line break never reaches a header";
    EXPECT_FALSE(isApiKey(std::string(64, 'g')));
}

TEST(VirusTotalTest, ReadsTheLastAnalysisStatsAndSkipsTheRest)
{
    const std::optional<Verdict> verdict =
        readFileReport(report(R"({"malicious":2,"suspicious":1,"undetected":60,"harmless":3,"timeout":0,"type-unsupported":9})"));
    ASSERT_TRUE(verdict.has_value());
    // A plain value: bugprone-unchecked-optional-access doesn't model ASSERT_TRUE's early return.
    const Verdict read = verdict.value_or(Verdict{});
    EXPECT_EQ(read, (Verdict{.malicious = 2, .suspicious = 1, .undetected = 60, .harmless = 3}));
    EXPECT_EQ(read.flagged(), 3U);
    EXPECT_EQ(read.engines(), 66U) << "timeouts and unsupported types aren't in the denominator";
}

TEST(VirusTotalTest, ARepliesWithoutTheFourCountsIsNotAReport)
{
    EXPECT_FALSE(readFileReport(report(R"({"malicious":2,"suspicious":1,"undetected":60})")).has_value()) << "harmless missing";
    EXPECT_FALSE(readFileReport(report(R"({"malicious":-1,"suspicious":1,"undetected":60,"harmless":0})")).has_value());
    EXPECT_FALSE(readFileReport(report(R"({"malicious":1.5,"suspicious":1,"undetected":60,"harmless":0})")).has_value());
    EXPECT_FALSE(readFileReport(report(R"({"malicious":"2","suspicious":1,"undetected":60,"harmless":0})")).has_value());
    EXPECT_FALSE(readFileReport(R"({"error":{"code":"NotFoundError","message":"not found"}})").has_value());
    EXPECT_FALSE(readFileReport(R"({"data":"not an object"})").has_value());
    EXPECT_FALSE(readFileReport(report(R"({"malicious":2,"suspicious":1,"undetected":60,"harmless":0})") + " trailing").has_value());
    EXPECT_FALSE(readFileReport("").has_value());
    EXPECT_FALSE(readFileReport(std::string(MAX_RESPONSE_BYTES + 1, ' ')).has_value());
    // Deeper than the reader allows anywhere is refused, not recursed into.
    EXPECT_FALSE(readFileReport(std::string(R"({"data":{"x":)") + std::string(100, '[') + std::string(100, ']') + "}}").has_value());
}

TEST(VirusTotalTest, EachStatusReadsAsItsOutcomeAndLine)
{
    const std::string found = report(R"({"malicious":2,"suspicious":1,"undetected":60,"harmless":3})");
    const LookupResult flagged = interpret(200, found);
    EXPECT_EQ(flagged.outcome, Outcome::Found);
    EXPECT_EQ(summary("notepad.exe", flagged), "VirusTotal: 3 of 66 engines flag notepad.exe (2 malicious, 1 suspicious)");

    const LookupResult clean = interpret(200, report(R"({"malicious":0,"suspicious":0,"undetected":70,"harmless":0})"));
    EXPECT_EQ(summary("bash", clean), "VirusTotal: no engine flags bash (0 of 70)");

    EXPECT_EQ(interpret(404, "{}").outcome, Outcome::Unknown);
    EXPECT_EQ(summary("bash", interpret(404, "{}")), "VirusTotal doesn't know bash's file: nobody has submitted it");
    EXPECT_EQ(interpret(401, "").outcome, Outcome::KeyRefused);
    EXPECT_EQ(interpret(403, "").outcome, Outcome::KeyRefused);
    EXPECT_EQ(summary("bash", interpret(401, "")), "VirusTotal refused the API key: check it in Settings");
    EXPECT_EQ(interpret(429, "").outcome, Outcome::RateLimited);
    EXPECT_EQ(summary("bash", interpret(500, "")), "Couldn't look up bash on VirusTotal: it answered HTTP 500");
    EXPECT_EQ(summary("bash", interpret(200, "not json")), "Couldn't look up bash on VirusTotal: its reply couldn't be read");
}

// #3874: counts that would wrap round to 0 flags, or more engines than any report has, are a failure, never clean.
TEST(VirusTotalTest, HugeCountsAreNeverReadAsNoEngineFlags)
{
    constexpr std::uint64_t MAX = UINT64_MAX;
    const Verdict wraps{.malicious = MAX, .suspicious = 1, .undetected = 0, .harmless = 0};
    EXPECT_EQ(wraps.flagged(), MAX) << "saturates rather than wrapping to 0";
    EXPECT_EQ(wraps.engines(), MAX);
    EXPECT_FALSE(wraps.plausible());
    const Verdict allHuge{.malicious = MAX, .suspicious = MAX, .undetected = MAX, .harmless = MAX};
    EXPECT_EQ(allHuge.flagged(), MAX);
    EXPECT_EQ(allHuge.engines(), MAX);
    const Verdict undetectedWraps{.malicious = 0, .suspicious = 0, .undetected = MAX, .harmless = 2};
    EXPECT_EQ(undetectedWraps.flagged(), 0U);
    EXPECT_EQ(undetectedWraps.engines(), MAX);
    EXPECT_FALSE(undetectedWraps.plausible());

    EXPECT_TRUE((Verdict{.malicious = 0, .suspicious = 0, .undetected = MAX_ENGINES, .harmless = 0}).plausible());
    EXPECT_FALSE((Verdict{.malicious = 1, .suspicious = 0, .undetected = MAX_ENGINES, .harmless = 0}).plausible());

    // In a reply: 18446744073709551615 + 1 malicious and suspicious wrapped to "no engine flags" before.
    const std::string hostile = report(R"({"malicious":18446744073709551615,"suspicious":1,"undetected":70,"harmless":0})");
    EXPECT_FALSE(readFileReport(hostile).has_value());
    const LookupResult read = interpret(200, hostile);
    EXPECT_EQ(read.outcome, Outcome::Failed);
    EXPECT_EQ(summary("evil.exe", read), "Couldn't look up evil.exe on VirusTotal: its reply couldn't be read");
    // Each count in range, but more engines together than any report has.
    EXPECT_FALSE(readFileReport(report(R"({"malicious":0,"suspicious":0,"undetected":9000,"harmless":1001})")).has_value());
    EXPECT_TRUE(readFileReport(report(R"({"malicious":0,"suspicious":0,"undetected":9000,"harmless":1000})")).has_value());

    // A Found result carrying such a verdict anyway is a failure line, not a verdict.
    const LookupResult forged{.outcome = Outcome::Found, .verdict = wraps, .detail = {}};
    EXPECT_EQ(summary("evil.exe", forged), "Couldn't look up evil.exe on VirusTotal: its reply couldn't be read");
}

TEST(JsonReaderTest, ReadsANonNegativeIntegerAndSkipsAnythingElse)
{
    std::optional<std::uint64_t> value;
    Json::Reader digits("18446744073709551615");
    EXPECT_TRUE(digits.readUnsigned(value));
    EXPECT_EQ(value, std::optional<std::uint64_t>(18446744073709551615ULL));
    Json::Reader tooBig("18446744073709551616");
    EXPECT_TRUE(tooBig.readUnsigned(value)) << "well formed, just out of range";
    EXPECT_FALSE(value.has_value());
    Json::Reader fraction("2.5,");
    EXPECT_TRUE(fraction.readUnsigned(value));
    EXPECT_FALSE(value.has_value());
    EXPECT_TRUE(fraction.consume(',')) << "the value is skipped whole";
    Json::Reader text(R"("7",)");
    EXPECT_TRUE(text.readUnsigned(value));
    EXPECT_FALSE(value.has_value());
    EXPECT_TRUE(text.consume(','));
    Json::Reader zero("0");
    EXPECT_TRUE(zero.readUnsigned(value));
    EXPECT_EQ(value, std::optional<std::uint64_t>(0));
}

// #3795: a number with a leading zero isn't JSON, and a value that can't be skipped is malformed: both say so, rather
// than reading as a skipped count.
TEST(JsonReaderTest, ALeadingZeroOrAnUnskippableValueIsMalformed)
{
    std::optional<std::uint64_t> value;
    Json::Reader leadingZero("007");
    EXPECT_FALSE(leadingZero.readUnsigned(value));
    Json::Reader broken(R"({"a": )");
    EXPECT_FALSE(broken.readUnsigned(value));
    Json::Reader unclosed("[1, 2");
    EXPECT_FALSE(unclosed.readUnsigned(value));
    EXPECT_FALSE(value.has_value());
}

// #3795: a reply whose count is malformed isn't read as if its other counts were the verdict.
TEST(JsonReaderTest, AReportWithALeadingZeroCountCantBeRead)
{
    const std::string body =
        R"({"data":{"attributes":{"last_analysis_stats":{"malicious":01,"suspicious":0,"undetected":60,"harmless":5}}}})";
    EXPECT_EQ(summary("bash", interpret(200, body)), "Couldn't look up bash on VirusTotal: its reply couldn't be read");
}

} // namespace
} // namespace Domain::VirusTotal
