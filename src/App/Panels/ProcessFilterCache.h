#pragma once

// When the Processes table's filtered (and sorted) indices must be rebuilt, and which processes the
// filter keeps. Pure, so both are unit-testable without an ImGui context (#1394, #838).

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace App::ProcessFilterCache
{

/// True when indices built for snapshot generation `filterVersion` and search `cachedSearchTerm` no
/// longer describe the render cache. `adoptedVersion` must be the generation of the vector actually
/// held -- the version ProcessModel::tryCopySnapshotsIfNewer() returned with it, under the same lock
/// -- never one read from snapshotVersion() before adopting: a generation published between the two
/// would be adopted while the key still matched the old indices, and those indices would then
/// address the new, possibly shorter, vector (#1394).
[[nodiscard]] constexpr bool
isStale(std::uint64_t adoptedVersion, std::uint64_t filterVersion, std::string_view searchTerm, std::string_view cachedSearchTerm) noexcept
{
    return adoptedVersion != filterVersion || searchTerm != cachedSearchTerm;
}

/// `ch` with A-Z folded to a-z. Every other byte is unchanged, so a UTF-8 sequence never folds.
[[nodiscard]] constexpr char foldAscii(char ch) noexcept
{
    return (ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch - 'A' + 'a') : ch;
}

/// Whether `text` contains `term`, ASCII letters compared case-insensitively. An empty term is in
/// every text. Allocates nothing: candidate starts are found by the term's first character, so a long
/// command line costs about one pass.
[[nodiscard]] constexpr bool containsIgnoreCase(std::string_view text, std::string_view term) noexcept
{
    if (term.empty())
    {
        return true;
    }
    if (text.size() < term.size())
    {
        return false;
    }
    const char first = foldAscii(term.front());
    const std::size_t lastStart = text.size() - term.size();
    for (std::size_t start = 0; start <= lastStart; ++start)
    {
        if (foldAscii(text[start]) != first)
        {
            continue;
        }
        std::size_t k = 1;
        while (k < term.size() && foldAscii(text[start + k]) == foldAscii(term[k]))
        {
            ++k;
        }
        if (k == term.size())
        {
            return true;
        }
    }
    return false;
}

/// Whether the Processes filter keeps a process: `term` is in its name or its command line, case
/// aside, as the user guide says (#838).
[[nodiscard]] constexpr bool matches(std::string_view name, std::string_view command, std::string_view term) noexcept
{
    return containsIgnoreCase(name, term) || containsIgnoreCase(command, term);
}

} // namespace App::ProcessFilterCache
