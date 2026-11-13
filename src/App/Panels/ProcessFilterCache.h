#pragma once

// When the Processes table's filtered (and sorted) indices must be rebuilt, and which processes the
// filter keeps. Pure, so both are unit-testable without an ImGui context (#1394, #838).

#include "Domain/AccountName.h"
#include "Platform/AsciiCase.h"

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

/// Whether `term` is in a process's name or its command line, ASCII case aside, as the user guide says
/// (#838). Platform::containsIgnoreCase() allocates nothing and scans by the term's first character, so a
/// long command line costs about one pass per row.
[[nodiscard]] constexpr bool matches(std::string_view name, std::string_view command, std::string_view term) noexcept
{
    return Platform::containsIgnoreCase(name, term) || Platform::containsIgnoreCase(command, term);
}

/// As above, or `term` is in the owner's account (#3328): its short name, its domain, or "PC\\alice" across
/// the two (Domain::AccountName::matches()), so the filter finds a user by either name the User column writes.
[[nodiscard]] constexpr bool
matches(std::string_view name, std::string_view command, std::string_view userDomain, std::string_view user, std::string_view term) noexcept
{
    return matches(name, command, term) || Domain::AccountName::matches(userDomain, user, term);
}

} // namespace App::ProcessFilterCache
