#pragma once

#include <cctype>
#include <string>

namespace Platform
{

/// Normalize a GPU name for comparison: lowercase, and collapse runs of whitespace
/// (including leading/trailing) to single spaces / nothing. Used to match DXGI and NVML
/// names for the same physical adapter, which are rarely byte-identical.
[[nodiscard]] inline std::string normalizeGPUName(const std::string& name)
{
    std::string normalized;
    normalized.reserve(name.size());

    // Convert to lowercase and remove extra whitespace
    bool lastWasSpace = true; // Skip leading spaces
    for (const char c : name)
    {
        if (std::isspace(static_cast<unsigned char>(c)) != 0)
        {
            if (!lastWasSpace)
            {
                normalized += ' ';
                lastWasSpace = true;
            }
        }
        else
        {
            normalized += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            lastWasSpace = false;
        }
    }

    // Remove trailing space
    if (!normalized.empty() && normalized.back() == ' ')
    {
        normalized.pop_back();
    }

    return normalized;
}

/// Check if two GPU names refer to the same adapter, handling DXGI vs NVML name
/// differences (case, whitespace, and one being a substring of the other, e.g.
/// "NVIDIA GeForce RTX 4090" vs "GeForce RTX 4090").
[[nodiscard]] inline bool gpuNamesMatch(const std::string& name1, const std::string& name2)
{
    // Normalize first (rather than trying a raw exact-match shortcut before this): two
    // raw-identical strings always normalize equal too, so nothing is lost, and computing the
    // normalized emptiness check up front - before any equality check - closes the case a raw
    // exact-match shortcut would otherwise miss, such as two distinct all-whitespace names
    // ("   " == "   ") that are byte-identical but must not count as a match.
    const std::string norm1 = normalizeGPUName(name1);
    const std::string norm2 = normalizeGPUName(name2);

    // An empty (or all-whitespace) name - e.g. NVML's DeviceGetName failed for this device -
    // must never match anything, including another empty/whitespace-only name: "" == "" and
    // "".contains("") are both true, which would otherwise let a nameless device spuriously
    // match every other GPU below.
    if (norm1.empty() || norm2.empty())
    {
        return false;
    }

    if (norm1 == norm2)
    {
        return true;
    }

    // Check if one contains the other (handles "NVIDIA GeForce..." vs "GeForce...")
    if (norm1.contains(norm2) || norm2.contains(norm1))
    {
        return true;
    }

    return false;
}

} // namespace Platform
