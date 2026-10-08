#pragma once

// Pure helpers behind IProcessEnvironmentReader (#179): parsing a raw environment block into
// NAME=VALUE entries, escaping what a process put there so it is safe to draw, and turning an OS
// error into a read status. Free of OS calls, so they build and are tested on every platform
// (tests/Platform/test_ProcessEnvironment.cpp).

#include "Platform/IProcessEnvironment.h"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Platform::Environment
{

namespace Detail
{

/// Length of the well-formed UTF-8 sequence at the start of @p text, or 0 if it does not start with
/// one (a stray continuation byte, a truncated sequence, an overlong form, a UTF-16 surrogate or a
/// code point past U+10FFFF). @p text must not be empty.
[[nodiscard]] constexpr std::size_t utf8SequenceLength(std::string_view text) noexcept
{
    const auto byteAt = [text](std::size_t i)
    {
        return static_cast<std::uint8_t>(text[i]);
    };
    const std::uint8_t lead = byteAt(0);
    if (lead < 0x80U)
    {
        return 1;
    }
    std::size_t length = 0;
    std::uint8_t secondMin = 0x80U; // Bounds on the second byte rule out overlongs, surrogates and > U+10FFFF
    std::uint8_t secondMax = 0xBFU;
    if (lead >= 0xC2U && lead <= 0xDFU)
    {
        length = 2;
    }
    else if (lead >= 0xE0U && lead <= 0xEFU)
    {
        length = 3;
        secondMin = (lead == 0xE0U) ? std::uint8_t{0xA0U} : secondMin; // overlong
        secondMax = (lead == 0xEDU) ? std::uint8_t{0x9FU} : secondMax; // surrogates
    }
    else if (lead >= 0xF0U && lead <= 0xF4U)
    {
        length = 4;
        secondMin = (lead == 0xF0U) ? std::uint8_t{0x90U} : secondMin; // overlong
        secondMax = (lead == 0xF4U) ? std::uint8_t{0x8FU} : secondMax; // past U+10FFFF
    }
    else
    {
        return 0; // a continuation byte, C0/C1 (overlong two-byte), or F5..FF
    }
    if (text.size() < length || byteAt(1) < secondMin || byteAt(1) > secondMax)
    {
        return 0;
    }
    for (std::size_t i = 2; i < length; ++i)
    {
        if ((byteAt(i) & 0xC0U) != 0x80U)
        {
            return 0;
        }
    }
    return length;
}

/// Upper-case hex digits for "\xNN" escapes.
inline constexpr std::string_view HEX_DIGITS = "0123456789ABCDEF";

} // namespace Detail

/// @p text made safe to draw on one line: well-formed UTF-8 is kept as it is, while each byte that is
/// not part of a well-formed sequence becomes "\xNN", and control characters are escaped ("\n", "\t",
/// "\r", otherwise "\xNN"), so a value cannot break a table row or hand ImGui invalid UTF-8.
[[nodiscard]] inline std::string escapeForDisplay(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    while (!text.empty())
    {
        const std::size_t length = Detail::utf8SequenceLength(text);
        const auto byte = static_cast<std::uint8_t>(text.front());
        if (length > 1 || (length == 1 && byte >= 0x20U && byte != 0x7FU))
        {
            out.append(text.substr(0, length)); // printable ASCII or a whole UTF-8 sequence
            text.remove_prefix(length);
            continue;
        }
        switch (byte)
        {
        case '\n':
            out += "\\n";
            break;
        case '\t':
            out += "\\t";
            break;
        case '\r':
            out += "\\r";
            break;
        default:
            // Other controls and invalid bytes: "\xNN" appended directly, not through std::format -- a
            // binary value of a megabyte would otherwise be a million format calls in one read.
            out += "\\x";
            out += Detail::HEX_DIGITS[byte >> 4U];
            out += Detail::HEX_DIGITS[byte & 0x0FU];
            break;
        }
        text.remove_prefix(1);
    }
    return out;
}

/// The entries of a raw environment block: NUL-separated NAME=VALUE strings, as /proc/[pid]/environ
/// holds them.
///
/// - Empty pieces (an empty block, the trailing NUL, doubled NULs) are skipped.
/// - The name ends at the first '=' after its first character, so a value may itself contain '='
///   and a leading '=' stays in the name (Windows-style "=C:=C:\dir").
/// - An entry without '=' is kept as a name with an empty value rather than dropped.
/// - Both halves are passed through escapeForDisplay().
/// Entries are returned in block order; sorting is up to the caller.
[[nodiscard]] inline std::vector<EnvironmentVariable> parseEnvironBlock(std::string_view raw)
{
    std::vector<EnvironmentVariable> variables;
    while (!raw.empty())
    {
        const std::size_t nul = raw.find('\0');
        const std::string_view entry = raw.substr(0, nul);
        raw.remove_prefix((nul == std::string_view::npos) ? raw.size() : nul + 1);
        if (entry.empty())
        {
            continue;
        }
        const std::size_t equals = entry.find('=', 1);
        const std::string_view name = entry.substr(0, equals);
        const std::string_view value = (equals == std::string_view::npos) ? std::string_view{} : entry.substr(equals + 1);
        variables.push_back({.name = escapeForDisplay(name), .value = escapeForDisplay(value)});
    }
    return variables;
}

/// The status a failed environment read reports for the OS error @p err (an errno value):
/// EACCES/EPERM are PermissionDenied (another user's process), ENOENT/ESRCH are ProcessExited (its
/// /proc directory is gone, or the directory handle outlived the process), anything else Failed.
[[nodiscard]] constexpr EnvironmentReadStatus statusFromErrno(int err) noexcept
{
    switch (err)
    {
    case EACCES:
    case EPERM:
        return EnvironmentReadStatus::PermissionDenied;
    case ENOENT:
    case ESRCH:
        return EnvironmentReadStatus::ProcessExited;
    default:
        return EnvironmentReadStatus::Failed;
    }
}

} // namespace Platform::Environment
