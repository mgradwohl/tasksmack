#pragma once

// The helpers test_ProcessModel*.cpp share (#3850: the one 5,000-line file split by feature).

#include "Domain/GPUModel.h"
#include "Domain/PriorityConfig.h"
#include "Domain/ProcessModel.h"
#include "Domain/ProcessSnapshot.h"
#include "Domain/SamplingConfig.h"
#include "Mocks/MockGPUProbe.h"
#include "Mocks/MockProbes.h"
#include "Platform/GPUTypes.h"
#include "Platform/IProcessProbe.h"
#include "Platform/ImageSignature.h"
#include "Platform/ProcessTypes.h"
#include "Platform/SchedPolicy.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <future>
#include <latch>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ProcessModelTesting
{

// Deterministic time source for tests that need a known interval between refreshes,
// so rates and power are exact instead of depending on how long a sleep took (#1136).
class ManualClock
{
  public:
    [[nodiscard]] Domain::ProcessModel::NowFunction now()
    {
        return [this]
        {
            return m_Time;
        };
    }

    void advance(Domain::ProcessModel::Clock::duration duration)
    {
        m_Time += duration;
    }

  private:
    Domain::ProcessModel::Clock::time_point m_Time;
};

// Test constants for overflow scenarios
inline constexpr uint64_t OVERFLOW_TEST_MARGIN = 10000; // Distance from max value for overflow tests

/// Helper to create a process counter (legacy compatibility wrapper).
inline Platform::ProcessCounters makeCounter(int32_t pid,
                                             const std::string& name,
                                             char state,
                                             uint64_t userTime,
                                             uint64_t systemTime,
                                             uint64_t startTime = 1000,
                                             uint64_t rssBytes = 1024 * 1024,
                                             int32_t parentPid = 1)
{
    return TestMocks::makeProcessCounters(pid, name, state, userTime, systemTime, startTime, rssBytes, parentPid);
}

/// The model's published aggregated system histories: all empty before the first generation.
inline Domain::ProcessSystemHistories systemHistories(const Domain::ProcessModel& model)
{
    Domain::ProcessSystemHistories histories;
    static_cast<void>(model.tryCopySystemHistoriesIfNewer(0, histories));
    return histories;
}

} // namespace ProcessModelTesting
