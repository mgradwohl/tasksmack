#pragma once

// A minimal DNS message codec (RFC 1035) for the public IP lookup (#1518): an A or AAAA query for one
// name, and the first matching address out of the reply. Pure -- no sockets -- so it is unit-tested on
// any host; the platform's reader sends the bytes. Only what that lookup needs: one question, answers
// read by type, name compression followed, everything else (authority, additional) ignored.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

namespace Platform::DnsMessage
{

/// The record types asked for: their QTYPE values on the wire.
enum class RecordType : std::uint16_t // NOLINT(performance-enum-size): QTYPE is 16 bits on the wire
{
    A = 1,     ///< An IPv4 address
    AAAA = 28, ///< An IPv6 address
};

using Ipv4 = std::array<std::uint8_t, 4>;
using Ipv6 = std::array<std::uint8_t, 16>;

/// Why a reply gave no address.
enum class AnswerError : std::uint8_t
{
    Malformed,    ///< Too short, truncated, a bad name, or a record past the end
    NotTheReply,  ///< Not a response to this query (its ID, or the question, differs)
    ServerError,  ///< The server answered with an error (RCODE other than NXDOMAIN)
    NameNotFound, ///< NXDOMAIN
    NoAddress,    ///< A good reply, but no record of the type asked for
};

namespace Detail
{

constexpr std::size_t HEADER_SIZE = 12;
constexpr std::uint16_t CLASS_IN = 1;
constexpr std::size_t MAX_LABEL = 63;
constexpr std::size_t MAX_NAME = 255;
/// Pointer hops followed in one name before it is taken for a loop.
constexpr int MAX_POINTERS = 16;

[[nodiscard]] constexpr std::uint16_t read16(std::span<const std::uint8_t> bytes, std::size_t at) noexcept
{
    return static_cast<std::uint16_t>((static_cast<unsigned>(bytes[at]) << 8U) | static_cast<unsigned>(bytes[at + 1]));
}

/// The offset just past the (possibly compressed) name starting at @p at, or nullopt for a malformed
/// one. A compressed name ends at its first pointer, whatever it points at; the pointers are followed
/// only to check they lead to a well-formed name.
[[nodiscard]] inline std::optional<std::size_t> skipName(std::span<const std::uint8_t> bytes, std::size_t at)
{
    std::optional<std::size_t> end;
    std::size_t length = 0;
    int pointers = 0;
    while (at < bytes.size())
    {
        const std::uint8_t lengthByte = bytes[at];
        if ((lengthByte & 0xC0U) == 0xC0U)
        {
            if (at + 1 >= bytes.size() || ++pointers > MAX_POINTERS)
            {
                return std::nullopt;
            }
            if (!end.has_value())
            {
                end = at + 2;
            }
            at = static_cast<std::size_t>(((lengthByte & 0x3FU) << 8U) | bytes[at + 1]);
            continue;
        }
        if ((lengthByte & 0xC0U) != 0)
        {
            return std::nullopt; // 0x40 and 0x80: reserved label types
        }
        if (lengthByte == 0)
        {
            return end.value_or(at + 1);
        }
        length += static_cast<std::size_t>(lengthByte) + 1;
        if (lengthByte > MAX_LABEL || length > MAX_NAME)
        {
            return std::nullopt;
        }
        at += static_cast<std::size_t>(lengthByte) + 1;
    }
    return std::nullopt;
}

} // namespace Detail

/// The query asking for @p type records of @p name ("myip.opendns.com"), with ID @p id and recursion
/// desired. Empty when the name can't be encoded: an empty label, a label over 63 bytes, or a name over
/// 255. A trailing dot is allowed.
[[nodiscard]] inline std::vector<std::uint8_t> buildQuery(std::uint16_t id, std::string_view name, RecordType type)
{
    if (name.ends_with('.'))
    {
        name.remove_suffix(1);
    }
    std::vector<std::uint8_t> query{
        static_cast<std::uint8_t>(id >> 8U),
        static_cast<std::uint8_t>(id & 0xFFU),
        0x01, // RD: recursion desired
        0x00,
        0x00,
        0x01, // QDCOUNT 1
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
    };
    std::size_t encoded = 0;
    while (!name.empty())
    {
        const std::size_t dot = name.find('.');
        const std::string_view label = name.substr(0, dot);
        if (label.empty() || label.size() > Detail::MAX_LABEL)
        {
            return {};
        }
        encoded += label.size() + 1;
        if (encoded > Detail::MAX_NAME)
        {
            return {};
        }
        query.push_back(static_cast<std::uint8_t>(label.size()));
        query.insert(query.end(), label.begin(), label.end());
        name = (dot == std::string_view::npos) ? std::string_view{} : name.substr(dot + 1);
    }
    if (encoded == 0)
    {
        return {};
    }
    query.push_back(0x00);
    const auto qtype = static_cast<std::uint16_t>(type);
    query.push_back(static_cast<std::uint8_t>(qtype >> 8U));
    query.push_back(static_cast<std::uint8_t>(qtype & 0xFFU));
    query.push_back(0x00);
    query.push_back(static_cast<std::uint8_t>(Detail::CLASS_IN));
    return query;
}

/// The first address of the type asked for in @p reply, a response to @p query (as buildQuery() made
/// it): an Ipv4 for an A query, an Ipv6 for AAAA. Or why there is none. The reply must carry the query's
/// ID and repeat its question, so a stray or spoofed datagram for another lookup is never taken.
[[nodiscard]] inline std::variant<Ipv4, Ipv6, AnswerError> parseAnswer(std::span<const std::uint8_t> reply,
                                                                       std::span<const std::uint8_t> query)
{
    using Detail::read16;
    if (query.size() <= Detail::HEADER_SIZE || reply.size() < Detail::HEADER_SIZE)
    {
        return AnswerError::Malformed;
    }
    if (read16(reply, 0) != read16(query, 0) || (reply[2] & 0x80U) == 0)
    {
        return AnswerError::NotTheReply; // another ID, or not a response (QR clear)
    }
    if ((reply[2] & 0x02U) != 0)
    {
        return AnswerError::Malformed; // TC: truncated
    }
    const unsigned rcode = reply[3] & 0x0FU;
    if (rcode == 3)
    {
        return AnswerError::NameNotFound;
    }
    if (rcode != 0)
    {
        return AnswerError::ServerError;
    }
    if (read16(reply, 4) != 1)
    {
        return AnswerError::NotTheReply; // one question asked, one must come back
    }
    // The question, byte for byte as asked (DNS names compare case-insensitively, but a server echoes
    // the query's bytes; anything else is not this lookup's reply).
    const std::span<const std::uint8_t> question = query.subspan(Detail::HEADER_SIZE);
    if (reply.size() < Detail::HEADER_SIZE + question.size() ||
        !std::equal(question.begin(), question.end(), reply.begin() + static_cast<std::ptrdiff_t>(Detail::HEADER_SIZE)))
    {
        return AnswerError::NotTheReply;
    }
    const auto asked = static_cast<RecordType>(read16(question, question.size() - 4));
    std::size_t at = Detail::HEADER_SIZE + question.size();
    const std::uint16_t answers = read16(reply, 6);
    for (std::uint16_t answer = 0; answer < answers; ++answer)
    {
        const std::optional<std::size_t> afterName = Detail::skipName(reply, at);
        if (!afterName.has_value() || *afterName + 10 > reply.size())
        {
            return AnswerError::Malformed;
        }
        at = *afterName;
        const std::uint16_t type = read16(reply, at);
        const std::uint16_t klass = read16(reply, at + 2);
        const std::uint16_t dataLength = read16(reply, at + 8);
        at += 10;
        if (at + dataLength > reply.size())
        {
            return AnswerError::Malformed;
        }
        if (klass == Detail::CLASS_IN && type == static_cast<std::uint16_t>(asked))
        {
            if (asked == RecordType::A && dataLength == 4)
            {
                Ipv4 address{};
                std::copy_n(reply.begin() + static_cast<std::ptrdiff_t>(at), address.size(), address.begin());
                return address;
            }
            if (asked == RecordType::AAAA && dataLength == 16)
            {
                Ipv6 address{};
                std::copy_n(reply.begin() + static_cast<std::ptrdiff_t>(at), address.size(), address.begin());
                return address;
            }
            return AnswerError::Malformed; // the right type, the wrong size
        }
        at += dataLength; // a CNAME or another record: skipped
    }
    return AnswerError::NoAddress;
}

} // namespace Platform::DnsMessage
