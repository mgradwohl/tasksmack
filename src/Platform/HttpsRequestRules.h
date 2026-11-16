#pragma once

// What IHttpsClient refuses to send (#3870): one rule for every platform's client, checked before anything is
// opened. LinuxHttpsClient and WindowsHttpsClient used to check different parts of it.

#include "Platform/IHttpsClient.h"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>

namespace Platform
{

/// Why @p request is refused before anything is sent, or nullopt when it can be sent:
/// - an empty host, path or User-Agent, or a path that doesn't start with '/';
/// - a host that isn't a plain DNS name (letters, digits, '.' and '-'), so nothing in it can change the URL;
/// - a path holding '#', a space, CR, LF or NUL, which would start a fragment or break the request line;
/// - a User-Agent or Accept holding CR, LF or NUL (header injection);
/// - an extra header whose name isn't an RFC 9110 token, or whose value holds CR, LF or NUL.
/// The messages never quote a header's value, which may be a secret such as VirusTotal's API key.
[[nodiscard]] inline std::optional<std::string> requestRefusal(const HttpsRequest& request)
{
    if (request.host.empty() || request.path.empty() || request.userAgent.empty())
    {
        return "The request's host, path or User-Agent is empty";
    }
    if (!request.path.starts_with('/'))
    {
        return "The request's path doesn't start with '/'";
    }
    const auto dnsCharacter = [](char c)
    {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-';
    };
    if (!std::ranges::all_of(request.host, dnsCharacter))
    {
        return "The request's host isn't a plain host name";
    }
    // The path goes into the URL as it is: nothing in it may start a fragment or break the line.
    if (std::ranges::any_of(request.path, [](char c) { return c == '#' || c == ' ' || c == '\r' || c == '\n' || c == '\0'; }))
    {
        return "The request's path holds a character a URL path can't";
    }
    const auto breaksTheLine = [](std::string_view value)
    {
        return std::ranges::any_of(value, [](char c) { return c == '\r' || c == '\n' || c == '\0'; });
    };
    if (breaksTheLine(request.userAgent))
    {
        return "The request's User-Agent holds a line break";
    }
    if (breaksTheLine(request.accept))
    {
        return "The request's Accept header holds a line break";
    }
    // Extra headers (#3094): a name is an RFC 9110 token, and a value can't end the header or the request.
    const auto tokenCharacter = [](char c)
    {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               std::string_view("!#$%&'*+-.^_`|~").contains(c);
    };
    for (const auto& [name, value] : request.headers)
    {
        if (name.empty() || !std::ranges::all_of(name, tokenCharacter))
        {
            return "A request header's name isn't a header token";
        }
        if (breaksTheLine(value))
        {
            return "A request header's value holds a line break";
        }
    }
    return std::nullopt;
}

} // namespace Platform
