/// @file test_ProcessState.cpp
/// @brief Tests for Domain::processStateCode() / processStateName() (#1352)
///
/// The Processes table's State column prints processStateCode(displayState). It used to print the
/// name's first letter, so Stopped read S and Dead read D. These tests run every raw state code a
/// probe reports (Linux /proc/[pid]/stat, and Windows' derived R/S/T/I/? since #1302) through
/// ProcessModel and check the published name maps back to the kernel's code.

#include "Domain/ProcessModel.h"
#include "Domain/ProcessState.h"
#include "Mocks/MockProbes.h"

#include <gtest/gtest.h>

#include <array>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace
{

struct StateCase
{
    char rawState;         // what the probe reports
    std::string_view name; // the ProcessSnapshot::displayState ProcessModel publishes
    char code;             // what the State column prints
};

constexpr std::array<StateCase, 11> STATE_CASES{{
    {.rawState = 'R', .name = "Running", .code = 'R'},
    {.rawState = 'S', .name = "Sleeping", .code = 'S'},
    {.rawState = 'D', .name = "Disk Sleep", .code = 'D'},
    {.rawState = 'Z', .name = "Zombie", .code = 'Z'},
    {.rawState = 'T', .name = "Stopped", .code = 'T'},
    {.rawState = 't', .name = "Tracing", .code = 't'},
    {.rawState = 'X', .name = "Dead", .code = 'X'},
    {.rawState = 'I', .name = "Idle", .code = 'I'},
    // Windows reports '?' for a process it can't classify; Linux codes TaskSmack doesn't name
    // (e.g. 'W' paging, 'P' parked) also publish as Unknown.
    {.rawState = '?', .name = "Unknown", .code = '?'},
    {.rawState = 'W', .name = "Unknown", .code = '?'},
    {.rawState = 'P', .name = "Unknown", .code = '?'},
}};

TEST(ProcessStateTest, EveryPublishedStateMapsToItsKernelCode)
{
    for (const auto& c : STATE_CASES)
    {
        SCOPED_TRACE(std::string(1, c.rawState));

        auto probe = std::make_unique<TestMocks::MockProcessProbe>();
        probe->setCounters({TestMocks::makeProcessCounters(1, "test", c.rawState)});
        probe->setTotalCpuTime(100000);
        Domain::ProcessModel model(std::move(probe));
        model.refresh();

        const auto snaps = model.snapshots();
        ASSERT_EQ(snaps.size(), 1U);
        EXPECT_EQ(snaps[0].displayState, c.name);
        EXPECT_EQ(Domain::processStateCode(snaps[0].displayState), c.code);
        EXPECT_EQ(Domain::processStateName(c.rawState), c.name);
    }
}

TEST(ProcessStateTest, StoppedAndDeadNoLongerReadAsTheirFirstLetter)
{
    // The #1352 cases: the name's first letter is a different state's code.
    EXPECT_EQ(Domain::processStateCode("Stopped"), 'T');
    EXPECT_EQ(Domain::processStateCode("Dead"), 'X');
    EXPECT_EQ(Domain::processStateCode("Unknown"), '?');
}

TEST(ProcessStateTest, UnrecognisedOrEmptyNameIsUnknownCode)
{
    EXPECT_EQ(Domain::processStateCode(""), '?');
    EXPECT_EQ(Domain::processStateCode("running"), '?'); // names are exact, not case-folded
    EXPECT_EQ(Domain::processStateCode("Suspended"), '?');
}

TEST(ProcessStateTest, CodeAndNameTablesRoundTrip)
{
    for (const auto& entry : Domain::PROCESS_STATE_NAMES)
    {
        EXPECT_EQ(Domain::processStateCode(Domain::processStateName(entry.code)), entry.code);
    }
    static_assert(Domain::processStateCode("Stopped") == 'T');
    static_assert(Domain::processStateName('X') == "Dead");
}

} // namespace
