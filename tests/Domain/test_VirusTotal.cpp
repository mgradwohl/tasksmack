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
    EXPECT_EQ(*verdict, (Verdict{.malicious = 2, .suspicious = 1, .undetected = 60, .harmless = 3}));
    EXPECT_EQ(verdict->flagged(), 3U);
    EXPECT_EQ(verdict->engines(), 66U) << "timeouts and unsupported types aren't in the denominator";
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

TEST(JsonReaderTest, ReadsANonNegativeIntegerAndSkipsAnythingElse)
{
    Json::Reader digits("18446744073709551615");
    EXPECT_EQ(digits.readUnsigned(), std::optional<std::uint64_t>(18446744073709551615ULL));
    Json::Reader tooBig("18446744073709551616");
    EXPECT_FALSE(tooBig.readUnsigned().has_value());
    Json::Reader fraction("2.5,");
    EXPECT_FALSE(fraction.readUnsigned().has_value());
    EXPECT_TRUE(fraction.consume(',')) << "the value is skipped whole";
    Json::Reader text(R"("7",)");
    EXPECT_FALSE(text.readUnsigned().has_value());
    EXPECT_TRUE(text.consume(','));
}

} // namespace
} // namespace Domain::VirusTotal
