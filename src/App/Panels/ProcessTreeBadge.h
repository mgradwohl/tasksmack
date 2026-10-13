#pragma once

// The child-count badge tree view draws after a collapsed parent's name (#838), "svchost.exe (12)",
// and its tooltip. Pure and ImGui-free, so the wording is unit-tested directly (CONTRIBUTING.md's
// "extract the pure decision logic into a small header" pattern).
//
// It counts the parent's direct children, not all its descendants. That is the number of rows one
// click on the caret adds, so the badge says what expanding will show. It is the size of the
// parent's group of children, which the tree's build already has (ProcessTreeRow::childCount), so it
// costs nothing per frame or per rebuild. While a filter is on it counts the children shown --
// matches and their ancestors -- as the rows do. A descendant total would need its own pass over the
// tree and would lump a deep subtree (a service host's children's children) into one number.

#include <array>
#include <charconv>
#include <cstddef>
#include <format>
#include <string>
#include <string_view>

namespace App::ProcessTreeBadge
{

/// Room for "(" + a std::size_t + ")".
using BadgeBuffer = std::array<char, 24>;

/// The badge for @p childCount children, "(n)", written into @p buffer: drawn every frame for each
/// collapsed parent in view, so it is not allocated.
[[nodiscard]] inline std::string_view badgeText(std::size_t childCount, BadgeBuffer& buffer) noexcept
{
    buffer[0] = '(';
    const auto result = std::to_chars(buffer.data() + 1, buffer.data() + buffer.size() - 1, childCount);
    *result.ptr = ')';
    return {buffer.data(), static_cast<std::size_t>(result.ptr + 1 - buffer.data())};
}

/// The badge's tooltip: what it counts, and, while a filter is on, that it counts only what is shown.
[[nodiscard]] inline std::string badgeTooltip(std::size_t childCount, bool filtered)
{
    const bool one = childCount == 1;
    std::string text = std::format(
        "{} direct child process{}, not counting their own children; expand to show {}", childCount, one ? "" : "es", one ? "it" : "them");
    if (filtered)
    {
        text += "\nWhile filtering, only the children that match or lead to a match are counted";
    }
    return text;
}

} // namespace App::ProcessTreeBadge
