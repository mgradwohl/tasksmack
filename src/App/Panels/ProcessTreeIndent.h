#pragma once

// Pure tree-indent sizing extracted from ProcessesPanel::renderProcessRow() (#906), so the rule
// that keeps the expand/collapse control reachable can be unit-tested directly instead of only
// indirectly through a live ImGui context. See CONTRIBUTING.md's "extract the pure decision logic
// into a small header" pattern (also used by ProcessTreeFlatten.h, AdaptiveIntervalUtils.h,
// TitleBarGeometry.h). This computes widths only -- it makes no ImGui calls -- so there is no
// live-context blocker to testing it.

#include <algorithm>

namespace App::ProcessTreeIndent
{

/// Indent per tree level, in ems: 16px at the reference em (32/3 px), the fixed pixel value it
/// replaces. As pixels it did not follow the font, so at Extra Large on a 175% display a child was
/// indented by less than half a character and was hard to tell from a root (#971).
inline constexpr float INDENT_PER_LEVEL_EM = 1.5F;

/// Name text kept visible past the expander however deep the row, in ems: 72px at the reference em.
/// The point of the reservation is "a recognisable chunk of the process name" (#906, #913); as a
/// fixed 72px that was about thirteen characters at the reference font and about three at Even
/// Huger on a scaled display, which defeated it.
inline constexpr float MIN_NAME_WIDTH_EM = 6.75F;

/// Indent to apply to a tree row's Name cell, clamped so the expander and a minimum slice of the
/// name always stay inside the cell.
///
/// The Name column is fixed-width, and ProcessesPanel widens it by the deepest indent in the tree
/// (see m_MaxTreeDepth) so text room at every depth matches a flat list's. That widening is not
/// guaranteed, though: ImGui persists a user-dragged column width in its ini and then ignores the
/// width passed to TableSetupColumn(). Left unclamped, a user who narrows this column would push
/// the expand/collapse button out of its cell and be unable to toggle deep parents at all -- so the
/// indent yields to the controls rather than the other way round.
///
/// @param depth          Tree depth of the row; 0 is a root.
/// @param indentPerLevel Pixels of indent per level.
/// @param cellWidth      Width actually available in the Name cell this frame.
/// @param reservedWidth  Pixels to keep for the expander plus a minimum readable slice of the name.
/// @return Indent in pixels, never negative and never large enough to consume `reservedWidth`.
[[nodiscard]] constexpr float clampedIndent(int depth, float indentPerLevel, float cellWidth, float reservedWidth) noexcept
{
    if (depth <= 0 || indentPerLevel <= 0.0F)
    {
        return 0.0F;
    }
    const float requested = indentPerLevel * static_cast<float>(depth);
    // A cell narrower than the reserved space leaves nothing to give: fall back to no indent, so
    // the expander keeps whatever room there is rather than being pushed out entirely.
    const float allowed = std::max(0.0F, cellWidth - reservedWidth);
    return std::min(requested, allowed);
}

} // namespace App::ProcessTreeIndent
