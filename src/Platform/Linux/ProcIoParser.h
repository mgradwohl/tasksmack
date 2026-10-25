#pragma once

// /proc/[pid]/io's read_bytes and write_bytes (#2728): pure parsing, shared by LinuxProcessProbe, which
// reads its own user's processes, and tasksmack-helper, which reads other users' for it
// (ProcessCountersRead.h). Fuzzed through the probe's tests' corpus like the other /proc parsers.

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <system_error>

namespace Platform::Linux::ProcIoParser
{

struct ProcIo
{
    std::uint64_t readBytes = 0;
    std::uint64_t writeBytes = 0;
};

/// read_bytes and write_bytes from @p text, the file's "key: value" lines; nullopt unless both parse.
[[nodiscard]] inline std::optional<ProcIo> parse(std::string_view text)
{
    constexpr std::string_view READ_PREFIX = "read_bytes:";
    constexpr std::string_view WRITE_PREFIX = "write_bytes:";
    ProcIo io;
    bool hasRead = false;
    bool hasWrite = false;
    while (!text.empty())
    {
        const std::size_t eol = std::min(text.find('\n'), text.size());
        std::string_view value = text.substr(0, eol);
        text.remove_prefix(std::min(eol + 1, text.size()));
        const bool isRead = value.starts_with(READ_PREFIX);
        if (!isRead && !value.starts_with(WRITE_PREFIX))
        {
            continue;
        }
        value.remove_prefix(isRead ? READ_PREFIX.size() : WRITE_PREFIX.size());
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
        {
            value.remove_prefix(1);
        }
        std::uint64_t bytes = 0;
        if (std::from_chars(value.data(), value.data() + value.size(), bytes).ec != std::errc{})
        {
            continue;
        }
        (isRead ? io.readBytes : io.writeBytes) = bytes;
        (isRead ? hasRead : hasWrite) = true;
    }
    if (!hasRead || !hasWrite)
    {
        return std::nullopt;
    }
    return io;
}

} // namespace Platform::Linux::ProcIoParser
