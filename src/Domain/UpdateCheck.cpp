#include "Domain/UpdateCheck.h"

#include "Domain/JsonReader.h"
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

using JsonReader = Domain::Json::Reader;

/// Reads one `"name": value` member of the release object at @p reader into @p fields (#3093): the four
/// names readReleaseFields() keeps are stored when they have the expected type, and any other member,
/// or one of the four with another type, is skipped and leaves its field unset. A repeated name keeps
/// its last value. False when the member isn't well formed, which fails the whole reply.
[[nodiscard]] bool readReleaseMember(JsonReader& reader, ReleaseFields& fields)
{
    reader.skipSpace();
    if (reader.peek() != '"')
    {
        return false; // a member starts with its quoted name
    }
    const std::optional<std::string> name = reader.readString();
    reader.skipSpace();
    if (!name.has_value() || !reader.consume(':'))
    {
        return false;
    }
    reader.skipSpace();

    // Which field, if any, this name fills: a string one or a boolean one.
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
        return text->has_value();
    }
    if (flag != nullptr && (reader.peek() == 't' || reader.peek() == 'f'))
    {
        *flag = reader.readBool();
        return flag->has_value();
    }
    // Another member, or one of these with another type: skipped, and the field left unset.
    if (text != nullptr)
    {
        text->reset();
    }
    if (flag != nullptr)
    {
        flag->reset();
    }
    return reader.skipValue();
}

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
        return std::nullopt; // not an object: GitHub's error pages and rate-limit replies are objects too
    }
    ReleaseFields fields;
    reader.skipSpace();
    if (!reader.consume('}')) // an empty object reads as no fields
    {
        // One member at a time, each followed by ',' or the object's closing '}'.
        while (true)
        {
            if (!readReleaseMember(reader, fields))
            {
                return std::nullopt;
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
            return std::nullopt; // neither separator: malformed
        }
    }
    // Nothing but whitespace may follow the object, so a truncated or concatenated reply is refused.
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
    // An escape is two hex digits, and never a dot or a slash: a browser resolves ".%2e" as ".." (#3290), and
    // %2f or %5c as a path separator, so either could leave the releases pages the check above keeps it in.
    const auto hex = [](char c)
    {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    };
    std::size_t percent = rest.find('%');
    while (percent != std::string_view::npos)
    {
        if (percent + 2 >= rest.size() || !hex(rest[percent + 1]) || !hex(rest[percent + 2]))
        {
            return false;
        }
        const char high = rest[percent + 1];
        const char low = static_cast<char>(rest[percent + 2] | 0x20); // lower case
        if ((high == '2' && (low == 'e' || low == 'f')) || (high == '5' && low == 'c'))
        {
            return false;
        }
        percent = rest.find('%', percent + 3);
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
