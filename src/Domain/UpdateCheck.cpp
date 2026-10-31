#include "Domain/UpdateCheck.h"

#include "Domain/SamplingConfig.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

namespace Domain::UpdateCheck
{

namespace
{

// ---------------------------------------------------------------------------------------------------
// Versions
// ---------------------------------------------------------------------------------------------------

[[nodiscard]] constexpr bool isDigit(char c) noexcept
{
    return c >= '0' && c <= '9';
}

[[nodiscard]] constexpr bool isIdentifierChar(char c) noexcept
{
    return isDigit(c) || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '-';
}

[[nodiscard]] bool allDigits(std::string_view text) noexcept
{
    return !text.empty() && std::ranges::all_of(text, isDigit);
}

/// The number at the start of @p text, consumed: digits without a leading zero, fitting 32 bits.
[[nodiscard]] std::optional<std::uint32_t> takeNumber(std::string_view& text) noexcept
{
    // A position, not the iterator: string_view's is a pointer in libstdc++ and a class in MSVC's STL.
    const auto length = static_cast<std::size_t>(std::ranges::find_if_not(text, isDigit) - text.begin());
    if (length == 0 || (length > 1 && text.front() == '0'))
    {
        return std::nullopt;
    }
    std::uint32_t value = 0;
    const char* const first = text.data();
    const auto [end, error] = std::from_chars(first, first + length, value);
    if (error != std::errc{} || end != first + length)
    {
        return std::nullopt; // more than 32 bits
    }
    text.remove_prefix(length);
    return value;
}

[[nodiscard]] bool takeChar(std::string_view& text, char c) noexcept
{
    if (text.empty() || text.front() != c)
    {
        return false;
    }
    text.remove_prefix(1);
    return true;
}

/// The dot-separated identifiers of a pre-release (@p prerelease) or a build, each non-empty and made of
/// [0-9A-Za-z-]; a pre-release's numeric ones without a leading zero.
[[nodiscard]] std::optional<std::vector<std::string>> splitIdentifiers(std::string_view text, bool prerelease)
{
    std::vector<std::string> identifiers;
    while (true)
    {
        const std::size_t dot = text.find('.');
        const std::string_view identifier = text.substr(0, dot);
        if (identifier.empty() || !std::ranges::all_of(identifier, isIdentifierChar))
        {
            return std::nullopt;
        }
        if (prerelease && allDigits(identifier) && identifier.size() > 1 && identifier.front() == '0')
        {
            return std::nullopt;
        }
        identifiers.emplace_back(identifier);
        if (dot == std::string_view::npos)
        {
            return identifiers;
        }
        text.remove_prefix(dot + 1);
    }
}

/// One pre-release identifier against another, as SemVer orders them.
[[nodiscard]] std::strong_ordering compareIdentifier(const std::string& left, const std::string& right)
{
    const bool leftNumeric = allDigits(left);
    const bool rightNumeric = allDigits(right);
    if (leftNumeric && rightNumeric)
    {
        // No leading zeros, so the longer number is the larger; of two as long, the text orders them.
        if (left.size() != right.size())
        {
            return left.size() <=> right.size();
        }
        return left.compare(right) <=> 0;
    }
    if (leftNumeric != rightNumeric)
    {
        return leftNumeric ? std::strong_ordering::less : std::strong_ordering::greater;
    }
    return left.compare(right) <=> 0;
}

// ---------------------------------------------------------------------------------------------------
// The release JSON: a reader of the top-level object that skips nested values without recursion
// ---------------------------------------------------------------------------------------------------

/// Deeper nesting than this is refused; GitHub's release JSON nests three levels.
constexpr std::size_t MAX_NESTING = 64;

class JsonReader
{
  public:
    explicit JsonReader(std::string_view text) noexcept : m_Text(text)
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
        while (isDigit(peek()))
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

} // namespace

std::string userAgent(std::string_view currentVersion)
{
    return std::format("TaskSmack/{}", currentVersion);
}

std::string Version::text() const
{
    std::string out = std::format("{}.{}.{}", major, minor, patch);
    for (std::size_t i = 0; i < prerelease.size(); ++i)
    {
        out += (i == 0) ? '-' : '.';
        out += prerelease[i];
    }
    return out;
}

std::optional<Version> parseVersion(std::string_view text)
{
    if (!text.empty() && (text.front() == 'v' || text.front() == 'V'))
    {
        text.remove_prefix(1);
    }
    Version version;
    const std::optional<std::uint32_t> major = takeNumber(text);
    if (!major.has_value() || !takeChar(text, '.'))
    {
        return std::nullopt;
    }
    const std::optional<std::uint32_t> minor = takeNumber(text);
    if (!minor.has_value() || !takeChar(text, '.'))
    {
        return std::nullopt;
    }
    const std::optional<std::uint32_t> patch = takeNumber(text);
    if (!patch.has_value())
    {
        return std::nullopt;
    }
    version.major = *major;
    version.minor = *minor;
    version.patch = *patch;

    const std::size_t plus = text.find('+');
    std::string_view prerelease = text.substr(0, plus);
    if (!prerelease.empty())
    {
        if (!takeChar(prerelease, '-'))
        {
            return std::nullopt; // "1.2.3x", "1.2.3.4"
        }
        std::optional<std::vector<std::string>> identifiers = splitIdentifiers(prerelease, true);
        if (!identifiers.has_value())
        {
            return std::nullopt;
        }
        version.prerelease = std::move(*identifiers);
    }
    if (plus != std::string_view::npos)
    {
        const std::string_view build = text.substr(plus + 1);
        if (!splitIdentifiers(build, false).has_value())
        {
            return std::nullopt;
        }
        version.build = std::string(build);
    }
    return version;
}

std::strong_ordering compare(const Version& left, const Version& right)
{
    if (const auto order = std::tie(left.major, left.minor, left.patch) <=> std::tie(right.major, right.minor, right.patch); order != 0)
    {
        return order;
    }
    // A release ranks above its pre-releases.
    if (left.prerelease.empty() || right.prerelease.empty())
    {
        if (left.prerelease.empty() == right.prerelease.empty())
        {
            return std::strong_ordering::equal;
        }
        return left.prerelease.empty() ? std::strong_ordering::greater : std::strong_ordering::less;
    }
    const std::size_t shared = std::min(left.prerelease.size(), right.prerelease.size());
    for (std::size_t i = 0; i < shared; ++i)
    {
        if (const std::strong_ordering order = compareIdentifier(left.prerelease[i], right.prerelease[i]); order != 0)
        {
            return order;
        }
    }
    return left.prerelease.size() <=> right.prerelease.size();
}

std::optional<ReleaseFields> readReleaseFields(std::string_view json)
{
    if (json.size() > MAX_RESPONSE_BYTES)
    {
        return std::nullopt;
    }
    JsonReader reader(json);
    reader.skipSpace();
    if (!reader.consume('{'))
    {
        return std::nullopt;
    }
    ReleaseFields fields;
    reader.skipSpace();
    if (!reader.consume('}'))
    {
        while (true)
        {
            reader.skipSpace();
            if (reader.peek() != '"')
            {
                return std::nullopt;
            }
            const std::optional<std::string> name = reader.readString();
            reader.skipSpace();
            if (!name.has_value() || !reader.consume(':'))
            {
                return std::nullopt;
            }
            reader.skipSpace();

            std::optional<std::string>* text = nullptr;
            std::optional<bool>* flag = nullptr;
            if (*name == "tag_name")
            {
                text = &fields.tagName;
            }
            else if (*name == "html_url")
            {
                text = &fields.htmlUrl;
            }
            else if (*name == "draft")
            {
                flag = &fields.draft;
            }
            else if (*name == "prerelease")
            {
                flag = &fields.prerelease;
            }

            if (text != nullptr && reader.peek() == '"')
            {
                *text = reader.readString();
                if (!text->has_value())
                {
                    return std::nullopt;
                }
            }
            else if (flag != nullptr && (reader.peek() == 't' || reader.peek() == 'f'))
            {
                *flag = reader.readBool();
                if (!flag->has_value())
                {
                    return std::nullopt;
                }
            }
            else
            {
                // Another member, or one of these with another type: skipped, and the field left unset.
                if (text != nullptr)
                {
                    text->reset();
                }
                if (flag != nullptr)
                {
                    flag->reset();
                }
                if (!reader.skipValue())
                {
                    return std::nullopt;
                }
            }

            reader.skipSpace();
            if (reader.consume(','))
            {
                continue;
            }
            if (reader.consume('}'))
            {
                break;
            }
            return std::nullopt;
        }
    }
    reader.skipSpace();
    if (!reader.atEnd())
    {
        return std::nullopt;
    }
    return fields;
}

bool isReleasePageUrl(std::string_view url)
{
    if (url.size() > MAX_URL_BYTES || !url.starts_with(RELEASES_PAGE_PREFIX) || url.size() == RELEASES_PAGE_PREFIX.size())
    {
        return false;
    }
    const std::string_view rest = url.substr(RELEASES_PAGE_PREFIX.size());
    if (rest.contains(".."))
    {
        return false; // no way out of the releases pages
    }
    return std::ranges::all_of(
        rest, [](char c) { return isIdentifierChar(c) || c == '.' || c == '_' || c == '~' || c == '/' || c == '%' || c == '+'; });
}

Outcome failed(std::string reason)
{
    return Outcome{.latestVersion = {}, .releaseUrl = {}, .detail = std::move(reason), .kind = Outcome::Kind::Failed};
}

Outcome evaluateResponse(int status, std::string_view body, const Version& current)
{
    if (status != 200)
    {
        // 403 or 429: GitHub's rate limit for requests without a token; 404: no release yet.
        return failed(std::format("GitHub answered HTTP {}", status));
    }
    const std::optional<ReleaseFields> fields = readReleaseFields(body);
    if (!fields.has_value())
    {
        return failed("The reply isn't a release's JSON");
    }
    if (!fields->tagName.has_value())
    {
        return failed("The reply has no tag_name");
    }
    if (fields->draft.value_or(false) || fields->prerelease.value_or(false))
    {
        return failed(std::format("The latest release, {}, is a draft or a pre-release", *fields->tagName));
    }
    const std::optional<Version> latest = parseVersion(*fields->tagName);
    if (!latest.has_value() || !latest->prerelease.empty())
    {
        return failed(std::format("The latest release's tag, {}, isn't a release version", *fields->tagName));
    }
    std::string url(LATEST_RELEASE_PAGE);
    if (fields->htmlUrl.has_value() && isReleasePageUrl(*fields->htmlUrl))
    {
        url = *fields->htmlUrl;
    }
    const Outcome::Kind kind = (compare(*latest, current) > 0) ? Outcome::Kind::Available : Outcome::Kind::UpToDate;
    return Outcome{.latestVersion = latest->text(), .releaseUrl = std::move(url), .detail = {}, .kind = kind};
}

std::optional<std::chrono::system_clock::time_point> fromUnixSeconds(std::int64_t unixSeconds)
{
    // 9999-12-31T23:59:59Z: later than any check, and far inside every clock's range.
    constexpr std::int64_t LATEST_UNIX_SECONDS = 253'402'300'799;
    if (unixSeconds <= 0 || unixSeconds > LATEST_UNIX_SECONDS)
    {
        return std::nullopt;
    }
    return std::chrono::system_clock::time_point(
        std::chrono::duration_cast<std::chrono::system_clock::duration>(std::chrono::seconds(unixSeconds)));
}

std::int64_t toUnixSeconds(std::chrono::system_clock::time_point time)
{
    return std::chrono::duration_cast<std::chrono::seconds>(time.time_since_epoch()).count();
}

bool isCheckDue(bool enabled, std::optional<std::chrono::system_clock::time_point> lastCheck, std::chrono::system_clock::time_point now)
{
    if (!enabled)
    {
        return false;
    }
    if (!lastCheck.has_value() || now < *lastCheck)
    {
        return true;
    }
    return now - *lastCheck >= std::chrono::hours(Sampling::UPDATE_CHECK_INTERVAL_HOURS);
}

std::optional<Notice> noticeFor(const Persisted& state, const Version& current)
{
    if (!state.enabled)
    {
        return std::nullopt;
    }
    const std::optional<Version> latest = parseVersion(state.latestVersion);
    if (!latest.has_value() || !latest->prerelease.empty() || compare(*latest, current) <= 0)
    {
        return std::nullopt;
    }
    if (const std::optional<Version> dismissed = parseVersion(state.dismissedVersion);
        dismissed.has_value() && compare(*latest, *dismissed) <= 0)
    {
        return std::nullopt; // dismissed: until a newer release than that one
    }
    std::string url(LATEST_RELEASE_PAGE);
    if (isReleasePageUrl(state.latestUrl))
    {
        url = state.latestUrl;
    }
    return Notice{.version = latest->text(), .url = std::move(url)};
}

std::string noticeText(std::string_view version)
{
    return std::format("TaskSmack v{} is available", version);
}

} // namespace Domain::UpdateCheck
