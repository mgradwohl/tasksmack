#pragma once

#include <cctype>
#include <string_view>

namespace Core
{

/// Returns true if the environment variable value represents a truthy flag.
/// "0", "false", "off", "no" (case-insensitive) and nullptr/empty are treated
/// as disabled. Any other non-empty value (e.g. "1", "true", "yes", "on") is
/// treated as enabled.
[[nodiscard]] inline bool isEnvFlagEnabled(const char* value) noexcept
{
    if (value == nullptr)
    {
        return false;
    }

    const std::string_view text(value);
    if (text.empty())
    {
        return false;
    }

    // Case-insensitive equality; b must already be lowercase.
    const auto ciEqual = [](const std::string_view a, const std::string_view b) noexcept
    {
        if (a.size() != b.size())
        {
            return false;
        }
        for (std::size_t i = 0; i < a.size(); ++i)
        {
            if (std::tolower(static_cast<unsigned char>(a[i])) != b[i])
            {
                return false;
            }
        }
        return true;
    };

    return !ciEqual(text, "0") && !ciEqual(text, "false") && !ciEqual(text, "off") && !ciEqual(text, "no");
}

} // namespace Core
