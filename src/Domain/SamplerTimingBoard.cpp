#include "SamplerTimingBoard.h"

#include "TimingHistogram.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <format>
#include <mutex>
#include <ratio>
#include <string>
#include <string_view>
#include <vector>

namespace Domain
{

namespace
{

[[nodiscard]] double toMilliseconds(std::chrono::microseconds duration) noexcept
{
    return std::chrono::duration<double, std::milli>(duration).count();
}

} // namespace

std::string formatSamplerTimingRow(const SamplerTimingRow& row)
{
    return std::format("{}: {} passes, mean {:.1f} / p50 {:.1f} / p95 {:.1f} / max {:.1f} ms, {} overruns, {} backoffs",
                       row.sampler,
                       row.pass.count(),
                       toMilliseconds(row.pass.mean()),
                       toMilliseconds(row.pass.percentile(0.50)),
                       toMilliseconds(row.pass.percentile(0.95)),
                       toMilliseconds(row.pass.max()),
                       row.overruns,
                       row.backoffs);
}

SamplerTimingBoard& SamplerTimingBoard::global()
{
    static SamplerTimingBoard board;
    return board;
}

void SamplerTimingBoard::publish(std::string_view sampler,
                                 const TimingHistogram& pass,
                                 std::uint64_t overruns,
                                 std::uint64_t backoffs) noexcept
{
    try
    {
        const std::scoped_lock lock(m_Mutex);
        auto row = std::ranges::find(m_Rows, sampler, &SamplerTimingRow::sampler);
        if (row == m_Rows.end())
        {
            row = m_Rows.insert(m_Rows.end(), SamplerTimingRow{.sampler = std::string(sampler)});
        }
        row->pass = pass;
        row->overruns = overruns;
        row->backoffs = backoffs;
    }
    catch (...) // NOLINT(bugprone-empty-catch) - a row that can't be added is shown from a later pass
    {}
}

std::vector<SamplerTimingRow> SamplerTimingBoard::snapshot() const
{
    const std::scoped_lock lock(m_Mutex);
    return m_Rows;
}

} // namespace Domain
