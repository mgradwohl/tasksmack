#include "Platform/PublicIpLookup.h"

#include "Platform/DnsMessage.h"
#include "Platform/ISystemInfoProbe.h"

#include <chrono>
#include <format>
#include <string>
#include <utility>
#include <variant>

namespace Platform::PublicIpLookup
{

ReplyDisposition replyDisposition(const Answer& answer, bool v6, AddressText addressText)
{
    using Kind = ReplyDisposition::Kind;
    if (const auto* error = std::get_if<DnsMessage::AnswerError>(&answer))
    {
        switch (*error)
        {
        case DnsMessage::AnswerError::NotTheReply:
            return {.kind = Kind::Ignore, .text = {}}; // a stray datagram: keep waiting for the reply
        case DnsMessage::AnswerError::Malformed:
            return {.kind = Kind::Fail, .text = std::string(UNREADABLE_REPLY)};
        case DnsMessage::AnswerError::ServerError:
        case DnsMessage::AnswerError::NameNotFound:
            return {.kind = Kind::Fail, .text = "The resolver answered with an error"};
        case DnsMessage::AnswerError::NoAddress:
            return {.kind = Kind::Fail, .text = v6 ? "No IPv6 address was returned" : "No IPv4 address was returned"};
        }
        return {.kind = Kind::Fail, .text = std::string(UNREADABLE_REPLY)}; // not an AnswerError parseAnswer() gives
    }
    std::string address = addressText(answer);
    if (address.empty())
    {
        return {.kind = Kind::Fail, .text = std::string(UNREADABLE_REPLY)};
    }
    return {.kind = Kind::Address, .text = std::move(address)};
}

void Outcome::fail(std::string reason)
{
    done = true;
    unavailable = std::move(reason);
}

void Outcome::settle(ReplyDisposition reply)
{
    switch (reply.kind)
    {
    case ReplyDisposition::Kind::Ignore:
        return;
    case ReplyDisposition::Kind::Fail:
        fail(std::move(reply.text));
        return;
    case ReplyDisposition::Kind::Address:
        done = true;
        address = std::move(reply.text);
        return;
    }
}

std::string timedOutReason(std::chrono::milliseconds timeout)
{
    return std::format("The resolver didn't answer within {} s", (timeout.count() + 999) / 1000);
}

PublicIpInfo assembleInfo(const Options& options, const Outcome& v4, const Outcome& v6)
{
    const std::string timedOut = timedOutReason(options.timeout);
    PublicIpInfo info;
    info.read = true;
    info.source = options.source;
    info.v4 = v4.address;
    info.v4Unavailable = v4.done ? v4.unavailable : timedOut;
    info.v6 = v6.address;
    info.v6Unavailable = v6.done ? v6.unavailable : timedOut;
    return info;
}

} // namespace Platform::PublicIpLookup
