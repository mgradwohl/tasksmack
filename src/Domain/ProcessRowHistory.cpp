#include "ProcessRowHistory.h"

#include "ProcessSnapshot.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace Domain::ProcessRowHistory
{

Generation::Generation(MetricSet metrics, std::size_t rows) : m_Metrics(metrics), m_Rows(rows)
{
    m_SlotOf.fill(NO_SLOT);
    std::size_t metric = 0;
    while (metric < METRIC_COUNT)
    {
        if (contains(metrics, static_cast<Metric>(metric)))
        {
            m_SlotOf[metric] = static_cast<std::uint8_t>(m_SlotsPerRow);
            ++m_SlotsPerRow;
        }
        ++metric;
    }
    m_Rings.resize(m_Rows * m_SlotsPerRow);
}

const Ring* Generation::ring(std::size_t row, Metric metric) const noexcept
{
    const auto metricIndex = static_cast<std::size_t>(metric);
    if (row >= m_Rows || metricIndex >= METRIC_COUNT || m_SlotOf[metricIndex] == NO_SLOT)
    {
        return nullptr;
    }
    return &m_Rings[(row * m_SlotsPerRow) + m_SlotOf[metricIndex]];
}

std::shared_ptr<const Generation> Tracker::advance(const std::vector<ProcessSnapshot>& snapshots, MetricSet metrics)
{
    if (metrics == 0)
    {
        // Nothing is shown: drop everything, the index's capacity included.
        m_Last.reset();
        std::vector<KeyRow>().swap(m_LastKeys);
        std::vector<KeyRow>().swap(m_NextKeys);
        return nullptr;
    }

    auto next = std::make_shared<Generation>(metrics, snapshots.size());

    // This generation's index: its rows sorted by (pid, start time), then row, so an identity listed
    // twice finds its first row. A sorted vector reused across generations rather than a hash map
    // rebuilt for each, so carrying the rings over allocates nothing but the generation.
    m_NextKeys.clear();
    m_NextKeys.reserve(snapshots.size());
    std::size_t row = 0;
    while (row < snapshots.size())
    {
        m_NextKeys.push_back({.pid = snapshots[row].pid, .startTimeTicks = snapshots[row].startTimeTicks, .row = row});
        ++row;
    }
    std::ranges::sort(m_NextKeys);

    row = 0;
    while (row < snapshots.size())
    {
        const ProcessSnapshot& proc = snapshots[row];
        // The process's row in the last generation, if it listed it (an identity listed twice: its first row).
        const Generation* previous = nullptr;
        std::size_t previousRow = 0;
        if (m_Last)
        {
            const KeyRow wanted{.pid = proc.pid, .startTimeTicks = proc.startTimeTicks, .row = 0};
            const auto it = std::ranges::lower_bound(m_LastKeys, wanted);
            if (it != m_LastKeys.end() && it->pid == proc.pid && it->startTimeTicks == proc.startTimeTicks)
            {
                previous = m_Last.get();
                previousRow = it->row;
            }
        }
        std::size_t metric = 0;
        while (metric < METRIC_COUNT)
        {
            const std::uint8_t slot = next->m_SlotOf[metric];
            if (slot != Generation::NO_SLOT)
            {
                Ring& ring = next->ringAt(row, slot);
                if (previous != nullptr)
                {
                    if (const Ring* kept = previous->ring(previousRow, static_cast<Metric>(metric)); kept != nullptr)
                    {
                        ring = *kept;
                    }
                }
                ring.push(static_cast<float>(sampleOf(proc, static_cast<Metric>(metric))));
            }
            ++metric;
        }
        ++row;
    }

    m_LastKeys.swap(m_NextKeys);
    m_Last = next;
    return next;
}

std::size_t Tracker::bytes() const noexcept
{
    if (!m_Last)
    {
        return 0;
    }
    return sizeof(Generation) + m_Last->ringBytes() + ((m_LastKeys.capacity() + m_NextKeys.capacity()) * sizeof(KeyRow));
}

} // namespace Domain::ProcessRowHistory
