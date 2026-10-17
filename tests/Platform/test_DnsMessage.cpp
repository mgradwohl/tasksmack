/// @file test_DnsMessage.cpp
/// @brief The public IP lookup's DNS codec (#1518): the query as built (header, labels, type, class),
/// names it refuses, and the reply parsed: the address of the type asked for, compressed names, records
/// skipped, and the replies it refuses (another ID, another question, truncated, errors, overruns,
/// pointer loops).

#include "Platform/DnsMessage.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <initializer_list>
#include <string>
#include <variant>
#include <vector>

namespace Platform
{
namespace
{

using DnsMessage::AnswerError;
using DnsMessage::buildQuery;
using DnsMessage::Ipv4;
using DnsMessage::Ipv6;
using DnsMessage::parseAnswer;
using DnsMessage::RecordType;
using Bytes = std::vector<std::uint8_t>;

/// A reply to @p query: its header with QR and RA set, @p rcode, @p answerCount answers; the question
/// echoed; then @p answers appended as given.
[[nodiscard]] Bytes reply(const Bytes& query, std::uint16_t answerCount, const Bytes& answers, std::uint8_t rcode = 0)
{
    Bytes out(query);
    out[2] = static_cast<std::uint8_t>(0x80U | out[2]); // QR
    out[3] = static_cast<std::uint8_t>(0x80U | rcode);  // RA
    out[6] = static_cast<std::uint8_t>(answerCount >> 8U);
    out[7] = static_cast<std::uint8_t>(answerCount & 0xFFU);
    out.insert(out.end(), answers.begin(), answers.end());
    return out;
}

/// One answer record named by a pointer to the question's name (offset 12).
[[nodiscard]] Bytes record(std::uint16_t type, const Bytes& data, std::uint16_t klass = 1)
{
    Bytes out{0xC0, 0x0C};
    out.push_back(static_cast<std::uint8_t>(type >> 8U));
    out.push_back(static_cast<std::uint8_t>(type & 0xFFU));
    out.push_back(static_cast<std::uint8_t>(klass >> 8U));
    out.push_back(static_cast<std::uint8_t>(klass & 0xFFU));
    out.insert(out.end(), {0x00, 0x00, 0x00, 0x3C}); // TTL 60
    out.push_back(static_cast<std::uint8_t>(data.size() >> 8U));
    out.push_back(static_cast<std::uint8_t>(data.size() & 0xFFU));
    out.insert(out.end(), data.begin(), data.end());
    return out;
}

[[nodiscard]] Bytes concat(std::initializer_list<Bytes> parts)
{
    Bytes out;
    for (const Bytes& part : parts)
    {
        out.insert(out.end(), part.begin(), part.end());
    }
    return out;
}

TEST(DnsMessageTest, BuildsTheQuery)
{
    const Bytes query = buildQuery(0xBEEF, "myip.opendns.com", RecordType::A);
    const Bytes expected{0xBE, 0xEF, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // header, RD, QDCOUNT 1
                         4,    'm',  'y',  'i',  'p',  7,    'o',  'p',  'e',  'n',  'd',  'n',
                         's',  3,    'c',  'o',  'm',  0,    0x00, 0x01, 0x00, 0x01}; // A, IN
    EXPECT_EQ(query, expected);
    EXPECT_EQ(buildQuery(0xBEEF, "myip.opendns.com.", RecordType::A), expected) << "a trailing dot is allowed";
    const Bytes aaaa = buildQuery(1, "a.b", RecordType::AAAA);
    ASSERT_GE(aaaa.size(), 4U);
    EXPECT_EQ(aaaa[aaaa.size() - 3], 28) << "QTYPE AAAA";
}

TEST(DnsMessageTest, RefusesNamesItCantEncode)
{
    EXPECT_TRUE(buildQuery(1, "", RecordType::A).empty());
    EXPECT_TRUE(buildQuery(1, ".", RecordType::A).empty());
    EXPECT_TRUE(buildQuery(1, "a..b", RecordType::A).empty());
    EXPECT_TRUE(buildQuery(1, ".a", RecordType::A).empty());
    EXPECT_TRUE(buildQuery(1, std::string(64, 'x'), RecordType::A).empty()) << "a label over 63 bytes";
    EXPECT_FALSE(buildQuery(1, std::string(63, 'x'), RecordType::A).empty());
    std::string longName;
    for (int label = 0; label < 5; ++label)
    {
        longName += std::string(60, 'x') + ".";
    }
    EXPECT_TRUE(buildQuery(1, longName, RecordType::A).empty()) << "a name over 255 bytes";
}

TEST(DnsMessageTest, ReadsTheAddressOfTheTypeAsked)
{
    const Bytes query4 = buildQuery(7, "myip.opendns.com", RecordType::A);
    const auto v4 = parseAnswer(reply(query4, 1, record(1, {203, 0, 113, 7})), query4);
    ASSERT_TRUE(std::holds_alternative<Ipv4>(v4));
    EXPECT_EQ(std::get<Ipv4>(v4), (Ipv4{203, 0, 113, 7}));

    const Bytes query6 = buildQuery(8, "myip.opendns.com", RecordType::AAAA);
    const Ipv6 address{0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 7};
    const auto v6 = parseAnswer(reply(query6, 1, record(28, Bytes(address.begin(), address.end()))), query6);
    ASSERT_TRUE(std::holds_alternative<Ipv6>(v6));
    EXPECT_EQ(std::get<Ipv6>(v6), address);
}

TEST(DnsMessageTest, SkipsOtherRecordsAndFollowsUncompressedNames)
{
    const Bytes query = buildQuery(9, "myip.opendns.com", RecordType::A);
    // A CNAME (type 5) first, then an A of another class, then the A in an uncompressed name.
    Bytes plainName{4, 'm', 'y', 'i', 'p', 7, 'o', 'p', 'e', 'n', 'd', 'n', 's', 3, 'c', 'o', 'm', 0};
    Bytes uncompressed = record(1, {198, 51, 100, 1});
    uncompressed.erase(uncompressed.begin(), uncompressed.begin() + 2);
    uncompressed.insert(uncompressed.begin(), plainName.begin(), plainName.end());
    const Bytes answers = concat({record(5, {0xC0, 0x0C}), record(1, {10, 0, 0, 1}, /*klass=*/3), uncompressed});
    const auto answer = parseAnswer(reply(query, 3, answers), query);
    ASSERT_TRUE(std::holds_alternative<Ipv4>(answer));
    EXPECT_EQ(std::get<Ipv4>(answer), (Ipv4{198, 51, 100, 1}));
}

TEST(DnsMessageTest, RefusesAReplyToAnotherQuery)
{
    const Bytes query = buildQuery(10, "myip.opendns.com", RecordType::A);
    Bytes otherId = reply(query, 1, record(1, {1, 2, 3, 4}));
    otherId[1] = 11;
    EXPECT_EQ(std::get<AnswerError>(parseAnswer(otherId, query)), AnswerError::NotTheReply);

    Bytes notAResponse = query; // QR clear: the query itself, reflected
    EXPECT_EQ(std::get<AnswerError>(parseAnswer(notAResponse, query)), AnswerError::NotTheReply);

    const Bytes otherName = buildQuery(10, "myip.example.com", RecordType::A);
    EXPECT_EQ(std::get<AnswerError>(parseAnswer(reply(otherName, 1, record(1, {1, 2, 3, 4})), query)), AnswerError::NotTheReply);

    const Bytes otherType = buildQuery(10, "myip.opendns.com", RecordType::AAAA);
    EXPECT_EQ(std::get<AnswerError>(parseAnswer(reply(otherType, 1, record(1, {1, 2, 3, 4})), query)), AnswerError::NotTheReply);

    Bytes noQuestion = reply(query, 0, {});
    noQuestion[5] = 0;
    EXPECT_EQ(std::get<AnswerError>(parseAnswer(noQuestion, query)), AnswerError::NotTheReply);
}

TEST(DnsMessageTest, ReportsErrorsAndNoAddress)
{
    const Bytes query = buildQuery(12, "myip.opendns.com", RecordType::A);
    EXPECT_EQ(std::get<AnswerError>(parseAnswer(reply(query, 0, {}, /*rcode=*/3), query)), AnswerError::NameNotFound);
    EXPECT_EQ(std::get<AnswerError>(parseAnswer(reply(query, 0, {}, /*rcode=*/2), query)), AnswerError::ServerError);
    EXPECT_EQ(std::get<AnswerError>(parseAnswer(reply(query, 0, {}), query)), AnswerError::NoAddress);
    EXPECT_EQ(std::get<AnswerError>(parseAnswer(reply(query, 1, record(5, {0xC0, 0x0C})), query)), AnswerError::NoAddress)
        << "only a CNAME";
}

TEST(DnsMessageTest, RefusesMalformedReplies)
{
    const Bytes query = buildQuery(13, "myip.opendns.com", RecordType::A);
    EXPECT_EQ(std::get<AnswerError>(parseAnswer(Bytes{0x00, 0x0D}, query)), AnswerError::Malformed) << "shorter than a header";
    EXPECT_EQ(std::get<AnswerError>(parseAnswer(reply(query, 0, {}), Bytes{})), AnswerError::Malformed) << "no query";
    // #2172: a query with less than a whole question (fuzz_dns_message found the out-of-bounds read of
    // its type). The reply here repeats it, as a reply to such a query would.
    const Bytes shortQuery{0x12, 0x34, 0x81, 0x80, 0x00, 0x01, 0x00, 0x00, 0x0D, 0x01, 0x00, 0x00, 0x83};
    EXPECT_EQ(std::get<AnswerError>(parseAnswer(shortQuery, shortQuery)), AnswerError::Malformed) << "query shorter than a question";

    Bytes truncated = reply(query, 1, record(1, {1, 2, 3, 4}));
    truncated[2] = static_cast<std::uint8_t>(truncated[2] | 0x02U);
    EXPECT_EQ(std::get<AnswerError>(parseAnswer(truncated, query)), AnswerError::Malformed) << "TC set";

    Bytes promised = reply(query, 2, record(5, {0xC0, 0x0C}));
    EXPECT_EQ(std::get<AnswerError>(parseAnswer(promised, query)), AnswerError::Malformed) << "a second answer promised but missing";

    Bytes overrun = reply(query, 1, record(1, {1, 2, 3, 4}));
    overrun.pop_back();
    EXPECT_EQ(std::get<AnswerError>(parseAnswer(overrun, query)), AnswerError::Malformed) << "RDATA past the end";

    EXPECT_EQ(std::get<AnswerError>(parseAnswer(reply(query, 1, record(1, {1, 2, 3})), query)), AnswerError::Malformed)
        << "an A record of 3 bytes";

    Bytes loop = record(1, {1, 2, 3, 4});
    const auto at = static_cast<std::uint8_t>(query.size()); // the answer's own offset: a pointer to itself
    loop[1] = at;
    EXPECT_EQ(std::get<AnswerError>(parseAnswer(reply(query, 1, loop), query)), AnswerError::Malformed) << "a pointer loop";

    Bytes reserved = record(1, {1, 2, 3, 4});
    reserved[0] = 0x40; // a reserved label type
    EXPECT_EQ(std::get<AnswerError>(parseAnswer(reply(query, 1, reserved), query)), AnswerError::Malformed);
}

} // namespace
} // namespace Platform
