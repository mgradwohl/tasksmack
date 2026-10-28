#pragma once

// The opt-in update check's decisions (#3093): what TaskSmack asks GitHub, how it reads the answer,
// whether that answer is a newer release, and when the next check is due. Pure: no sockets, no clock
// of its own and no files, so all of it is unit-tested (and fuzzed) on every host. The request itself
// is Platform::IHttpsClient's; App::UpdateChecker runs it on a worker and keeps the result.
//
// What goes over the network, and only while the user has ticked "Check for updates daily" (or clicked
// "Check now" with it ticked): one HTTPS GET of https://api.github.com/repos/mgradwohl/tasksmack/releases/latest
// with a "TaskSmack/<version>" User-Agent and GitHub's JSON Accept header. No identifier, cookie or
// credential. The reply's `tag_name` is compared with the running version, and its `html_url` is the
// page the notice links to. Nothing is downloaded or installed.

#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Domain::UpdateCheck
{

/// The host and path of the one request the check makes.
inline constexpr std::string_view API_HOST = "api.github.com";
inline constexpr std::string_view LATEST_RELEASE_PATH = "/repos/mgradwohl/tasksmack/releases/latest";

/// GitHub's media type for its REST API, sent as the request's Accept header.
inline constexpr std::string_view ACCEPT_HEADER = "application/vnd.github+json";

/// Every release page starts with this. A reply's `html_url` that doesn't is not linked to; the
/// latest-release page is, instead.
inline constexpr std::string_view RELEASES_PAGE_PREFIX = "https://github.com/mgradwohl/tasksmack/releases/";
inline constexpr std::string_view LATEST_RELEASE_PAGE = "https://github.com/mgradwohl/tasksmack/releases/latest";

/// The most of a reply that is read: GitHub's answer for a release with its assets is tens of KB.
inline constexpr std::size_t MAX_RESPONSE_BYTES = std::size_t{1} << 20U;

/// The longest version text kept (config.toml's latest_version and dismissed_version), and the longest
/// release URL; anything longer is no version or URL TaskSmack would show.
inline constexpr std::size_t MAX_VERSION_TEXT_BYTES = 64;
inline constexpr std::size_t MAX_URL_BYTES = 512;

/// The User-Agent the request sends: "TaskSmack/0.10.0". GitHub refuses a request without one.
[[nodiscard]] std::string userAgent(std::string_view currentVersion);

// ---------------------------------------------------------------------------------------------------
// Versions
// ---------------------------------------------------------------------------------------------------

/// A Semantic Versioning 2.0.0 version: MAJOR.MINOR.PATCH, an optional pre-release ("rc.1") and an
/// optional build ("+sha.abc", which precedence ignores).
struct Version
{
    std::uint32_t major = 0;
    std::uint32_t minor = 0;
    std::uint32_t patch = 0;
    std::vector<std::string> prerelease; ///< Its dot-separated identifiers; empty for a release
    std::string build;                   ///< After '+', or empty

    /// "0.10.0", "1.2.3-rc.1": without the build, which doesn't change what the version is.
    [[nodiscard]] std::string text() const;
};

/// @p text as a version: "v0.10.0", "V1.2.3-rc.1+build.7" or "1.2.3". One optional 'v' or 'V' first;
/// otherwise strict SemVer: three numbers without leading zeros, each fitting 32 bits, and non-empty
/// [0-9A-Za-z-] identifiers (a numeric pre-release identifier without leading zeros). nullopt for
/// anything else: "", "1.2", "1.2.3.4", " 1.2.3", "01.2.3", "1.2.3-", "1.2.3-01", "latest".
[[nodiscard]] std::optional<Version> parseVersion(std::string_view text);

/// SemVer precedence: the numbers in order, then a release above any of its pre-releases, then the
/// pre-release identifiers in order (numeric ones by value and below alphanumeric ones, alphanumeric
/// ones in ASCII order, and a shorter list below a longer one it begins). The build is ignored.
[[nodiscard]] std::strong_ordering compare(const Version& left, const Version& right);

// ---------------------------------------------------------------------------------------------------
// The reply
// ---------------------------------------------------------------------------------------------------

/// What is read from GitHub's release JSON: its top-level members only, so the `html_url` of the
/// release's author or of an asset is never taken for the release's own.
struct ReleaseFields
{
    std::optional<std::string> tagName; ///< "tag_name"
    std::optional<std::string> htmlUrl; ///< "html_url"
    std::optional<bool> draft;          ///< "draft"
    std::optional<bool> prerelease;     ///< "prerelease"
};

/// Reads @p json, a JSON object, for its top-level "tag_name", "html_url", "draft" and "prerelease".
/// A member that is there with another type is left unset; a repeated one keeps its last value. The
/// object itself must be well formed (strings, numbers and literals as JSON has them, brackets
/// balanced, nothing after it but whitespace); nested values are skipped without recursion, so no
/// input can exhaust the stack. nullopt for text that isn't, or is longer than MAX_RESPONSE_BYTES.
[[nodiscard]] std::optional<ReleaseFields> readReleaseFields(std::string_view json);

/// Whether @p url is a page under RELEASES_PAGE_PREFIX, made only of characters a URL needs
/// (unreserved, '/', '%' and '+'), no longer than MAX_URL_BYTES.
[[nodiscard]] bool isReleasePageUrl(std::string_view url);

/// How a check ended.
struct Outcome
{
    enum class Kind : std::uint8_t
    {
        UpToDate,  ///< The latest release is this version, or older
        Available, ///< A newer release: latestVersion and releaseUrl say which and where
        Failed,    ///< No answer, or none TaskSmack could read: detail says why (for the debug log)
    };

    std::string latestVersion; ///< "0.10.0": the latest release's version, without the 'v'; empty when Failed
    std::string releaseUrl;    ///< Its page, always one isReleasePageUrl() accepts; empty when Failed
    std::string detail;        ///< Why it failed; empty otherwise
    Kind kind = Kind::Failed;  ///< Last, so the strings pack without padding between them
};

/// The check's outcome from GitHub's answer: HTTP @p status and @p body, against @p current, the
/// running version. Anything but a 200 with a readable release is Failed. A draft or pre-release (which
/// /releases/latest never returns) is never offered, nor a tag that isn't a version.
[[nodiscard]] Outcome evaluateResponse(int status, std::string_view body, const Version& current);

/// The Failed outcome for a request that got no answer, for @p reason.
[[nodiscard]] Outcome failed(std::string reason);

// ---------------------------------------------------------------------------------------------------
// When to check, and what to show
// ---------------------------------------------------------------------------------------------------

/// What config.toml's [updates] table keeps (UserConfig): the setting, and what the last check found.
struct Persisted
{
    /// When the last check started, in seconds since the Unix epoch; 0 when there has been none.
    std::int64_t lastCheckUnixSeconds = 0;
    std::string latestVersion;    ///< The latest release the last successful check saw ("0.10.0"), or empty
    std::string latestUrl;        ///< Its release page, or empty
    std::string dismissedVersion; ///< The version whose notice the user dismissed, or empty
    bool enabled = false;         ///< "Check for updates daily"; off by default, and then nothing is sent
};

/// The wall-clock time @p unixSeconds names, or nullopt for 0 (never) or a negative value.
[[nodiscard]] std::optional<std::chrono::system_clock::time_point> fromUnixSeconds(std::int64_t unixSeconds);

/// @p time in whole seconds since the Unix epoch.
[[nodiscard]] std::int64_t toUnixSeconds(std::chrono::system_clock::time_point time);

/// Whether the daily check is due at @p now: only while @p enabled, and then when there has been no
/// check, the last was UPDATE_CHECK_INTERVAL_HOURS or more ago, or the clock now reads earlier than the
/// last check (it was set back, and waiting for it to catch up could take any time).
[[nodiscard]] bool
isCheckDue(bool enabled, std::optional<std::chrono::system_clock::time_point> lastCheck, std::chrono::system_clock::time_point now);

/// The release the notice offers, when it offers one.
struct Notice
{
    std::string version; ///< "0.10.0"
    std::string url;     ///< Its release page
};

/// What the notice shows for @p state against @p current: the last-seen release while checks are on,
/// it is newer than @p current, the user hasn't dismissed that version, and its page is a release page.
[[nodiscard]] std::optional<Notice> noticeFor(const Persisted& state, const Version& current);

/// The notice's text: "TaskSmack v0.10.0 is available".
[[nodiscard]] std::string noticeText(std::string_view version);

} // namespace Domain::UpdateCheck
