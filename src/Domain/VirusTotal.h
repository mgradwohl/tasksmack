#pragma once

// Look up on VirusTotal (#3094, the maintainer's design of 2026-11-12): the user's own API key, and only the
// executable's SHA-256 is sent (GET /api/v3/files/<sha256>, never the file). What the request is, how the reply
// reads, and the line the result leaves. Pure: the HTTPS client and the hash are the caller's.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace Domain::VirusTotal
{

inline constexpr std::string_view API_HOST = "www.virustotal.com";
inline constexpr std::string_view FILES_PATH_PREFIX = "/api/v3/files/";
inline constexpr std::string_view API_KEY_HEADER = "x-apikey";
inline constexpr std::string_view ACCEPT_HEADER = "application/json";
inline constexpr std::string_view REPORT_PAGE_PREFIX = "https://www.virustotal.com/gui/file/";
/// A file report holds every engine's result; a few hundred KB in practice.
inline constexpr std::size_t MAX_RESPONSE_BYTES = std::size_t{4} << 20U;
/// VirusTotal's API keys are 64 hex digits; anything shorter or longer, or with other characters, isn't sent.
inline constexpr std::size_t API_KEY_LENGTH = 64;

/// Whether @p text is a SHA-256 as imageSha256() writes it: 64 lowercase hex digits.
[[nodiscard]] bool isSha256Hex(std::string_view text) noexcept;

/// Whether @p key could be a VirusTotal API key (64 hex digits, either case). Checked before it goes in a header.
[[nodiscard]] bool isApiKey(std::string_view key) noexcept;

/// The request path for @p sha256 (already checked by isSha256Hex()).
[[nodiscard]] std::string filePath(std::string_view sha256);

/// The report page for @p sha256, for the "Open report" link.
[[nodiscard]] std::string reportPage(std::string_view sha256);

/// last_analysis_stats: how many engines said what of the file at its last analysis.
struct Verdict
{
    std::uint64_t malicious = 0;
    std::uint64_t suspicious = 0;
    std::uint64_t undetected = 0;
    std::uint64_t harmless = 0;

    /// The engines that flag it: malicious and suspicious.
    [[nodiscard]] std::uint64_t flagged() const noexcept
    {
        return malicious + suspicious;
    }
    /// The engines that gave a verdict, the denominator VirusTotal shows: the four above. Engines that timed out,
    /// failed or don't handle the file type aren't counted.
    [[nodiscard]] std::uint64_t engines() const noexcept
    {
        return malicious + suspicious + undetected + harmless;
    }

    friend bool operator==(const Verdict&, const Verdict&) = default;
};

/// data.attributes.last_analysis_stats from a file report, or nullopt when @p json isn't one (not an object,
/// too long, too deep, or without those four counts).
[[nodiscard]] std::optional<Verdict> readFileReport(std::string_view json);

/// What a lookup came to.
enum class Outcome : std::uint8_t
{
    Found,       ///< The report was read: verdict holds its counts
    Unknown,     ///< 404: VirusTotal has never seen this file
    KeyRefused,  ///< 401 or 403: the key is wrong, or not allowed this call
    RateLimited, ///< 429: the public API's 4 a minute or 500 a day were used up
    Failed,      ///< Anything else: another status, or a report that can't be read
};

struct LookupResult
{
    Outcome outcome = Outcome::Failed;
    std::optional<Verdict> verdict;
    std::string detail; ///< For Failed: the status or why the reply couldn't be read
};

/// The reply's @p status and @p body as a LookupResult.
[[nodiscard]] LookupResult interpret(int status, std::string_view body);

/// The result line for process @p name: "VirusTotal: 3 of 72 engines flag notepad.exe (2 malicious,
/// 1 suspicious)", "VirusTotal: no engine flags notepad.exe (0 of 70)", "VirusTotal doesn't know notepad.exe's
/// file", and so on.
[[nodiscard]] std::string summary(std::string_view name, const LookupResult& result);

} // namespace Domain::VirusTotal
