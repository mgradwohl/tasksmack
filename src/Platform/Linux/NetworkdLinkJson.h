#pragma once

// systemd-networkd's per-link description (#1518): org.freedesktop.network1.Link.Describe() returns
// the link as JSON (systemd's src/network/networkd-json.c), and its DHCPv4 lease is read from that.
// A small JSON parser, bounded in depth and size since the text comes from another process, and the
// lease extraction, both pure so they're unit-tested (and fuzzed) without a bus.
//
// The fields used, as networkd writes them (systemd 252 and later):
//   "Addresses": [ { "Family": 2, "Address": [192,168,1,20], "PrefixLength": 24,
//                    "ConfigSource": "DHCPv4", "ConfigProvider": [192,168,1,1],
//                    "ValidLifetimeUSec": <CLOCK_BOOTTIME usec>, ... }, ... ]
//   "DHCPv4Client": { "Lease": { "LeaseTimestampUSec": <CLOCK_BOOTTIME usec>, "Timeout1USec": ...,
//                                "Timeout2USec": ..., "Hostname": "..." } }
// Addresses are byte arrays; times are absolute on CLOCK_BOOTTIME, so they are converted with the
// current boot time and wall clock.

#include "Platform/ISystemInfoProbe.h"
#include "Platform/Linux/LinuxNetworkAdapters.h"

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace Platform::NetworkdLinkJson
{

/// A parsed JSON value. Numbers keep their integer value when they are one (networkd's microsecond
/// times exceed a double's exact range only after ~285 years of uptime, but integers are exact anyway).
struct Value
{
    enum class Kind : std::uint8_t
    {
        Null,
        Bool,
        Number,
        String,
        Array,
        Object,
    };
    Kind kind = Kind::Null;
    bool boolean = false;
    std::optional<std::uint64_t> unsignedInteger; ///< For a non-negative integer that fits.
    bool negative = false;                        ///< A number with a leading '-'.
    std::string string;
    std::vector<Value> items;       ///< Array elements, or an object's member values.
    std::vector<std::string> names; ///< An object's member names, one per item, in order.

    /// The object member named @p key, or nullptr (also when this isn't an object).
    [[nodiscard]] const Value* find(std::string_view key) const noexcept
    {
        if (kind != Kind::Object)
        {
            return nullptr;
        }
        for (std::size_t i = 0; i < names.size() && i < items.size(); ++i)
        {
            if (names[i] == key)
            {
                return &items[i];
            }
        }
        return nullptr;
    }
};

/// Deepest nesting accepted; networkd's description nests about four levels.
inline constexpr int MAX_DEPTH = 32;
/// Largest text accepted: a description is a few KB, even with many addresses and routes.
inline constexpr std::size_t MAX_TEXT_BYTES = std::size_t{4} << 20U;

namespace Detail
{

class Parser
{
  public:
    explicit Parser(std::string_view text) : m_Text(text)
    {}

    [[nodiscard]] std::optional<Value> parseDocument()
    {
        std::optional<Value> value = parseValue(0);
        skipSpace();
        if (!value || m_Pos != m_Text.size())
        {
            return std::nullopt;
        }
        return value;
    }

  private:
    void skipSpace() noexcept
    {
        while (m_Pos < m_Text.size() && (m_Text[m_Pos] == ' ' || m_Text[m_Pos] == '\t' || m_Text[m_Pos] == '\n' || m_Text[m_Pos] == '\r'))
        {
            ++m_Pos;
        }
    }

    [[nodiscard]] bool consume(std::string_view literal) noexcept
    {
        if (m_Text.size() - m_Pos < literal.size() || std::string_view(m_Text.data() + m_Pos, literal.size()) != literal)
        {
            return false;
        }
        m_Pos += literal.size();
        return true;
    }

    // parseValue(), parseObject() and parseArray() recurse into each other for nested values; the depth
    // is bounded by MAX_DEPTH, so the text can't exhaust the stack (and the parser is fuzzed).
    // NOLINTBEGIN(misc-no-recursion)
    [[nodiscard]] std::optional<Value> parseValue(int depth)
    {
        if (depth > MAX_DEPTH)
        {
            return std::nullopt;
        }
        skipSpace();
        if (m_Pos >= m_Text.size())
        {
            return std::nullopt;
        }
        Value value;
        const char c = m_Text[m_Pos];
        if (c == '{')
        {
            return parseObject(depth);
        }
        if (c == '[')
        {
            return parseArray(depth);
        }
        if (c == '"')
        {
            std::optional<std::string> text = parseString();
            if (!text)
            {
                return std::nullopt;
            }
            value.kind = Value::Kind::String;
            value.string = std::move(*text);
            return value;
        }
        if (consume("true"))
        {
            value.kind = Value::Kind::Bool;
            value.boolean = true;
            return value;
        }
        if (consume("false"))
        {
            value.kind = Value::Kind::Bool;
            return value;
        }
        if (consume("null"))
        {
            return value;
        }
        return parseNumber();
    }

    [[nodiscard]] std::optional<Value> parseObject(int depth)
    {
        Value value;
        value.kind = Value::Kind::Object;
        ++m_Pos; // '{'
        skipSpace();
        if (consume("}"))
        {
            return value;
        }
        while (true)
        {
            skipSpace();
            if (m_Pos >= m_Text.size() || m_Text[m_Pos] != '"')
            {
                return std::nullopt;
            }
            std::optional<std::string> key = parseString();
            skipSpace();
            if (!key || !consume(":"))
            {
                return std::nullopt;
            }
            std::optional<Value> member = parseValue(depth + 1);
            if (!member)
            {
                return std::nullopt;
            }
            value.names.push_back(std::move(*key));
            value.items.push_back(std::move(*member));
            skipSpace();
            if (consume("}"))
            {
                return value;
            }
            if (!consume(","))
            {
                return std::nullopt;
            }
        }
    }

    [[nodiscard]] std::optional<Value> parseArray(int depth)
    {
        Value value;
        value.kind = Value::Kind::Array;
        ++m_Pos; // '['
        skipSpace();
        if (consume("]"))
        {
            return value;
        }
        while (true)
        {
            std::optional<Value> item = parseValue(depth + 1);
            if (!item)
            {
                return std::nullopt;
            }
            value.items.push_back(std::move(*item));
            skipSpace();
            if (consume("]"))
            {
                return value;
            }
            if (!consume(","))
            {
                return std::nullopt;
            }
        }
    }

    // NOLINTEND(misc-no-recursion)

    [[nodiscard]] std::optional<std::uint32_t> parseHex4() noexcept
    {
        if (m_Pos + 4 > m_Text.size())
        {
            return std::nullopt;
        }
        std::uint32_t code = 0;
        const auto [ptr, ec] = std::from_chars(m_Text.data() + m_Pos, m_Text.data() + m_Pos + 4, code, 16);
        if (ec != std::errc{} || ptr != m_Text.data() + m_Pos + 4)
        {
            return std::nullopt;
        }
        m_Pos += 4;
        return code;
    }

    static void appendUtf8(std::string& out, std::uint32_t code)
    {
        if (code < 0x80U)
        {
            out += static_cast<char>(code);
        }
        else if (code < 0x800U)
        {
            out += static_cast<char>(0xC0U | (code >> 6U));
            out += static_cast<char>(0x80U | (code & 0x3FU));
        }
        else if (code < 0x10000U)
        {
            out += static_cast<char>(0xE0U | (code >> 12U));
            out += static_cast<char>(0x80U | ((code >> 6U) & 0x3FU));
            out += static_cast<char>(0x80U | (code & 0x3FU));
        }
        else
        {
            out += static_cast<char>(0xF0U | (code >> 18U));
            out += static_cast<char>(0x80U | ((code >> 12U) & 0x3FU));
            out += static_cast<char>(0x80U | ((code >> 6U) & 0x3FU));
            out += static_cast<char>(0x80U | (code & 0x3FU));
        }
    }

    [[nodiscard]] std::optional<std::string> parseString()
    {
        ++m_Pos; // opening quote
        std::string out;
        while (m_Pos < m_Text.size())
        {
            const char c = m_Text[m_Pos++];
            if (c == '"')
            {
                return out;
            }
            if (static_cast<unsigned char>(c) < 0x20U)
            {
                return std::nullopt; // a raw control character isn't valid JSON
            }
            if (c != '\\')
            {
                out += c;
                continue;
            }
            if (m_Pos >= m_Text.size())
            {
                return std::nullopt;
            }
            const char escape = m_Text[m_Pos++];
            switch (escape)
            {
            case '"':
            case '\\':
            case '/':
                out += escape;
                break;
            case 'b':
                out += '\b';
                break;
            case 'f':
                out += '\f';
                break;
            case 'n':
                out += '\n';
                break;
            case 'r':
                out += '\r';
                break;
            case 't':
                out += '\t';
                break;
            case 'u':
            {
                std::optional<std::uint32_t> code = parseHex4();
                if (!code)
                {
                    return std::nullopt;
                }
                if (*code >= 0xD800U && *code < 0xDC00U && consume("\\u"))
                {
                    // A surrogate pair: the low half follows.
                    const std::optional<std::uint32_t> low = parseHex4();
                    if (!low || *low < 0xDC00U || *low >= 0xE000U)
                    {
                        return std::nullopt;
                    }
                    code = 0x10000U + ((*code - 0xD800U) << 10U) + (*low - 0xDC00U);
                }
                else if (*code >= 0xD800U && *code < 0xE000U)
                {
                    return std::nullopt; // an unpaired surrogate
                }
                appendUtf8(out, *code);
                break;
            }
            default:
                return std::nullopt;
            }
        }
        return std::nullopt; // unterminated
    }

    [[nodiscard]] std::optional<Value> parseNumber()
    {
        Value value;
        value.kind = Value::Kind::Number;
        if (m_Pos < m_Text.size() && m_Text[m_Pos] == '-')
        {
            value.negative = true;
            ++m_Pos;
        }
        const std::size_t digitsStart = m_Pos;
        while (m_Pos < m_Text.size() && m_Text[m_Pos] >= '0' && m_Text[m_Pos] <= '9')
        {
            ++m_Pos;
        }
        if (m_Pos == digitsStart)
        {
            return std::nullopt;
        }
        bool integral = true;
        if (m_Pos < m_Text.size() && m_Text[m_Pos] == '.')
        {
            integral = false;
            ++m_Pos;
            const std::size_t fractionStart = m_Pos;
            while (m_Pos < m_Text.size() && m_Text[m_Pos] >= '0' && m_Text[m_Pos] <= '9')
            {
                ++m_Pos;
            }
            if (m_Pos == fractionStart)
            {
                return std::nullopt;
            }
        }
        if (m_Pos < m_Text.size() && (m_Text[m_Pos] == 'e' || m_Text[m_Pos] == 'E'))
        {
            integral = false;
            ++m_Pos;
            if (m_Pos < m_Text.size() && (m_Text[m_Pos] == '+' || m_Text[m_Pos] == '-'))
            {
                ++m_Pos;
            }
            const std::size_t exponentStart = m_Pos;
            while (m_Pos < m_Text.size() && m_Text[m_Pos] >= '0' && m_Text[m_Pos] <= '9')
            {
                ++m_Pos;
            }
            if (m_Pos == exponentStart)
            {
                return std::nullopt;
            }
        }
        if (integral && !value.negative)
        {
            std::uint64_t number = 0;
            const auto [ptr, ec] = std::from_chars(m_Text.data() + digitsStart, m_Text.data() + m_Pos, number, 10);
            if (ec == std::errc{} && ptr == m_Text.data() + m_Pos)
            {
                value.unsignedInteger = number;
            }
        }
        return value;
    }

    std::string_view m_Text;
    std::size_t m_Pos = 0;
};

} // namespace Detail

/// @p text parsed as one JSON document; nullopt when it isn't one, nests deeper than MAX_DEPTH, or is
/// longer than MAX_TEXT_BYTES.
[[nodiscard]] inline std::optional<Value> parse(std::string_view text)
{
    if (text.size() > MAX_TEXT_BYTES)
    {
        return std::nullopt;
    }
    return Detail::Parser(text).parseDocument();
}

/// What networkd's description says about the link's DHCPv4 lease, its times still on CLOCK_BOOTTIME.
struct Lease
{
    std::string server;                            ///< The DHCPv4 address's ConfigProvider; empty when not given
    std::optional<std::uint64_t> obtainedBootUsec; ///< DHCPv4Client.Lease.LeaseTimestampUSec
    std::optional<std::uint64_t> expiresBootUsec;  ///< The DHCPv4 address's ValidLifetimeUSec
};

/// The IPv4 address a JSON byte array holds ([192,168,1,1]); empty when it isn't four bytes.
[[nodiscard]] inline std::string ipv4FromBytes(const Value& bytes)
{
    if (bytes.kind != Value::Kind::Array || bytes.items.size() != 4)
    {
        return {};
    }
    std::array<std::uint8_t, 4> octets{};
    for (std::size_t i = 0; i < octets.size(); ++i)
    {
        const Value& item = bytes.items[i];
        if (item.kind != Value::Kind::Number || !item.unsignedInteger || *item.unsignedInteger > 255U)
        {
            return {};
        }
        octets[i] = static_cast<std::uint8_t>(*item.unsignedInteger);
    }
    return LinuxNetworkAdapters::formatAddressBytes(2, octets);
}

/// The DHCPv4 lease in a Link.Describe() document: the server and expiry from the first IPv4 address
/// networkd got by DHCPv4, the time it was obtained from DHCPv4Client.Lease. nullopt when the document
/// doesn't parse, or has neither.
[[nodiscard]] inline std::optional<Lease> leaseFromDescription(std::string_view json)
{
    const std::optional<Value> root = parse(json);
    if (!root || root->kind != Value::Kind::Object)
    {
        return std::nullopt;
    }
    Lease lease;
    bool found = false;
    if (const Value* client = root->find("DHCPv4Client"); client != nullptr)
    {
        if (const Value* dhcpLease = client->find("Lease"); dhcpLease != nullptr)
        {
            if (const Value* obtained = dhcpLease->find("LeaseTimestampUSec"); obtained != nullptr && obtained->unsignedInteger)
            {
                lease.obtainedBootUsec = obtained->unsignedInteger;
            }
            found = true;
        }
    }
    if (const Value* addresses = root->find("Addresses"); addresses != nullptr && addresses->kind == Value::Kind::Array)
    {
        for (const Value& address : addresses->items)
        {
            const Value* family = address.find("Family");
            const Value* source = address.find("ConfigSource");
            if (family == nullptr || !family->unsignedInteger || *family->unsignedInteger != 2U || source == nullptr ||
                source->kind != Value::Kind::String || source->string != "DHCPv4")
            {
                continue;
            }
            if (const Value* provider = address.find("ConfigProvider"); provider != nullptr)
            {
                lease.server = ipv4FromBytes(*provider);
            }
            if (const Value* valid = address.find("ValidLifetimeUSec"); valid != nullptr && valid->unsignedInteger)
            {
                lease.expiresBootUsec = valid->unsignedInteger;
            }
            found = true;
            break;
        }
    }
    if (!found)
    {
        return std::nullopt;
    }
    return lease;
}

/// @p lease as the page shows it: the expiry moved from CLOCK_BOOTTIME to Unix time with the current
/// readings of both clocks, and the length from when it was obtained to when it runs out. nullopt when
/// nothing is left to show.
[[nodiscard]] inline std::optional<DhcpLease> toDhcpLease(const Lease& lease, std::uint64_t nowUnixSeconds, std::uint64_t nowBootUsec)
{
    constexpr std::uint64_t USEC_PER_SECOND = 1'000'000;
    DhcpLease result;
    result.server = lease.server;
    if (lease.expiresBootUsec)
    {
        const std::uint64_t expires = *lease.expiresBootUsec;
        if (expires >= nowBootUsec)
        {
            result.expiresUnix = nowUnixSeconds + ((expires - nowBootUsec) / USEC_PER_SECOND);
        }
        else
        {
            const std::uint64_t agoSeconds = (nowBootUsec - expires) / USEC_PER_SECOND;
            result.expiresUnix = agoSeconds < nowUnixSeconds ? nowUnixSeconds - agoSeconds : 0;
        }
        if (lease.obtainedBootUsec && *lease.obtainedBootUsec <= expires)
        {
            const std::uint64_t seconds = (expires - *lease.obtainedBootUsec) / USEC_PER_SECOND;
            result.leaseSeconds = seconds > UINT32_MAX ? UINT32_MAX : static_cast<std::uint32_t>(seconds);
        }
    }
    if (result.server.empty() && result.leaseSeconds == 0 && result.expiresUnix == 0)
    {
        return std::nullopt;
    }
    return result;
}

} // namespace Platform::NetworkdLinkJson
