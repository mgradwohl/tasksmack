#pragma once

#include "UI/ChartWidgets.h"

#include <algorithm>
#include <cstdint>

namespace App
{

/// The System Metrics panel's chart data generations, one per source it adopts (#843): the system
/// publication (CPU, memory, network, battery), the storage and GPU publications, and the process
/// model's system-wide histories (power, threads, page faults, handles). Each chart's caches
/// (HistoryChartConfig::dataGeneration, windowSeriesMax(), ReducedPointsCache) are keyed on the
/// generation of the sources it draws, so a storage or GPU publication no longer makes the CPU and
/// memory charts reduce and rescan their histories again: with one generation for every source they
/// did that three or four times per refresh instead of once.
///
/// Every generation comes from UI::Widgets::nextChartDataGeneration(), which only counts up, so the
/// newest of several is a generation for all of them together (combined()): it changes whenever any
/// of them does. 0 is "nothing adopted yet", which the caches never keep.
class ChartSourceGenerations
{
  public:
    void systemAdopted() noexcept
    {
        m_System = UI::Widgets::nextChartDataGeneration();
    }
    void storageAdopted() noexcept
    {
        m_Storage = UI::Widgets::nextChartDataGeneration();
    }
    void gpuAdopted() noexcept
    {
        m_Gpu = UI::Widgets::nextChartDataGeneration();
    }
    void processHistoriesAdopted() noexcept
    {
        m_ProcessHistories = UI::Widgets::nextChartDataGeneration();
    }

    [[nodiscard]] std::uint64_t system() const noexcept
    {
        return m_System;
    }
    [[nodiscard]] std::uint64_t storage() const noexcept
    {
        return m_Storage;
    }
    [[nodiscard]] std::uint64_t gpu() const noexcept
    {
        return m_Gpu;
    }
    [[nodiscard]] std::uint64_t processHistories() const noexcept
    {
        return m_ProcessHistories;
    }

    /// A generation for a chart that draws from both sources (the Overview's Power and Battery): it
    /// changes whenever either of theirs does, since a new generation is larger than every earlier one.
    [[nodiscard]] static constexpr std::uint64_t combined(std::uint64_t first, std::uint64_t second) noexcept
    {
        return std::max(first, second);
    }

  private:
    std::uint64_t m_System = 0;
    std::uint64_t m_Storage = 0;
    std::uint64_t m_Gpu = 0;
    std::uint64_t m_ProcessHistories = 0;
};

} // namespace App
