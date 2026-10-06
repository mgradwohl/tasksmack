#pragma once

// When the Processes table's filtered (and sorted) indices must be rebuilt. Pure, so the keying is
// unit-testable without an ImGui context (#1394).

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

} // namespace App::ProcessFilterCache
