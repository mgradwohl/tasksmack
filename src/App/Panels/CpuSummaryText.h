#pragma once

// The CPU header's logical-processor summary, " (N logical processors @ X.XX GHz)", which the CPU
// Cores tab prints after the CPU model (#1180; the Overview's CPU Details block lists the same facts
// as rows, #809). Pure: no ImGui calls.

#include "Domain/Numeric.h"
#include "UI/Format.h"

#include <cstdint>
#include <string>

namespace App::Detail
{

/// The summary suffix for `coreCount` logical processors at `cpuFreqMHz`; an unknown clock (0) is
/// left out rather than shown as "0.00 GHz". The count is of logical processors, not cores (#1203).
[[nodiscard]] inline std::string cpuCoreSummary(int coreCount, std::uint64_t cpuFreqMHz)
{
    return UI::Format::formatLogicalProcessorSummary(coreCount, (cpuFreqMHz > 0) ? Domain::Numeric::toDouble(cpuFreqMHz) : 0.0);
}

/// cpuCoreSummary(), formatted when its inputs change rather than every frame (#1171).
class CpuCoreSummaryCache
{
  public:
    /// The summary for these inputs, rebuilt only when either differs from the last call's. The text
    /// is built before the keys are committed: a render exception is caught and the app carries on,
    /// so a rebuild that throws must be retried on the next call rather than leave stale text keyed
    /// as current.
    [[nodiscard]] const std::string& get(int coreCount, std::uint64_t cpuFreqMHz)
    {
        if (coreCount != m_CoreCount || cpuFreqMHz != m_FreqMHz)
        {
            m_Text = cpuCoreSummary(coreCount, cpuFreqMHz);
            m_CoreCount = coreCount;
            m_FreqMHz = cpuFreqMHz;
        }
        return m_Text;
    }

  private:
    std::string m_Text;
    int m_CoreCount = -1;
    std::uint64_t m_FreqMHz = 0;
};

} // namespace App::Detail
