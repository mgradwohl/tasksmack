#pragma once

// The Processes table's multi-selection (#804): which processes are selected, and how a click,
// Ctrl+click, Shift+click or Ctrl+A changes that. ImGui-free and unit-tested
// (test_ProcessSelection.cpp); ProcessesPanel feeds it the clicked row's identity, the modifiers held
// and, for a range, the visible rows in drawn order.
//
// Every process is named by its uniqueKey (a hash of PID and start time), never by its PID alone: a
// PID the system hands to a new process after the selected one exits names a different row, which
// must not inherit the selection, and a batch action must never reach it (#973).
//
// The selection is separate from the panel's "primary" process -- the one last clicked or moved to,
// which Process Details shows and the keyboard moves from. A plain click or a keyboard move makes the
// selection that one process, so with one row selected everything behaves as before multi-selection.

#include "App/Panels/ProcessTableNavigation.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <unordered_set>
#include <utility>

namespace App::ProcessSelection
{

/// What a click on a row does to the selection, from the modifiers held.
enum class ClickKind : std::uint8_t
{
    Replace,  ///< Plain click: select only this row
    Toggle,   ///< Ctrl+click: add or remove this row, leave the others
    Range,    ///< Shift+click: select only the rows from the anchor to this one
    AddRange, ///< Ctrl+Shift+click: add the rows from the anchor to this one
};

/// The click kind for the modifiers held. Alt and Super do not change a click.
[[nodiscard]] constexpr ClickKind clickKindFor(bool ctrl, bool shift) noexcept
{
    if (shift)
    {
        return ctrl ? ClickKind::AddRange : ClickKind::Range;
    }
    return ctrl ? ClickKind::Toggle : ClickKind::Replace;
}

/// The selected processes, by uniqueKey, and the anchor a Shift+click extends from. Lookups are O(1).
class Selection
{
  public:
    [[nodiscard]] bool contains(std::uint64_t key) const noexcept
    {
        return m_Keys.contains(key);
    }

    [[nodiscard]] std::size_t size() const noexcept
    {
        return m_Keys.size();
    }

    /// Whether any of @p visibleKeys is selected: a filter can hide every selected row while they stay
    /// selected, and a batch shortcut must not act on rows the user cannot see (#804 review).
    [[nodiscard]] bool anyVisible(std::span<const std::uint64_t> visibleKeys) const noexcept
    {
        return std::ranges::any_of(visibleKeys, [this](std::uint64_t key) { return m_Keys.contains(key); });
    }

    [[nodiscard]] bool empty() const noexcept
    {
        return m_Keys.empty();
    }

    /// The selected keys, in no particular order.
    [[nodiscard]] const std::unordered_set<std::uint64_t>& keys() const noexcept
    {
        return m_Keys;
    }

    /// The row a Shift+click extends from: the last row clicked without Shift, or moved to with the
    /// keyboard. None before any.
    [[nodiscard]] std::optional<std::uint64_t> anchor() const noexcept
    {
        return m_Anchor;
    }

    /// Select only @p key, and anchor a later Shift+click there (a plain click, a keyboard move).
    void selectOnly(std::uint64_t key)
    {
        m_Keys.clear();
        m_Keys.insert(key);
        m_Anchor = key;
    }

    /// Add @p key if it is not selected, remove it if it is, and anchor there (Ctrl+click). Returns
    /// whether it is selected now.
    bool toggle(std::uint64_t key)
    {
        m_Anchor = key;
        if (m_Keys.erase(key) > 0)
        {
            return false;
        }
        m_Keys.insert(key);
        return true;
    }

    /// Select the rows from the anchor to @p key, inclusive, in @p visibleKeys' order (the rows as
    /// drawn: sorted, filtered, collapsed branches left out). With @p additive (Ctrl+Shift) they are
    /// added to the selection; otherwise they replace it. The anchor does not move, so a second
    /// Shift+click re-draws the range from the same row. When the anchor is unset or not visible, or
    /// @p key is not visible, this selects as a plain or Ctrl click would.
    void selectRange(std::span<const std::uint64_t> visibleKeys, std::uint64_t key, bool additive)
    {
        const std::optional<std::size_t> to = ProcessTableNavigation::indexOfKey(visibleKeys, key);
        const std::optional<std::size_t> from =
            m_Anchor.has_value() ? ProcessTableNavigation::indexOfKey(visibleKeys, *m_Anchor) : std::nullopt;
        if (!to.has_value() || !from.has_value())
        {
            if (additive)
            {
                m_Keys.insert(key);
                m_Anchor = key;
            }
            else
            {
                selectOnly(key);
            }
            return;
        }
        if (!additive)
        {
            m_Keys.clear();
        }
        const auto [first, last] = std::minmax(*from, *to);
        for (std::size_t i = first; i <= last; ++i)
        {
            m_Keys.insert(visibleKeys[i]);
        }
    }

    /// Apply a click on the row @p key of the kind @p kind. @p visibleKeys is read only for a range.
    void click(ClickKind kind, std::uint64_t key, std::span<const std::uint64_t> visibleKeys)
    {
        switch (kind)
        {
        case ClickKind::Replace:
            selectOnly(key);
            return;
        case ClickKind::Toggle:
            (void) toggle(key);
            return;
        case ClickKind::Range:
            selectRange(visibleKeys, key, false);
            return;
        case ClickKind::AddRange:
            selectRange(visibleKeys, key, true);
            return;
        }
    }

    /// Select every visible row (Ctrl+A): @p visibleKeys replace the selection. The anchor is kept
    /// when it is among them, otherwise it moves to the first.
    void selectAll(std::span<const std::uint64_t> visibleKeys)
    {
        m_Keys.clear();
        m_Keys.insert(visibleKeys.begin(), visibleKeys.end());
        if (!m_Anchor.has_value() || !m_Keys.contains(*m_Anchor))
        {
            m_Anchor = visibleKeys.empty() ? std::nullopt : std::optional<std::uint64_t>(visibleKeys.front());
        }
    }

    void clear() noexcept
    {
        m_Keys.clear();
        m_Anchor.reset();
    }

    /// Drop the selected processes that are no longer listed. @p snapshots is any range of objects with
    /// a `uniqueKey` (Domain::ProcessSnapshot): the current generation. One O(1) lookup per listed
    /// process; the set is rebuilt only when something did drop out. The anchor is left alone: a
    /// Shift+click from an anchor that is no longer visible selects as a plain click. Returns whether
    /// anything was dropped.
    template<typename SnapshotRange> bool retainPresent(const SnapshotRange& snapshots)
    {
        if (m_Keys.empty())
        {
            return false;
        }
        std::size_t present = 0;
        for (const auto& snapshot : snapshots)
        {
            if (m_Keys.contains(snapshot.uniqueKey))
            {
                ++present;
            }
        }
        if (present == m_Keys.size())
        {
            return false;
        }
        std::unordered_set<std::uint64_t> kept;
        kept.reserve(present);
        for (const auto& snapshot : snapshots)
        {
            if (m_Keys.contains(snapshot.uniqueKey))
            {
                kept.insert(snapshot.uniqueKey);
            }
        }
        m_Keys = std::move(kept);
        return true;
    }

  private:
    std::unordered_set<std::uint64_t> m_Keys;
    std::optional<std::uint64_t> m_Anchor;
};

} // namespace App::ProcessSelection
