#pragma once

// The Linux Recent crashes facts (#1524): the core files systemd-coredump keeps in
// /var/lib/systemd/coredump, read under an injected root ("/" in the app, a fixture tree in tests). Only
// the directory is listed; no core file is opened. Each name encodes the crash:
// "core.<comm>.<uid>.<boot id>.<pid>.<usec>[.<compression>]", where comm has '.', '/' and ' ' escaped as
// "\xNN" and usec is the crash time in microseconds since the epoch. Files of the last CRASH_HISTORY_DAYS
// days are kept, newest first, at most CRASH_LIST_MAX.
// systemd-coredump also records each crash in the journal, with the signal and the executable, and keeps
// that record when the core file itself wasn't kept (Storage=none, too large, vacuumed). readCrashFacts()
// merges an injected journal read (SystemdJournal.cpp, #1674) with the directory: the journal's entries
// first, then any core file the journal didn't return (another user's, when this one can't read theirs).
// Standard library only, so the fixture tests and the fuzzer (fuzz_coredump_names) run on every platform.

#include "Platform/ISystemInfoProbe.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
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

/// The oldest crash time kept, in seconds since the epoch: CRASH_HISTORY_DAYS before @p nowUnixSeconds.
[[nodiscard]] inline std::uint64_t oldestKeptSeconds(std::uint64_t nowUnixSeconds)
{
    constexpr std::uint64_t SECONDS_PER_DAY = 86'400;
    const std::uint64_t window = static_cast<std::uint64_t>(CRASH_HISTORY_DAYS) * SECONDS_PER_DAY;
    return nowUnixSeconds > window ? nowUnixSeconds - window : 0;
}

/// Newest first, at most @p maxEvents (capped when more).
inline void sortAndCap(CrashesInfo& info, std::size_t maxEvents)
{
    std::ranges::stable_sort(info.events, std::ranges::greater{}, &CrashEvent::unixSeconds);
    if (info.events.size() > maxEvents)
    {
        info.capped = true;
        info.events.resize(maxEvents);
    }
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
    constexpr std::uint64_t USEC_PER_SECOND = 1'000'000;
    const std::uint64_t oldest = oldestKeptSeconds(nowUnixSeconds);
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
        event.coreKept = true;
        info.events.push_back(std::move(event));
    }
    sortAndCap(info, maxEvents);
}

/// One systemd-coredump journal entry's fields, by name without the "COREDUMP_" prefix ("SIGNAL", "EXE").
using JournalFields = std::vector<std::pair<std::string, std::string>>;

/// What a journal read returned.
struct JournalRead
{
    bool opened = false;                ///< The journal could be read; entries may still be empty
    std::string error;                  ///< Why not, when !opened
    std::vector<JournalFields> entries; ///< Newest first
};

/// Reads the systemd-coredump entries since @p sinceUsec (microseconds since the epoch), newest first,
/// at most @p maxEntries. The app passes SystemdJournal::makeCoredumpJournalReader(); tests pass a fake.
using JournalReader = std::function<JournalRead(std::uint64_t sinceUsec, std::size_t maxEntries)>;

/// A signal number's name ("SIGSEGV"); "signal N" for one this table doesn't name. The ones a core dump
/// comes from, plus the common others.
[[nodiscard]] inline std::string signalName(int number)
{
    constexpr std::array<std::pair<int, std::string_view>, 13> NAMES{{
        {1, "SIGHUP"},
        {2, "SIGINT"},
        {3, "SIGQUIT"},
        {4, "SIGILL"},
        {5, "SIGTRAP"},
        {6, "SIGABRT"},
        {7, "SIGBUS"},
        {8, "SIGFPE"},
        {9, "SIGKILL"},
        {11, "SIGSEGV"},
        {24, "SIGXCPU"},
        {25, "SIGXFSZ"},
        {31, "SIGSYS"},
    }};
    for (const auto& [value, name] : NAMES)
    {
        if (value == number)
        {
            return std::string(name);
        }
    }
    return "signal " + std::to_string(number);
}

/// The value of field @p name in @p fields; empty when absent.
[[nodiscard]] inline std::string_view journalField(const JournalFields& fields, std::string_view name)
{
    const auto it = std::ranges::find(fields, name, &std::pair<std::string, std::string>::first);
    return it != fields.end() ? std::string_view(it->second) : std::string_view{};
}

/// A journal entry as its crash; nullopt when it names no process. @p coreFile receives the kept core
/// file's path (as the journal gives it, absolute), or stays empty when none was kept.
[[nodiscard]] inline std::optional<CrashEvent> crashFromJournal(const JournalFields& fields, std::string& coreFile)
{
    const auto number = []<typename T>(std::string_view text, T& value)
    {
        return !text.empty() && std::from_chars(text.data(), text.data() + text.size(), value).ec == std::errc{};
    };
    CrashEvent event;
    event.application = std::string(journalField(fields, "COMM"));
    std::uint32_t pid = 0;
    if (number(journalField(fields, "PID"), pid))
    {
        event.pid = pid;
    }
    if (event.application.empty() && !event.pid.has_value())
    {
        return std::nullopt;
    }
    std::uint32_t uid = 0;
    if (number(journalField(fields, "UID"), uid))
    {
        event.uid = uid;
    }
    constexpr std::uint64_t USEC_PER_SECOND = 1'000'000;
    std::uint64_t usec = 0;
    if (number(journalField(fields, "TIMESTAMP"), usec))
    {
        event.unixSeconds = usec / USEC_PER_SECOND;
    }
    event.signal = std::string(journalField(fields, "SIGNAL_NAME"));
    int signal = 0;
    if (event.signal.empty() && number(journalField(fields, "SIGNAL"), signal))
    {
        event.signal = signalName(signal);
    }
    event.executable = std::string(journalField(fields, "EXE"));
    coreFile = std::string(journalField(fields, "FILENAME"));
    event.coreKept = !coreFile.empty();
    return event;
}

/// The Recent crashes facts: @p journal's systemd-coredump entries, when it can be read, merged with the
/// core files under @p root (readCoredumps()); the directory alone when @p journal is empty or fails.
inline void readCrashFacts(const std::filesystem::path& root,
                           std::uint64_t nowUnixSeconds,
                           CrashesInfo& info,
                           const JournalReader& journal = {},
                           std::size_t maxEvents = CRASH_LIST_MAX)
{
    // Every core file, uncapped, so the merge below can drop the ones the journal already has.
    readCoredumps(root, nowUnixSeconds, info, std::numeric_limits<std::size_t>::max());
    if (!journal)
    {
        sortAndCap(info, maxEvents);
        return;
    }
    constexpr std::uint64_t USEC_PER_SECOND = 1'000'000;
    const JournalRead read = journal(oldestKeptSeconds(nowUnixSeconds) * USEC_PER_SECOND, maxEvents + 1);
    if (!read.opened)
    {
        sortAndCap(info, maxEvents);
        return;
    }

    std::vector<CrashEvent> merged;
    for (const JournalFields& fields : read.entries)
    {
        std::string coreFile;
        std::optional<CrashEvent> event = crashFromJournal(fields, coreFile);
        if (!event.has_value())
        {
            continue;
        }
        if (!coreFile.empty())
        {
            std::error_code sizeEc;
            const std::uintmax_t bytes = std::filesystem::file_size(root / std::filesystem::path(coreFile).relative_path(), sizeEc);
            event->coreBytes = sizeEc ? 0 : static_cast<std::uint64_t>(bytes);
        }
        merged.push_back(std::move(*event));
    }
    // A core file the journal didn't return -- another user's, whose entries this one can't read -- is
    // still listed. Its name holds the same pid and crash time as the journal's entry for it.
    const std::size_t fromJournal = merged.size();
    for (CrashEvent& file : info.events)
    {
        const auto same = [&file](const CrashEvent& entry)
        {
            return entry.pid == file.pid && entry.unixSeconds == file.unixSeconds;
        };
        if (std::none_of(merged.begin(), merged.begin() + static_cast<std::ptrdiff_t>(fromJournal), same))
        {
            merged.push_back(std::move(file));
        }
    }
    info.events = std::move(merged);
    info.listed = true;
    info.accessDenied = false;
    info.unavailableReason.clear();
    info.capped = read.entries.size() > maxEvents;
    sortAndCap(info, maxEvents);
}

} // namespace Platform::LinuxCoredumps
