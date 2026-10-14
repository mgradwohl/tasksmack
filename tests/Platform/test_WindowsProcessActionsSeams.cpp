/// @file test_WindowsProcessActionsSeams.cpp
/// @brief WindowsProcessActions' failure paths through faked Win32 calls (#2039), and the real
/// CPU-set reader, which only a machine with several processor groups calls.
///
/// The fakes never hand out a handle that could change anything: OpenProcess returns this test
/// process opened for query only, so a setter that reaches Windows is refused.

#include "Platform/CpuAffinity.h"
#include "Platform/IProcessActions.h"
#include "Platform/Windows/WindowsAffinityMath.h"
#include "Platform/Windows/WindowsProcessActions.h"

#include <gtest/gtest.h>

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// clang-format on

#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace Platform
{
namespace
{

/// What the faked calls do and saw. Function pointers can't capture, so the fakes share this state;
/// each test starts from reset().
struct FailingWin
{
    static inline DWORD openError = 0;  // OpenProcess fails with this, when not 0
    static inline DWORD timesError = 0; // GetProcessTimes fails with this, when not 0
    static inline int setterCalls = 0;

    static void reset()
    {
        openError = 0;
        timesError = 0;
        setterCalls = 0;
    }

    static HANDLE WINAPI openProcess(DWORD /*access*/, BOOL /*inherit*/, DWORD /*pid*/)
    {
        if (openError != 0)
        {
            SetLastError(openError);
            return nullptr;
        }
        // Query-only, whatever was asked for: a setter that gets this far is refused by Windows.
        return OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, GetCurrentProcessId());
    }
    static BOOL WINAPI
    getProcessTimes(HANDLE /*process*/, LPFILETIME creation, LPFILETIME /*exit*/, LPFILETIME /*kernel*/, LPFILETIME /*user*/)
    {
        if (timesError != 0)
        {
            SetLastError(timesError);
            return FALSE;
        }
        creation->dwLowDateTime = TARGET_TICKS;
        creation->dwHighDateTime = 0;
        return TRUE;
    }
    static BOOL WINAPI setProcessAffinityMask(HANDLE /*process*/, DWORD_PTR /*mask*/)
    {
        ++setterCalls;
        return TRUE;
    }
    static BOOL WINAPI setProcessDefaultCpuSets(HANDLE /*process*/, const ULONG* /*ids*/, ULONG /*count*/)
    {
        ++setterCalls;
        return TRUE;
    }

    [[nodiscard]] static WindowsProcessFunctions functions()
    {
        return {
            .openProcess = &openProcess,
            .getProcessTimes = &getProcessTimes,
            .setProcessAffinityMask = &setProcessAffinityMask,
            .setProcessDefaultCpuSets = &setProcessDefaultCpuSets,
            .readGroupSizes = [] { return std::vector<std::uint32_t>{8}; },
            .readCores = [] { return std::vector<ProcessorCoreRecord>{}; },
            .readCpuSets = [] { return std::vector<CpuSetRecord>{}; },
        };
    }

    static constexpr DWORD TARGET_TICKS = 4321;
};

constexpr ProcessTarget TARGET{.pid = 4242, .startTimeTicks = FailingWin::TARGET_TICKS};

class WindowsProcessActionsSeamTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        FailingWin::reset();
    }
};

} // namespace

TEST_F(WindowsProcessActionsSeamTest, AnUnreadableCreationTimeStopsEveryActionBeforeItIsSent)
{
    FailingWin::timesError = ERROR_ACCESS_DENIED;
    WindowsProcessActions actions(FailingWin::functions());
    const std::string expected = "Cannot confirm the identity of process 4242: error 5; action not sent";

    const std::vector<std::pair<const char*, ProcessActionResult>> results{
        {"terminate", actions.terminate(TARGET)},
        {"kill", actions.kill(TARGET)},
        {"setPriority", actions.setPriority(TARGET, 10)},
        {"setAffinity", actions.setAffinity(TARGET, CpuAffinity::fromMask(0b1U))},
    };
    for (const auto& [name, result] : results)
    {
        EXPECT_FALSE(result.success) << name;
        EXPECT_EQ(result.errorMessage, expected) << name;
    }
    EXPECT_EQ(FailingWin::setterCalls, 0);
}

TEST_F(WindowsProcessActionsSeamTest, AnUnexpectedOpenErrorIsReportedWithItsCode)
{
    FailingWin::openError = ERROR_NOT_ENOUGH_MEMORY;
    WindowsProcessActions actions(FailingWin::functions());
    const std::string expected = "Failed to open process 4242: error 8";

    EXPECT_EQ(actions.setPriority(TARGET, 0).errorMessage, expected);
    EXPECT_EQ(actions.setAffinity(TARGET, CpuAffinity::fromMask(0b1U)).errorMessage, expected);
    EXPECT_EQ(actions.kill(TARGET).errorMessage, expected);
    EXPECT_EQ(actions.terminate(TARGET).errorMessage, expected);
    EXPECT_EQ(FailingWin::setterCalls, 0);
}

TEST_F(WindowsProcessActionsSeamTest, APriorityWindowsRefusesIsReportedWithItsError)
{
    // The identity check passes, but the handle is query-only, so SetPriorityClass is refused.
    WindowsProcessActions actions(FailingWin::functions());

    const ProcessActionResult result = actions.setPriority(TARGET, -10);

    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.errorMessage, "Failed to set priority for process 4242: error 5");
}

TEST(WindowsProcessFunctionsTest, TheRealCpuSetReaderListsEveryLogicalProcessorOnce)
{
    const WindowsProcessFunctions functions = realWindowsProcessFunctions();
    ASSERT_NE(functions.readCpuSets, nullptr);
    ASSERT_NE(functions.readGroupSizes, nullptr);

    const std::vector<CpuSetRecord> sets = functions.readCpuSets();
    const std::vector<std::uint32_t> groupSizes = functions.readGroupSizes();
    ASSERT_FALSE(sets.empty());
    ASSERT_FALSE(groupSizes.empty());

    std::set<std::uint32_t> ids;
    std::set<std::pair<std::uint16_t, std::uint8_t>> processors;
    for (const CpuSetRecord& set : sets)
    {
        EXPECT_TRUE(ids.insert(set.id).second) << "CPU set " << set.id << " listed twice";
        EXPECT_TRUE(processors.insert({set.group, set.logicalProcessorIndex}).second)
            << "group " << set.group << " processor " << static_cast<int>(set.logicalProcessorIndex) << " listed twice";
        ASSERT_LT(set.group, groupSizes.size());
        EXPECT_LT(set.logicalProcessorIndex, groupSizes[set.group]);
    }
    EXPECT_EQ(sets.size(), static_cast<std::size_t>(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)));
}

} // namespace Platform
