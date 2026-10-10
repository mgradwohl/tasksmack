#pragma once

// The Linux Recent crashes facts (#1524): the core files systemd-coredump keeps in
// /var/lib/systemd/coredump, read under an injected root ("/" in the app, a fixture tree in tests). Only
// the directory is listed; no core file is opened. Each name encodes the crash:
// "core.<comm>.<uid>.<boot id>.<pid>.<usec>[.<compression>]", where comm has '.', '/' and ' ' escaped as
// "\xNN" and usec is the crash time in microseconds since the epoch. Files of the last CRASH_HISTORY_DAYS
// days are kept, newest first, at most CRASH_LIST_MAX. Journal details (signal, executable path, the
// COREDUMP_* fields) aren't read here.
// Standard library only, so the fixture tests and the fuzzer (fuzz_coredump_names) run on every platform.

#include "Platform/ISystemInfoProbe.h"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace Platform::LinuxCoredumps
{

/// Where systemd-coredump stores core files, relative to the root.
inline constexpr std::string_view COREDUMP_DIR = "var/lib/systemd/coredump";

/// One core file's name, parsed.
struct CoredumpName
{
    std::string comm;        ///< The crashed process's name, unescaped ("python3.12")
    std::uint32_t uid = 0;   ///< Its user id
    std::string bootId;      ///< The boot it crashed in: 32 hex digits
    std::uint32_t pid = 0;   ///< Its process id
    std::uint64_t usec = 0;  ///< When, in microseconds since the epoch
    std::string compression; ///< "zst", "xz", "lz4"; empty when stored uncompressed
};

/// systemd's "\xNN" escapes decoded; any other backslash is kept as it is.
[[nodiscard]] inline std::string unescapeComm(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    constexpr std::size_t ESCAPE = 4; // "\xNN"
    constexpr int HEX = 16;
    std::size_t i = 0;
    while (i < text.size())
    {
        unsigned value = 0;
        if (text[i] == '\\' && i + ESCAPE <= text.size() && text[i + 1] == 'x')
        {
            const char* first = text.data() + i + 2;
            const auto [end, ec] = std::from_chars(first, first + 2, value, HEX);
            if (ec == std::errc{} && end == first + 2)
            {
                out += static_cast<char>(value);
                i += ESCAPE;
                continue;
            }
        }
        out += text[i];
        ++i;
    }
    return out;
}

/// A core file's name as its crash; nullopt for any other file (a "core.*" name of another shape, systemd's
/// ".#core..." temporary files, journal-only entries).
[[nodiscard]] inline std::optional<CoredumpName> parseCoredumpName(std::string_view name)
{
    constexpr std::string_view PREFIX = "core.";
    if (!name.starts_with(PREFIX))
    {
        return std::nullopt;
    }
    name.remove_prefix(PREFIX.size());
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    while (true)
    {
        const std::size_t dot = name.find('.', start);
        parts.push_back(name.substr(start, dot == std::string_view::npos ? std::string_view::npos : dot - start));
        if (dot == std::string_view::npos)
        {
            break;
        }
        start = dot + 1;
    }
    const auto isNumber = [](std::string_view text)
    {
        return !text.empty() && std::ranges::all_of(text, [](char c) { return c >= '0' && c <= '9'; });
    };
    const auto isBootId = [](std::string_view text)
    {
        constexpr std::size_t BOOT_ID_DIGITS = 32;
        return text.size() == BOOT_ID_DIGITS &&
               std::ranges::all_of(text, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); });
    };
    // comm (one part, or more if unescaped dots slipped in), uid, boot id, pid, usec, then the extension.
    constexpr std::size_t MIN_PARTS = 5;
    if (parts.size() < MIN_PARTS)
    {
        return std::nullopt;
    }
    const std::size_t n = parts.size();
    std::size_t boot = 0;
    if (isBootId(parts[n - 3]) && isNumber(parts[n - 2]) && isNumber(parts[n - 1]))
    {
        boot = n - 3;
    }
    else if (n > MIN_PARTS && isBootId(parts[n - 4]) && isNumber(parts[n - 3]) && isNumber(parts[n - 2]) && !parts[n - 1].empty())
    {
        boot = n - 4;
    }
    else
    {
        return std::nullopt;
    }
    if (boot < 2 || !isNumber(parts[boot - 1]))
    {
        return std::nullopt;
    }
    CoredumpName parsed;
    const std::string_view uid = parts[boot - 1];
    const std::string_view pid = parts[boot + 1];
    const std::string_view usec = parts[boot + 2];
    if (std::from_chars(uid.data(), uid.data() + uid.size(), parsed.uid).ec != std::errc{} ||
        std::from_chars(pid.data(), pid.data() + pid.size(), parsed.pid).ec != std::errc{} ||
        std::from_chars(usec.data(), usec.data() + usec.size(), parsed.usec).ec != std::errc{})
    {
        return std::nullopt;
    }
    // The comm is everything before the uid.
    const std::string_view comm = name.substr(0, static_cast<std::size_t>(uid.data() - name.data()) - 1);
    if (comm.empty())
    {
        return std::nullopt;
    }
    parsed.comm = unescapeComm(comm);
    parsed.bootId = std::string(parts[boot]);
    if (boot + 3 < n)
    {
        parsed.compression = std::string(parts[boot + 3]);
    }
    return parsed;
}

/// The core files under @p root of the CRASH_HISTORY_DAYS days before @p nowUnixSeconds (0: no age limit),
/// newest first, at most @p maxEvents.
inline void
readCoredumps(const std::filesystem::path& root, std::uint64_t nowUnixSeconds, CrashesInfo& info, std::size_t maxEvents = CRASH_LIST_MAX)
{
    info.available = true;
    info.family = OsFamily::Linux;
    const std::filesystem::path dir = root / COREDUMP_DIR;
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec))
    {
        info.unavailableReason = ec == std::errc::permission_denied
                                   ? "Listing /var/lib/systemd/coredump requires permission"
                                   : "No /var/lib/systemd/coredump: systemd-coredump isn't storing core dumps on this system";
        info.accessDenied = ec == std::errc::permission_denied;
        return;
    }
    std::filesystem::directory_iterator it(dir, ec);
    if (ec)
    {
        info.accessDenied = ec == std::errc::permission_denied;
        info.unavailableReason =
            info.accessDenied ? "Listing /var/lib/systemd/coredump requires permission" : "/var/lib/systemd/coredump couldn't be listed";
        return;
    }
    info.listed = true;
    constexpr std::uint64_t SECONDS_PER_DAY = 86'400;
    constexpr std::uint64_t USEC_PER_SECOND = 1'000'000;
    const std::uint64_t window = static_cast<std::uint64_t>(CRASH_HISTORY_DAYS) * SECONDS_PER_DAY;
    const std::uint64_t oldest = nowUnixSeconds > window ? nowUnixSeconds - window : 0;
    for (; it != std::filesystem::directory_iterator{}; it.increment(ec))
    {
        if (ec)
        {
            break;
        }
        const std::optional<CoredumpName> name = parseCoredumpName(it->path().filename().string());
        if (!name.has_value())
        {
            continue;
        }
        const std::uint64_t seconds = name->usec / USEC_PER_SECOND;
        if (nowUnixSeconds != 0 && seconds < oldest)
        {
            continue;
        }
        CrashEvent event;
        event.unixSeconds = seconds;
        event.application = name->comm;
        event.pid = name->pid;
        event.uid = name->uid;
        std::error_code sizeEc;
        const std::uintmax_t bytes = it->file_size(sizeEc);
        event.coreBytes = sizeEc ? 0 : static_cast<std::uint64_t>(bytes);
        info.events.push_back(std::move(event));
    }
    std::ranges::stable_sort(info.events, std::ranges::greater{}, &CrashEvent::unixSeconds);
    if (info.events.size() > maxEvents)
    {
        info.capped = true;
        info.events.resize(maxEvents);
    }
}

} // namespace Platform::LinuxCoredumps
