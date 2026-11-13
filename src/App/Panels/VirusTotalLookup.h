#pragma once

// Look up on VirusTotal (#3094), shared by the Processes row menu and Process Details' Actions: on the action
// worker, the process's image is hashed (IProcessActions::imageSha256), only that hash is sent with the user's own
// key (Domain::VirusTotal), and the reply becomes the result line. Nothing runs on the UI thread.

#include "App/Panels/ProcessDetailsPanel_ActionHelpers.h"
#include "Domain/UpdateCheck.h"
#include "Domain/VirusTotal.h"
#include "Platform/IHttpsClient.h"
#include "Platform/IProcessActions.h"

#include <chrono>
#include <expected>
#include <format>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>

namespace App::VirusTotalLookup
{

inline constexpr const char* MENU_LABEL = "Look up on VirusTotal";
inline constexpr const char* TOOLTIP = "Sends this executable's SHA-256, and nothing else, to VirusTotal with your API key, "
                                       "and shows how many security engines flag it. The key is set in Settings.";
/// The most a lookup waits on each step of the request (name lookup, connect, send, receive).
inline constexpr std::chrono::milliseconds REQUEST_TIMEOUT{15'000};

/// What a lookup needs besides the process: the client, the user's key and the User-Agent.
struct Context
{
    std::shared_ptr<Platform::IHttpsClient> client;
    std::string apiKey;
    std::string userAgent;
};

/// Whether the lookup can be offered: the platform can hash images, there's a client, and the key is one.
[[nodiscard]] inline bool offered(const Platform::ProcessActionCapabilities& capabilities, const Context& context) noexcept
{
    return capabilities.canHashImage && context.client != nullptr && Domain::VirusTotal::isApiKey(context.apiKey);
}

/// The worker's half: hash, ask, read. The result line is in successNote when VirusTotal answered with a report or
/// "unknown", in errorMessage otherwise; flagged() > 0 also comes back as a failure, so the line draws in the error
/// colour.
[[nodiscard]] inline Platform::ProcessActionResult lookUp(Platform::IProcessActions& actions,
                                                          const Context& context,
                                                          const Platform::ProcessTarget& target,
                                                          std::string_view name,
                                                          const std::stop_token& stop)
{
    if (context.client == nullptr || !Domain::VirusTotal::isApiKey(context.apiKey))
    {
        return Platform::ProcessActionResult::error("No VirusTotal API key: set one in Settings");
    }
    const std::expected<std::string, std::string> sha256 = actions.imageSha256(target, stop);
    if (!sha256.has_value())
    {
        return Platform::ProcessActionResult::error(std::format("Couldn't hash {}'s executable: {}", name, sha256.error()));
    }
    if (!Domain::VirusTotal::isSha256Hex(*sha256))
    {
        return Platform::ProcessActionResult::error(std::format("Couldn't hash {}'s executable", name));
    }
    const Platform::HttpsRequest request{
        .host = std::string(Domain::VirusTotal::API_HOST),
        .path = Domain::VirusTotal::filePath(*sha256),
        .userAgent = context.userAgent,
        .accept = std::string(Domain::VirusTotal::ACCEPT_HEADER),
        .timeout = REQUEST_TIMEOUT,
        .maxBodyBytes = Domain::VirusTotal::MAX_RESPONSE_BYTES,
        .headers = {{std::string(Domain::VirusTotal::API_KEY_HEADER), context.apiKey}},
    };
    const std::expected<Platform::HttpsResponse, std::string> response = context.client->get(request, stop);
    if (!response.has_value())
    {
        return Platform::ProcessActionResult::error(std::format("Couldn't reach VirusTotal: {}", response.error()));
    }
    const Domain::VirusTotal::LookupResult result = Domain::VirusTotal::interpret(response->status, response->body);
    std::string line = Domain::VirusTotal::summary(name, result);
    const bool flagged = result.verdict.has_value() && result.verdict->flagged() > 0;
    if (result.outcome == Domain::VirusTotal::Outcome::Found || result.outcome == Domain::VirusTotal::Outcome::Unknown)
    {
        line += std::format(" ({})", Domain::VirusTotal::reportPage(*sha256));
        return flagged ? Platform::ProcessActionResult::error(std::move(line)) : Platform::ProcessActionResult::ok(std::move(line));
    }
    return Platform::ProcessActionResult::error(std::move(line));
}

/// The result line: the worker already wrote it.
[[nodiscard]] inline Detail::ActionResultMessage format(const Platform::ProcessActionResult& result)
{
    return {.ok = result.success, .text = result.success ? result.successNote : result.errorMessage, .neutral = false};
}

} // namespace App::VirusTotalLookup
