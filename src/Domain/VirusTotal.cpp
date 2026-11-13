#include "Domain/VirusTotal.h"

#include "Domain/JsonReader.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>

namespace Domain::VirusTotal
{

namespace
{

[[nodiscard]] constexpr bool isLowerHex(char c) noexcept
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

[[nodiscard]] constexpr bool isHex(char c) noexcept
{
    return isLowerHex(c) || (c >= 'A' && c <= 'F');
}

/// What a member callback did with the member's value.
enum class Member : std::uint8_t
{
    Skip,      ///< Not wanted: readObject() skips the value
    Read,      ///< Read whole
    Malformed, ///< Couldn't be read: the reply fails
};

/// Reads the object at @p reader, calling @p onMember(name) for each member with the reader at its value.
/// False when the object, or a member the callback read, isn't well formed.
template<typename OnMember> [[nodiscard]] bool readObject(Json::Reader& reader, const OnMember& onMember)
{
    reader.skipSpace();
    if (!reader.consume('{'))
    {
        return false;
    }
    reader.skipSpace();
    if (reader.consume('}'))
    {
        return true;
    }
    for (;;)
    {
        reader.skipSpace();
        const std::optional<std::string> name = reader.peek() == '"' ? reader.readString() : std::nullopt;
        reader.skipSpace();
        if (!name.has_value() || !reader.consume(':'))
        {
            return false;
        }
        reader.skipSpace();
        const Member member = onMember(*name);
        if (member == Member::Malformed || (member == Member::Skip && !reader.skipValue()))
        {
            return false;
        }
        reader.skipSpace();
        if (reader.consume('}'))
        {
            return true;
        }
        if (!reader.consume(','))
        {
            return false;
        }
    }
}

} // namespace

bool isSha256Hex(std::string_view text) noexcept
{
    return text.size() == 64 && std::ranges::all_of(text, isLowerHex);
}

bool isApiKey(std::string_view key) noexcept
{
    return key.size() == API_KEY_LENGTH && std::ranges::all_of(key, isHex);
}

std::string filePath(std::string_view sha256)
{
    return std::string(FILES_PATH_PREFIX) + std::string(sha256);
}

std::string reportPage(std::string_view sha256)
{
    return std::string(REPORT_PAGE_PREFIX) + std::string(sha256);
}

std::optional<Verdict> readFileReport(std::string_view json)
{
    if (json.size() > MAX_RESPONSE_BYTES)
    {
        return std::nullopt;
    }
    Json::Reader reader(json);
    Verdict verdict;
    unsigned seen = 0; // one bit per count found: all four must be there

    // last_analysis_stats: the four counts, any other member skipped.
    const auto readStats = [&reader, &verdict, &seen](const std::string& stat)
    {
        constexpr std::array<std::string_view, 4> NAMES{"malicious", "suspicious", "undetected", "harmless"};
        const std::array<std::uint64_t*, 4> counts{&verdict.malicious, &verdict.suspicious, &verdict.undetected, &verdict.harmless};
        // An index, not an iterator: std::array's iterator is a pointer in libc++ but a class in MSVC's library.
        std::size_t index = 0;
        while (index < NAMES.size() && NAMES[index] != stat)
        {
            ++index;
        }
        if (index == NAMES.size())
        {
            return Member::Skip;
        }
        if (const std::optional<std::uint64_t> value = reader.readUnsigned())
        {
            *counts[index] = *value;
            seen |= 1U << index;
        }
        return Member::Read; // a count that isn't a non-negative integer was skipped by readUnsigned()
    };
    // Each level descends into the one member that leads there; anything else is skipped.
    const auto into = [&reader](std::string_view wanted, const auto& next)
    {
        return [&reader, wanted, &next](const std::string& name)
        {
            if (name != wanted)
            {
                return Member::Skip;
            }
            return readObject(reader, next) ? Member::Read : Member::Malformed;
        };
    };
    const auto attributes = into("last_analysis_stats", readStats);
    const auto data = into("attributes", attributes);
    const auto top = into("data", data);

    const bool wellFormed = readObject(reader, top);
    reader.skipSpace();
    if (!wellFormed || !reader.atEnd() || seen != 0xFU)
    {
        return std::nullopt;
    }
    return verdict;
}

LookupResult interpret(int status, std::string_view body)
{
    switch (status)
    {
    case 200:
        if (const std::optional<Verdict> verdict = readFileReport(body))
        {
            return {.outcome = Outcome::Found, .verdict = verdict, .detail = {}};
        }
        return {.outcome = Outcome::Failed, .verdict = std::nullopt, .detail = "its reply couldn't be read"};
    case 404:
        return {.outcome = Outcome::Unknown, .verdict = std::nullopt, .detail = {}};
    case 401:
    case 403:
        return {.outcome = Outcome::KeyRefused, .verdict = std::nullopt, .detail = {}};
    case 429:
        return {.outcome = Outcome::RateLimited, .verdict = std::nullopt, .detail = {}};
    default:
        return {.outcome = Outcome::Failed, .verdict = std::nullopt, .detail = std::format("it answered HTTP {}", status)};
    }
}

std::string summary(std::string_view name, const LookupResult& result)
{
    switch (result.outcome)
    {
    case Outcome::Found:
    {
        if (!result.verdict.has_value())
        {
            break; // Found always carries one; read as a failure if it ever doesn't
        }
        const Verdict& v = *result.verdict;
        if (v.flagged() == 0)
        {
            return std::format("VirusTotal: no engine flags {} (0 of {})", name, v.engines());
        }
        return std::format("VirusTotal: {} of {} engines flag {} ({} malicious, {} suspicious)",
                           v.flagged(),
                           v.engines(),
                           name,
                           v.malicious,
                           v.suspicious);
    }
    case Outcome::Unknown:
        return std::format("VirusTotal doesn't know {}'s file: nobody has submitted it", name);
    case Outcome::KeyRefused:
        return "VirusTotal refused the API key: check it in Settings";
    case Outcome::RateLimited:
        return "VirusTotal's request limit for this API key was reached: try again in a minute";
    case Outcome::Failed:
        break;
    }
    return std::format("Couldn't look up {} on VirusTotal: {}", name, result.detail);
}

} // namespace Domain::VirusTotal
