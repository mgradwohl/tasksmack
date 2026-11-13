#pragma once

// A small JSON reader (#3093, moved here for #3094): reads one value at a time from a reply, strings unescaped
// and checked, and skips any nested value without recursion, at most MAX_NESTING deep. No allocation beyond the
// strings it returns. Shared by the update check's release reply and Look up on VirusTotal's file report.

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace Domain::Json
{

/// Deeper nesting than this is refused; the replies read here nest a few levels.
inline constexpr std::size_t MAX_NESTING = 64;

[[nodiscard]] constexpr bool isJsonDigit(char c) noexcept
{
    return c >= '0' && c <= '9';
}

class Reader
{
  public:
    explicit Reader(std::string_view text) noexcept : m_Text(text)
    {}

    void skipSpace() noexcept
    {
        while (m_Pos < m_Text.size() && (m_Text[m_Pos] == ' ' || m_Text[m_Pos] == '\t' || m_Text[m_Pos] == '\n' || m_Text[m_Pos] == '\r'))
        {
            ++m_Pos;
        }
    }

    [[nodiscard]] bool atEnd() const noexcept
    {
        return m_Pos >= m_Text.size();
    }

    /// The next character, or '\0' at the end (which no valid token starts with).
    [[nodiscard]] char peek() const noexcept
    {
        return atEnd() ? '\0' : m_Text[m_Pos];
    }

    [[nodiscard]] bool consume(char c) noexcept
    {
        if (atEnd() || m_Text[m_Pos] != c)
        {
            return false;
        }
        ++m_Pos;
        return true;
    }

    /// A string, at its opening quote, with its escapes decoded (a \u surrogate pair to one character, in
    /// UTF-8). nullopt for a raw control character, a bad escape, a lone surrogate or no closing quote.
    [[nodiscard]] std::optional<std::string> readString()
    {
        std::string out;
        if (!scanString(&out))
        {
            return std::nullopt;
        }
        return out;
    }

    /// true or false, at its first letter.
    [[nodiscard]] std::optional<bool> readBool() noexcept
    {
        if (consumeLiteral("true"))
        {
            return true;
        }
        if (consumeLiteral("false"))
        {
            return false;
        }
        return std::nullopt;
    }

    /// Steps over one value of any kind, checking it is well formed. Iterative: nested arrays and objects
    /// are tracked on a stack of their closing brackets, at most MAX_NESTING deep.
    [[nodiscard]] bool skipValue()
    {
        std::vector<char> closers;
        while (true)
        {
            // A value is expected here.
            skipSpace();
            const char c = peek();
            if (c == '{' || c == '[')
            {
                ++m_Pos;
                skipSpace();
                const char closer = (c == '{') ? '}' : ']';
                if (!consume(closer)) // not empty: its first member or element follows
                {
                    if (closers.size() >= MAX_NESTING)
                    {
                        return false;
                    }
                    closers.push_back(closer);
                    if (closer == '}' && !skipMemberName())
                    {
                        return false;
                    }
                    continue;
                }
            }
            else if (!skipScalar())
            {
                return false;
            }

            // After a value: a ',' and the next, or the closing bracket, until the outermost one closes.
            while (true)
            {
                if (closers.empty())
                {
                    return true;
                }
                skipSpace();
                if (consume(','))
                {
                    if (closers.back() == '}' && !skipMemberName())
                    {
                        return false;
                    }
                    break; // to the next value
                }
                if (!consume(closers.back()))
                {
                    return false;
                }
                closers.pop_back();
            }
        }
    }

    /// A non-negative integer (VirusTotal's engine counts, #3094), or nullopt for any other value, which is then
    /// skipped whole. Out of std::uint64_t's range is nullopt too.
    [[nodiscard]] std::optional<std::uint64_t> readUnsigned()
    {
        const std::size_t start = m_Pos;
        if (!isJsonDigit(peek()))
        {
            static_cast<void>(skipValue());
            return std::nullopt;
        }
        while (isJsonDigit(peek()))
        {
            ++m_Pos;
        }
        if (peek() == '.' || peek() == 'e' || peek() == 'E')
        {
            m_Pos = start;
            static_cast<void>(skipValue());
            return std::nullopt;
        }
        std::uint64_t value = 0;
        const auto [end, error] = std::from_chars(m_Text.data() + start, m_Text.data() + m_Pos, value);
        if (error != std::errc{} || end != m_Text.data() + m_Pos)
        {
            return std::nullopt;
        }
        return value;
    }

  private:
    [[nodiscard]] bool consumeLiteral(std::string_view literal) noexcept
    {
        if (m_Text.size() - m_Pos < literal.size() || std::string_view(m_Text.data() + m_Pos, literal.size()) != literal)
        {
            return false;
        }
        m_Pos += literal.size();
        return true;
    }

    /// An object member's name and its ':', skipped.
    [[nodiscard]] bool skipMemberName()
    {
        skipSpace();
        if (peek() != '"' || !scanString(nullptr))
        {
            return false;
        }
        skipSpace();
        return consume(':');
    }

    /// A string, number, true, false or null, skipped.
    [[nodiscard]] bool skipScalar()
    {
        const char c = peek();
        if (c == '"')
        {
            return scanString(nullptr);
        }
        if (c == 't' || c == 'f')
        {
            return readBool().has_value();
        }
        if (c == 'n')
        {
            return consumeLiteral("null");
        }
        return skipNumber();
    }

    /// -?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?
    [[nodiscard]] bool skipNumber() noexcept
    {
        static_cast<void>(consume('-'));
        if (!consume('0') && !skipDigits()) // a leading zero stands alone
        {
            return false;
        }
        if (consume('.') && !skipDigits())
        {
            return false;
        }
        if (consume('e') || consume('E'))
        {
            if (!consume('+'))
            {
                static_cast<void>(consume('-'));
            }
            return skipDigits();
        }
        return true;
    }

    /// One or more digits.
    [[nodiscard]] bool skipDigits() noexcept
    {
        const std::size_t start = m_Pos;
        while (isJsonDigit(peek()))
        {
            ++m_Pos;
        }
        return m_Pos > start;
    }

    /// Four hex digits after "\u".
    [[nodiscard]] std::optional<std::uint32_t> takeHex4() noexcept
    {
        if (m_Text.size() - m_Pos < 4)
        {
            return std::nullopt;
        }
        std::uint32_t code = 0;
        const char* const first = m_Text.data() + m_Pos;
        const auto [end, error] = std::from_chars(first, first + 4, code, 16);
        if (error != std::errc{} || end != first + 4)
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

    /// The code point of a \u escape whose "\u" has been read, joining a surrogate pair.
    [[nodiscard]] std::optional<std::uint32_t> takeEscapedCodePoint() noexcept
    {
        const std::optional<std::uint32_t> first = takeHex4();
        if (!first.has_value())
        {
            return std::nullopt;
        }
        if (*first >= 0xDC00U && *first <= 0xDFFFU)
        {
            return std::nullopt; // a low surrogate with no high one before it
        }
        if (*first < 0xD800U || *first > 0xDBFFU)
        {
            return first;
        }
        if (!consume('\\') || !consume('u'))
        {
            return std::nullopt; // a high surrogate with no low one after it
        }
        const std::optional<std::uint32_t> second = takeHex4();
        if (!second.has_value() || *second < 0xDC00U || *second > 0xDFFFU)
        {
            return std::nullopt;
        }
        return 0x10000U + ((*first - 0xD800U) << 10U) + (*second - 0xDC00U);
    }

    /// A string at its opening quote, decoded into @p out when it isn't null (skipped otherwise).
    [[nodiscard]] bool scanString(std::string* out)
    {
        if (!consume('"'))
        {
            return false;
        }
        while (!atEnd())
        {
            const char c = m_Text[m_Pos++];
            if (c == '"')
            {
                return true;
            }
            if (static_cast<unsigned char>(c) < 0x20U)
            {
                return false; // a raw control character isn't valid JSON
            }
            if (c != '\\')
            {
                if (out != nullptr)
                {
                    *out += c;
                }
                continue;
            }
            if (atEnd())
            {
                return false;
            }
            const char escape = m_Text[m_Pos++];
            char decoded = '\0';
            switch (escape)
            {
            case '"':
            case '\\':
            case '/':
                decoded = escape;
                break;
            case 'b':
                decoded = '\b';
                break;
            case 'f':
                decoded = '\f';
                break;
            case 'n':
                decoded = '\n';
                break;
            case 'r':
                decoded = '\r';
                break;
            case 't':
                decoded = '\t';
                break;
            case 'u':
            {
                const std::optional<std::uint32_t> code = takeEscapedCodePoint();
                if (!code.has_value())
                {
                    return false;
                }
                if (out != nullptr)
                {
                    appendUtf8(*out, *code);
                }
                continue;
            }
            default:
                return false;
            }
            if (out != nullptr)
            {
                *out += decoded;
            }
        }
        return false; // no closing quote
    }

    std::string_view m_Text;
    std::size_t m_Pos = 0;
};

} // namespace Domain::Json
