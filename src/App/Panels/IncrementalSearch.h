#pragma once

// The Processes table's incremental search (#3097), htop's "/": the row a typed name jumps to. Pure, so
// tested without ImGui (test_IncrementalSearch.cpp).

#include "Platform/AsciiCase.h"

#include <cstddef>
#include <optional>
#include <span>
#include <string_view>

namespace App::IncrementalSearch
{

/// The first of @p names (the visible rows' names, in drawn order) containing @p query, ignoring ASCII
/// case, searching from @p from (the selected row; the top without one) and wrapping round to it. With
/// @p next, @p from itself is looked at last, so the search moves on to the following match. nullopt with
/// no match or an empty query.
[[nodiscard]] constexpr std::optional<std::size_t>
findMatch(std::span<const std::string_view> names, std::optional<std::size_t> from, std::string_view query, bool next) noexcept
{
    if (query.empty() || names.empty())
    {
        return std::nullopt;
    }
    const std::size_t start = (from.has_value() && *from < names.size()) ? *from : 0;
    const std::size_t first = (next && from.has_value()) ? 1 : 0;
    for (std::size_t step = first; step < names.size() + first; ++step)
    {
        const std::size_t row = (start + step) % names.size();
        if (Platform::containsIgnoreCase(names[row], query))
        {
            return row;
        }
    }
    return std::nullopt;
}

} // namespace App::IncrementalSearch
