#pragma once

// Each BackgroundSampler's pass timing, as it last published it (#3153), so a debug surface (the Render
// Metrics overlay) can show every sampler's cost side by side without calling a sampler. Each sampler
// publishes from its own thread after every pass: a short lock and a copy of a fixed-size histogram,
// allocating only for a sampler's first row. Readers take a copy (snapshot()).

#include "TimingHistogram.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace Domain
{

/// One sampler's whole-pass timing since it last started.
struct SamplerTimingRow
{
    std::string sampler; ///< Its thread name (SamplerConfig::threadName), e.g. "ts-sampler-disk"
    // NOLINTNEXTLINE(readability-redundant-member-init) - {} lets designated initializers omit it without a warning
    TimingHistogram pass{};     ///< Every pass's duration
    std::uint64_t overruns = 0; ///< Passes that took longer than the sampling interval
    std::uint64_t backoffs = 0; ///< Passes after which the next waited longer (nextSampleTime())
};

/// The row as one line for a debug surface: passes, mean/p50/p95/max in ms, overruns and backoffs.
[[nodiscard]] std::string formatSamplerTimingRow(const SamplerTimingRow& row);

class SamplerTimingBoard
{
  public:
    /// The board every BackgroundSampler publishes to.
    [[nodiscard]] static SamplerTimingBoard& global();

    /// Replace @p sampler's row (adding it the first time). Never throws: a row that can't be added
    /// (std::bad_alloc) is simply missing until a later pass adds it, and sampling is unaffected.
    void publish(std::string_view sampler, const TimingHistogram& pass, std::uint64_t overruns, std::uint64_t backoffs) noexcept;

    /// A copy of every row, in the order the samplers first published.
    [[nodiscard]] std::vector<SamplerTimingRow> snapshot() const;

  private:
    mutable std::mutex m_Mutex;
    std::vector<SamplerTimingRow> m_Rows; // guarded by m_Mutex
};

} // namespace Domain
