#pragma once

// Pinned processes (#3061): rows the user keeps at the top of the Processes table, through every sort
// and filter, as TaskSlinger pins them. A pin is a process name, so a process that restarts is pinned
// again, and the pins are saved in config.toml ([process_table] pinned). Pure, so tested without ImGui
// (test_ProcessPins.cpp).

#include "Domain/ProcessSnapshot.h"

#include <algorithm>
#include <cstddef>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace App::ProcessPins
{

/// At most this many pins are kept or loaded: a list is for a few processes, not a second filter.
inline constexpr std::size_t MAX_PINS = 64;

/// A name longer than this isn't pinned or loaded (a hand-edited config can't grow without bound).
inline constexpr std::size_t MAX_NAME_BYTES = 256;

/// The pinned process names.
class Pins
{
  public:
    Pins() = default;

    /// From config.toml's list: empty names, over-long ones, repeats and any past MAX_PINS left out.
    explicit Pins(const std::vector<std::string>& names)
    {
        for (const std::string& name : names)
        {
            static_cast<void>(pin(name));
        }
    }

    [[nodiscard]] bool contains(std::string_view name) const
    {
        return m_Names.contains(name);
    }

    [[nodiscard]] bool empty() const noexcept
    {
        return m_Names.empty();
    }

    /// Pins @p name. False when it already was, or can't be (empty, too long, MAX_PINS reached).
    bool pin(std::string_view name)
    {
        if (name.empty() || name.size() > MAX_NAME_BYTES || m_Names.size() >= MAX_PINS)
        {
            return false;
        }
        return m_Names.emplace(name).second;
    }

    /// Unpins @p name. False when it wasn't pinned.
    bool unpin(std::string_view name)
    {
        const auto found = m_Names.find(name);
        if (found == m_Names.end())
        {
            return false;
        }
        m_Names.erase(found);
        return true;
    }

    /// The names, sorted: config.toml's list.
    [[nodiscard]] std::vector<std::string> names() const
    {
        return {m_Names.begin(), m_Names.end()};
    }

  private:
    std::set<std::string, std::less<>> m_Names;
};

/// Moves the indices of pinned processes in @p order (into @p snapshots) ahead of the rest, each group
/// keeping its order: the table's sort among the pinned, and among the others. Out-of-range indices
/// count as not pinned.
inline void pinnedFirst(std::vector<std::size_t>& order, const std::vector<Domain::ProcessSnapshot>& snapshots, const Pins& pins)
{
    if (pins.empty())
    {
        return;
    }
    std::ranges::stable_partition(
        order, [&snapshots, &pins](std::size_t index) { return index < snapshots.size() && pins.contains(snapshots[index].name); });
}

} // namespace App::ProcessPins
