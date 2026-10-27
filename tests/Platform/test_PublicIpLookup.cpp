/// @file test_PublicIpLookup.cpp
/// @brief What the Linux and Windows public IP lookups share (#3117): the default resolvers, what each
/// reply DnsMessage::parseAnswer() can give means for its query (a stray datagram ignored, each error's
/// reason, an address as its text, an address with no text unreadable), a query settled by it, and the
/// result assembled from the two queries with the timeout's reason.

#include "Platform/DnsMessage.h"
#include "Platform/ISystemInfoProbe.h"
#include "Platform/PublicIpLookup.h"

#include <gtest/gtest.h>

#include <chrono>
#include <format>
#include <initializer_list>
#include <string>
#include <variant>

namespace Platform
{
namespace
{

using DnsMessage::AnswerError;
using DnsMessage::Ipv4;
using DnsMessage::Ipv6;
using PublicIpLookup::Answer;
using PublicIpLookup::Outcome;
using PublicIpLookup::replyDisposition;
using Kind = PublicIpLookup::ReplyDisposition::Kind;

/// Stands in for the platform's inet_ntop(): an IPv4 address dotted, any IPv6 one "2001:db8::7".
[[nodiscard]] std::string fakeText(const Answer& answer)
{
    if (const auto* v4 = std::get_if<Ipv4>(&answer))
    {
        return std::format("{}.{}.{}.{}",
                           static_cast<unsigned>(v4->at(0)),
                           static_cast<unsigned>(v4->at(1)),
                           static_cast<unsigned>(v4->at(2)),
                           static_cast<unsigned>(v4->at(3)));
    }
    if (std::holds_alternative<Ipv6>(answer))
    {
        return "2001:db8::7";
    }
    return "an error reached the address text";
}

/// An inet_ntop() that couldn't make the text.
[[nodiscard]] std::string noText(const Answer& /*answer*/)
{
    return {};
}

TEST(PublicIpLookupTest, DefaultsAskOpenDns)
{
    const PublicIpLookup::Options options;
    EXPECT_EQ(options.v4Resolver.address, "208.67.222.222");
    EXPECT_EQ(options.v4Resolver.port, 53);
    EXPECT_EQ(options.v6Resolver.address, "2620:119:35::35");
    EXPECT_EQ(options.v6Resolver.port, 53);
    EXPECT_EQ(options.name, "myip.opendns.com");
    EXPECT_EQ(options.timeout, std::chrono::milliseconds(2000));
    EXPECT_EQ(options.source, "OpenDNS (myip.opendns.com)");
    EXPECT_EQ(PublicIpLookup::Resolver{}.port, 53);
}

TEST(PublicIpLookupTest, AStrayDatagramIsIgnored)
{
    for (const bool v6 : {false, true})
    {
        const auto reply = replyDisposition(Answer{AnswerError::NotTheReply}, v6, fakeText);
        EXPECT_EQ(reply.kind, Kind::Ignore) << "v6 " << v6;
        EXPECT_TRUE(reply.text.empty());
    }
}

TEST(PublicIpLookupTest, EachErrorGivesTheReasonShown)
{
    for (const bool v6 : {false, true})
    {
        const auto malformed = replyDisposition(Answer{AnswerError::Malformed}, v6, fakeText);
        EXPECT_EQ(malformed.kind, Kind::Fail);
        EXPECT_EQ(malformed.text, "The resolver's reply couldn't be read");
        EXPECT_EQ(malformed.text, PublicIpLookup::UNREADABLE_REPLY);

        const auto serverError = replyDisposition(Answer{AnswerError::ServerError}, v6, fakeText);
        EXPECT_EQ(serverError.kind, Kind::Fail);
        EXPECT_EQ(serverError.text, "The resolver answered with an error");

        const auto notFound = replyDisposition(Answer{AnswerError::NameNotFound}, v6, fakeText);
        EXPECT_EQ(notFound.kind, Kind::Fail);
        EXPECT_EQ(notFound.text, "The resolver answered with an error") << "NXDOMAIN reads as any other error";
    }

    const auto noV4 = replyDisposition(Answer{AnswerError::NoAddress}, false, fakeText);
    EXPECT_EQ(noV4.kind, Kind::Fail);
    EXPECT_EQ(noV4.text, "No IPv4 address was returned");
    const auto noV6 = replyDisposition(Answer{AnswerError::NoAddress}, true, fakeText);
    EXPECT_EQ(noV6.kind, Kind::Fail);
    EXPECT_EQ(noV6.text, "No IPv6 address was returned");
}

TEST(PublicIpLookupTest, AnAddressIsItsText)
{
    const auto v4 = replyDisposition(Answer{Ipv4{203, 0, 113, 7}}, false, fakeText);
    EXPECT_EQ(v4.kind, Kind::Address);
    EXPECT_EQ(v4.text, "203.0.113.7");

    const auto v6 = replyDisposition(Answer{Ipv6{0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 7}}, true, fakeText);
    EXPECT_EQ(v6.kind, Kind::Address);
    EXPECT_EQ(v6.text, "2001:db8::7");
}

TEST(PublicIpLookupTest, AnAddressWithoutTextIsUnreadable)
{
    for (const Answer& answer : {Answer{Ipv4{203, 0, 113, 7}}, Answer{Ipv6{}}})
    {
        const auto reply = replyDisposition(answer, std::holds_alternative<Ipv6>(answer), noText);
        EXPECT_EQ(reply.kind, Kind::Fail);
        EXPECT_EQ(reply.text, "The resolver's reply couldn't be read");
    }
}

TEST(PublicIpLookupTest, SettlingEndsTheQueryUnlessTheReplyIsIgnored)
{
    Outcome query;
    query.settle({.kind = Kind::Ignore, .text = {}});
    EXPECT_FALSE(query.done) << "still waiting for its reply";
    EXPECT_TRUE(query.address.empty());
    EXPECT_TRUE(query.unavailable.empty());

    query.settle(replyDisposition(Answer{Ipv4{203, 0, 113, 7}}, false, fakeText));
    EXPECT_TRUE(query.done);
    EXPECT_EQ(query.address, "203.0.113.7");
    EXPECT_TRUE(query.unavailable.empty());

    Outcome failed;
    failed.settle(replyDisposition(Answer{AnswerError::ServerError}, false, fakeText));
    EXPECT_TRUE(failed.done);
    EXPECT_TRUE(failed.address.empty());
    EXPECT_EQ(failed.unavailable, "The resolver answered with an error");

    Outcome refused;
    refused.fail("The resolver refused the query");
    EXPECT_TRUE(refused.done);
    EXPECT_EQ(refused.unavailable, "The resolver refused the query");
}

TEST(PublicIpLookupTest, AssemblesTheResultFromBothQueries)
{
    PublicIpLookup::Options options;
    options.source = "the fake";
    Outcome v4;
    v4.settle({.kind = Kind::Address, .text = "203.0.113.7"});
    Outcome v6;
    v6.fail("No IPv6 route to the internet");

    const PublicIpInfo info = PublicIpLookup::assembleInfo(options, v4, v6);
    EXPECT_TRUE(info.read);
    EXPECT_EQ(info.source, "the fake");
    EXPECT_EQ(info.v4, "203.0.113.7");
    EXPECT_TRUE(info.v4Unavailable.empty());
    EXPECT_TRUE(info.v6.empty());
    EXPECT_EQ(info.v6Unavailable, "No IPv6 route to the internet");
}

TEST(PublicIpLookupTest, AQueryNotDoneTimedOut)
{
    PublicIpLookup::Options options;
    options.timeout = std::chrono::milliseconds(1500);
    const Outcome waiting;
    const PublicIpInfo info = PublicIpLookup::assembleInfo(options, waiting, waiting);
    EXPECT_TRUE(info.read);
    EXPECT_EQ(info.source, "OpenDNS (myip.opendns.com)");
    EXPECT_TRUE(info.v4.empty());
    EXPECT_TRUE(info.v6.empty());
    EXPECT_EQ(info.v4Unavailable, "The resolver didn't answer within 2 s") << "rounded up to whole seconds";
    EXPECT_EQ(info.v6Unavailable, "The resolver didn't answer within 2 s");

    EXPECT_EQ(PublicIpLookup::timedOutReason(std::chrono::milliseconds(2000)), "The resolver didn't answer within 2 s");
    EXPECT_EQ(PublicIpLookup::timedOutReason(std::chrono::milliseconds(500)), "The resolver didn't answer within 1 s");
    EXPECT_EQ(PublicIpLookup::timedOutReason(std::chrono::milliseconds(2001)), "The resolver didn't answer within 3 s");
}

} // namespace
} // namespace Platform
