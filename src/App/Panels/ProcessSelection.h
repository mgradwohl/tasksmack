#pragma once

// The Processes table's multi-selection (#804): which processes are selected, and how a click,
// Ctrl+click, Shift+click or Ctrl+A changes that. ImGui-free and unit-tested
// (test_ProcessSelection.cpp); ProcessesPanel feeds it the clicked row's identity, the modifiers held
// and, for a range, the visible rows in drawn order.
//
// Every process is named by its full identity -- PID and start time, a Platform::ProcessTarget --
// never by its PID alone, and never by the uniqueKey hash of the two (#1503). A PID the system hands
// to a new process after the selected one exits names a different row, which must not inherit the
// selection, and a batch action must never reach it (#973). A hash can collide: two processes, alive
// together or one after the other, would then share a key, and a selection keyed on it could move to,
// or act on, a process the user never picked. The exact identity cannot.
//
// The selection is separate from the panel's "primary" process -- the one last clicked or moved to,
// which Process Details shows and the keyboard moves from. A plain click or a keyboard move makes the
// selection that one process, so with one row selected everything behaves as before multi-selection.

#include "App/Panels/ProcessTableNavigation.h"
#include "Platform/IProcessActions.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <unordered_set>
#include <utility>

namespace App::ProcessSelection
{

/// A process's exact identity: PID and start time (#1503), the way a process action names its target.
using Identity = Platform::ProcessTarget;

/// The identity of @p snapshot: any object with a `pid` and a `startTimeTicks` (Domain::ProcessSnapshot).
template<typename Snapshot> [[nodiscard]] constexpr Identity identityOf(const Snapshot& snapshot) noexcept
{
    return Identity{.pid = snapshot.pid, .startTimeTicks = snapshot.startTimeTicks};
}

/// Hashes an Identity for the selection's set. Only a bucket choice: membership is decided by
/// Identity's operator==, which compares PID and start time exactly.
struct IdentityHash
{
    [[nodiscard]] std::size_t operator()(const Identity& id) const noexcept
    {
        // The PID's bits spread over the word (the 64-bit golden-ratio constant), then mixed with the
        // start time: PIDs are small and dense, start times large and close together.
        constexpr std::uint64_t SPREAD = 0x9E3779B97F4A7C15ULL;
        const auto pidBits = static_cast<std::uint64_t>(static_cast<std::uint32_t>(id.pid));
        return std::hash<std::uint64_t>{}(id.startTimeTicks ^ (pidBits * SPREAD));
    }
};

/// A set of process identities, with O(1) average lookups.
using IdentitySet = std::unordered_set<Identity, IdentityHash>;

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

/// The selected processes, by exact identity, and the anchor a Shift+click extends from. Lookups are
/// O(1) on average.
class Selection
{
  public:
    [[nodiscard]] bool contains(const Identity& id) const noexcept
    {
        return m_Selected.contains(id);
    }

    [[nodiscard]] std::size_t size() const noexcept
    {
        return m_Selected.size();
    }

    /// Whether any of @p visible is selected: a filter can hide every selected row while they stay
    /// selected, and a batch shortcut must not act on rows the user cannot see (#804 review).
    [[nodiscard]] bool anyVisible(std::span<const Identity> visible) const noexcept
    {
        return std::ranges::any_of(visible, [this](const Identity& id) { return m_Selected.contains(id); });
    }

    [[nodiscard]] bool empty() const noexcept
    {
        return m_Selected.empty();
    }

    /// The selected processes, in no particular order.
    [[nodiscard]] const IdentitySet& selected() const noexcept
    {
        return m_Selected;
    }

    /// The row a Shift+click extends from: the last row clicked without Shift, or moved to with the
    /// keyboard. None before any.
    [[nodiscard]] std::optional<Identity> anchor() const noexcept
    {
        return m_Anchor;
    }

    /// Select only @p id, and anchor a later Shift+click there (a plain click, a keyboard move).
    void selectOnly(const Identity& id)
    {
        m_Selected.clear();
        m_Selected.insert(id);
        m_Anchor = id;
    }

    /// Add @p id if it is not selected, remove it if it is, and anchor there (Ctrl+click). Returns
    /// whether it is selected now.
    bool toggle(const Identity& id)
    {
        m_Anchor = id;
        if (m_Selected.erase(id) > 0)
        {
            return false;
        }
        m_Selected.insert(id);
        return true;
    }

    /// Select the rows from the anchor to @p id, inclusive, in @p visible's order (the rows as drawn:
    /// sorted, filtered, collapsed branches left out). With @p additive (Ctrl+Shift) they are added to
    /// the selection; otherwise they replace it. The anchor does not move, so a second Shift+click
    /// re-draws the range from the same row. When the anchor is unset or not visible, or @p id is not
    /// visible, this selects as a plain or Ctrl click would.
    void selectRange(std::span<const Identity> visible, const Identity& id, bool additive)
    {
        const std::optional<std::size_t> to = ProcessTableNavigation::indexOfKey(visible, id);
        const std::optional<std::size_t> from =
            m_Anchor.has_value() ? ProcessTableNavigation::indexOfKey(visible, *m_Anchor) : std::nullopt;
        if (!to.has_value() || !from.has_value())
        {
            if (additive)
            {
                m_Selected.insert(id);
                m_Anchor = id;
            }
            else
            {
                selectOnly(id);
            }
            return;
        }
        if (!additive)
        {
            m_Selected.clear();
        }
        const auto [first, last] = std::minmax(*from, *to);
        for (std::size_t i = first; i <= last; ++i)
        {
            m_Selected.insert(visible[i]);
        }
    }

    /// Apply a click on the row @p id of the kind @p kind. @p visible is read only for a range.
    void click(ClickKind kind, const Identity& id, std::span<const Identity> visible)
    {
        switch (kind)
        {
        case ClickKind::Replace:
            selectOnly(id);
            return;
        case ClickKind::Toggle:
            (void) toggle(id);
            return;
        case ClickKind::Range:
            selectRange(visible, id, false);
            return;
        case ClickKind::AddRange:
            selectRange(visible, id, true);
            return;
        }
    }

    /// Select every visible row (Ctrl+A): @p visible replace the selection. The anchor is kept when it
    /// is among them, otherwise it moves to the first.
    void selectAll(std::span<const Identity> visible)
    {
        m_Selected.clear();
        m_Selected.insert(visible.begin(), visible.end());
        if (!m_Anchor.has_value() || !m_Selected.contains(*m_Anchor))
        {
            m_Anchor = visible.empty() ? std::nullopt : std::optional<Identity>(visible.front());
        }
    }

    void clear() noexcept
    {
        m_Selected.clear();
        m_Anchor.reset();
    }

    /// Drop the selected processes that are no longer listed. @p snapshots is any range of objects with
    /// a `pid` and a `startTimeTicks` (Domain::ProcessSnapshot): the current generation. One O(1)
    /// lookup per listed process. A process listed with a selected one's PID but another start time is
    /// a different process, so the selected one has exited and is dropped. The anchor is left alone: a
    /// Shift+click from an anchor that is no longer visible selects as a plain click. Returns whether
    /// anything was dropped.
    template<typename SnapshotRange> bool retainPresent(const SnapshotRange& snapshots)
    {
        if (m_Selected.empty())
        {
            return false;
        }
        // Runs once per new snapshot generation and allocates only while something is selected.
        IdentitySet kept;
        kept.reserve(m_Selected.size());
        for (const auto& snapshot : snapshots)
        {
            if (const Identity id = identityOf(snapshot); m_Selected.contains(id))
            {
                kept.insert(id);
            }
        }
        if (kept.size() == m_Selected.size())
        {
            return false;
        }
        m_Selected = std::move(kept);
        return true;
    }

  private:
    IdentitySet m_Selected;
    std::optional<Identity> m_Anchor;
};

} // namespace App::ProcessSelection
