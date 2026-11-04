#pragma once

// One published generation of ProcessModel's process list (#3345): the rows, and every index built over them,
// made together and immutable together.
//
// #3334 was an index (the PID -> row table) that a failed refresh left describing rows it never published, so a
// later lookup read the published rows through the wrong table. Here the table is private to the generation that
// owns the rows: it is built when the generation is, off to the side, and every lookup goes through the
// generation, so no index can be used against another generation's rows. A refresh that fails part way never
// publishes its generation, and the published one stays whole, with nothing to roll back.
//
// Immutable once made, so a reader holds it (or an aliasing pointer to its rows) for as long as it likes. Not a
// cache: one per published refresh.

#include "PidIndex.h"
#include "ProcessSnapshot.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace Domain
{

class ProcessGeneration
{
  public:
    /// The empty generation published before the first refresh.
    ProcessGeneration() = default;

    /// Takes @p rows, builds their PID index, and links each row to its children (the process tree): a row's
    /// childrenIndices list the rows whose parentPid is its PID, as long as it started at or before them (a PID
    /// the parent's exit freed and another process took isn't the child's parent). Throws std::bad_alloc, losing
    /// @p rows; the generation published before stays as it was.
    explicit ProcessGeneration(std::vector<ProcessSnapshot> rows) : m_Rows(std::move(rows))
    {
        m_Index.rebuild(m_Rows, &ProcessSnapshot::pid);
        linkChildren();
    }

    [[nodiscard]] const std::vector<ProcessSnapshot>& rows() const noexcept
    {
        return m_Rows;
    }

    /// The row of the process with @p pid that started at @p startTimeTicks, or nullptr. A PID listed twice (one
    /// reused within a single read) maps to its last listing; when that one is the other process, the rows are
    /// walked for this one, as rare as the reuse.
    [[nodiscard]] const ProcessSnapshot* find(std::int32_t pid, std::uint64_t startTimeTicks) const noexcept
    {
        const auto matches = [pid, startTimeTicks](const ProcessSnapshot& row) noexcept
        {
            return row.pid == pid && row.startTimeTicks == startTimeTicks;
        };
        const std::size_t row = m_Index.find(pid);
        if (row >= m_Rows.size())
        {
            return nullptr; // the index maps every PID in these rows: none is this one
        }
        // The PID check is a backstop: the table maps these rows, so its row holds this PID. Two processes started
        // in the same clock tick share a start time, so the start time alone doesn't tell them apart.
        if (matches(m_Rows[row]))
        {
            return &m_Rows[row];
        }
        const auto found = std::ranges::find_if(m_Rows, matches);
        return found != m_Rows.end() ? &*found : nullptr;
    }

  private:
    void linkChildren()
    {
        for (std::size_t i = 0; i < m_Rows.size(); ++i)
        {
            const std::int32_t parentPid = m_Rows[i].parentPid;
            if (parentPid <= 0)
            {
                continue;
            }
            const std::size_t parentRow = m_Index.find(parentPid);
            if (parentRow == PidIndex::NOT_FOUND || parentRow == i)
            {
                continue;
            }
            // Guard against PID reuse: the index maps parentPid to whichever *currently running* process now
            // holds that PID, which may no longer be the process that forked this child if the original parent
            // has since exited and the PID was recycled between the two /proc reads that produced this batch. A
            // real parent must have started at or before its child, so reject candidates that started later.
            // startTimeTicks is 0 when a probe couldn't determine it; skip the check rather than drop a
            // legitimate link when that data isn't available.
            const std::uint64_t candidateParentStart = m_Rows[parentRow].startTimeTicks;
            const std::uint64_t childStart = m_Rows[i].startTimeTicks;
            const bool startTimesKnown = candidateParentStart != 0 && childStart != 0;
            if (!startTimesKnown || candidateParentStart <= childStart)
            {
                m_Rows[parentRow].childrenIndices.push_back(i);
            }
        }
    }

    std::vector<ProcessSnapshot> m_Rows;
    PidIndex m_Index; // private to these rows (#3334): built with them, never rebuilt for others
};

} // namespace Domain
