// F7 and F8 on the Processes table (#3097): one priority step up or down, nice on Linux, classes on Windows.

#include "App/Panels/NiceStep.h"
#include "Domain/PriorityConfig.h"
#include "Platform/IProcessActions.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>

namespace App
{
namespace
{

using Domain::Priority::PriorityClass;
using NiceStep::Direction;

TEST(NiceStepTest, OneNiceValueAtATimeOnLinuxStoppingAtTheEnds)
{
    EXPECT_EQ(NiceStep::next(0, PriorityClass::None, Direction::Raise, false), std::optional<std::int32_t>{-1});
    EXPECT_EQ(NiceStep::next(0, PriorityClass::None, Direction::Lower, false), std::optional<std::int32_t>{1});
    EXPECT_EQ(NiceStep::next(-20, PriorityClass::None, Direction::Raise, false), std::nullopt);
    EXPECT_EQ(NiceStep::next(19, PriorityClass::None, Direction::Lower, false), std::nullopt);
}

TEST(NiceStepTest, OneClassAtATimeOnWindowsNeverUpToRealtime)
{
    namespace P = Domain::Priority;
    EXPECT_EQ(NiceStep::next(0, PriorityClass::Normal, Direction::Raise, true),
              std::optional<std::int32_t>{P::classNice(PriorityClass::AboveNormal)});
    EXPECT_EQ(NiceStep::next(0, PriorityClass::Normal, Direction::Lower, true),
              std::optional<std::int32_t>{P::classNice(PriorityClass::BelowNormal)});
    EXPECT_EQ(NiceStep::next(-15, PriorityClass::High, Direction::Raise, true), std::nullopt); // not to Realtime
    EXPECT_EQ(NiceStep::next(19, PriorityClass::Idle, Direction::Lower, true), std::nullopt);
    EXPECT_EQ(NiceStep::next(-20, PriorityClass::Realtime, Direction::Lower, true),
              std::optional<std::int32_t>{P::classNice(PriorityClass::High)});
    EXPECT_EQ(NiceStep::next(-20, PriorityClass::Realtime, Direction::Raise, true), std::nullopt);
    // A class that wasn't read: the one its nice value's label names.
    EXPECT_EQ(NiceStep::next(10, PriorityClass::None, Direction::Raise, true),
              std::optional<std::int32_t>{P::classNice(PriorityClass::Normal)});
}

TEST(NiceStepTest, ResultLineNamesTheProcessAndThePriority)
{
    Platform::ProcessActionResult ok;
    ok.success = true;
    const Detail::ActionResultMessage message = NiceStep::resultMessage("foo", 42, 10, ok, false);
    EXPECT_TRUE(message.ok);
    EXPECT_TRUE(message.text.starts_with("Priority of foo (PID 42) set to ")) << message.text;
    EXPECT_TRUE(message.text.contains("10")) << message.text;
    Platform::ProcessActionResult refused;
    refused.errorMessage = "Permission denied";
    EXPECT_EQ(NiceStep::resultMessage("foo", 42, -1, refused, false).text, "Permission denied");
}

} // namespace
} // namespace App
