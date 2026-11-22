#include "Platform/Linux/CronTab.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string_view>

// A crontab as the Startup tab reads it (#4357): a user's own (`crontab -l`, which the user writes) and a system table
// (/etc/crontab, /etc/cron.d, with a user column). TaskSmack can run as root while it parses a table another user
// wrote. Nothing may read past the input; each entry's line is one of the input's lines; a user's own table names no
// user.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) // NOLINT(readability-identifier-naming)
{
    const std::string_view text(reinterpret_cast<const char*>(data), size);
    const auto lines = static_cast<std::size_t>(std::ranges::count(text, '\n')) + 1;
    for (const bool withUser : {false, true})
    {
        for (const Platform::CronTab::Entry& entry : Platform::CronTab::parse(text, withUser))
        {
            if (entry.line == 0 || entry.line > lines || (!withUser && !entry.user.empty()))
            {
                __builtin_trap();
            }
        }
    }
    return 0;
}
