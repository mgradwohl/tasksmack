/// @file test_LinuxIoPriority.cpp
/// @brief The Linux I/O priority's pure pieces (#803): the ioprio value encoding and decoding
/// (IoPriority.h) and the error messages for ioprio_set(2)'s errnos (PriorityErrorMessage.h). The real
/// syscalls are exercised against a child process in test_LinuxProcessActions.cpp.

#include "Platform/IProcessActions.h"
#include "Platform/Linux/IoPriority.h"
#include "Platform/Linux/PriorityErrorMessage.h"

#include <gtest/gtest.h>

#include <array>
#include <cerrno>
#include <optional>
#include <string>

namespace Platform
{
namespace
{

// --- Encoding ------------------------------------------------------------------------------------

TEST(LinuxIoPriorityTest, EncodeMatchesTheKernelLayout)
{
    // IOPRIO_PRIO_VALUE(class, level) == (class << 13) | level, with the kernel's class numbers.
    struct Case
    {
        IoPriority priority;
        int expected = 0;
    };
    constexpr std::array<Case, 8> CASES{{
        {.priority = {.ioClass = IoPriorityClass::None, .level = 0}, .expected = 0},
        {.priority = {.ioClass = IoPriorityClass::Realtime, .level = 0}, .expected = (1 << 13)},
        {.priority = {.ioClass = IoPriorityClass::Realtime, .level = 7}, .expected = (1 << 13) | 7},
        {.priority = {.ioClass = IoPriorityClass::BestEffort, .level = 0}, .expected = (2 << 13)},
        {.priority = {.ioClass = IoPriorityClass::BestEffort, .level = 4}, .expected = (2 << 13) | 4},
        {.priority = {.ioClass = IoPriorityClass::BestEffort, .level = 7}, .expected = (2 << 13) | 7},
        {.priority = {.ioClass = IoPriorityClass::Idle, .level = 0}, .expected = (3 << 13)},
        // The kernel refuses a level with None and ignores one with Idle: neither is sent.
        {.priority = {.ioClass = IoPriorityClass::Idle, .level = 5}, .expected = (3 << 13)},
    }};
    for (const Case& testCase : CASES)
    {
        SCOPED_TRACE(testCase.expected);
        EXPECT_EQ(IoPrio::encode(testCase.priority), testCase.expected);
    }
    EXPECT_EQ(IoPrio::encode({.ioClass = IoPriorityClass::None, .level = 3}), 0);
}

TEST(LinuxIoPriorityTest, EncodeHoldsTheLevelToTheRange)
{
    EXPECT_EQ(IoPrio::encode({.ioClass = IoPriorityClass::BestEffort, .level = -3}), (2 << 13));
    EXPECT_EQ(IoPrio::encode({.ioClass = IoPriorityClass::BestEffort, .level = 99}), (2 << 13) | 7);
    EXPECT_EQ(IoPrio::encode({.ioClass = IoPriorityClass::Realtime, .level = 8}), (1 << 13) | 7);
}

// --- Decoding ------------------------------------------------------------------------------------

TEST(LinuxIoPriorityTest, DecodeReadsClassAndLevel)
{
    struct Case
    {
        int value = 0;
        IoPriority expected;
    };
    constexpr std::array<Case, 6> CASES{{
        {.value = 0, .expected = {.ioClass = IoPriorityClass::None, .level = 0}},
        {.value = (1 << 13) | 2, .expected = {.ioClass = IoPriorityClass::Realtime, .level = 2}},
        {.value = (2 << 13) | 4, .expected = {.ioClass = IoPriorityClass::BestEffort, .level = 4}},
        {.value = (2 << 13) | 7, .expected = {.ioClass = IoPriorityClass::BestEffort, .level = 7}},
        {.value = (3 << 13), .expected = {.ioClass = IoPriorityClass::Idle, .level = 0}},
        // A never-set class may report a level of its own (older kernels report the nice-derived one):
        // it means nothing there, so it reads as 0.
        {.value = 4, .expected = {.ioClass = IoPriorityClass::None, .level = 0}},
    }};
    for (const Case& testCase : CASES)
    {
        SCOPED_TRACE(testCase.value);
        EXPECT_EQ(IoPrio::decode(testCase.value), std::optional<IoPriority>{testCase.expected});
    }
}

TEST(LinuxIoPriorityTest, DecodeIgnoresHintBits)
{
    // Linux 6.5 puts hints in bits 3-12; the level is bits 0-2 alone.
    EXPECT_EQ(IoPrio::decode((2 << 13) | (1 << 3) | 5), (std::optional<IoPriority>{{.ioClass = IoPriorityClass::BestEffort, .level = 5}}));
}

TEST(LinuxIoPriorityTest, DecodeRefusesAnErrorOrAnUnknownClass)
{
    EXPECT_FALSE(IoPrio::decode(-1).has_value());
    EXPECT_FALSE(IoPrio::decode(4 << 13).has_value());
    EXPECT_FALSE(IoPrio::decode(7 << 13).has_value());
}

TEST(LinuxIoPriorityTest, EncodeThenDecodeRoundTrips)
{
    constexpr std::array<IoPriorityClass, 4> CLASSES{
        IoPriorityClass::None, IoPriorityClass::Realtime, IoPriorityClass::BestEffort, IoPriorityClass::Idle};
    for (const IoPriorityClass ioClass : CLASSES)
    {
        for (int level = 0; level <= 7; ++level)
        {
            SCOPED_TRACE((static_cast<int>(ioClass) * 10) + level);
            const IoPriority priority{.ioClass = ioClass, .level = IoPrio::classHasLevels(ioClass) ? level : 0};
            EXPECT_EQ(IoPrio::decode(IoPrio::encode(priority)), priority);
        }
    }
}

// --- Error messages ------------------------------------------------------------------------------

TEST(LinuxIoPriorityTest, EpermForRealtimeNamesTheCapability)
{
    const std::string message = ioPriorityErrorMessage(EPERM, IoPriorityClass::Realtime, 1234);
    EXPECT_NE(message.find("Realtime"), std::string::npos) << message;
    EXPECT_NE(message.find("CAP_SYS_ADMIN"), std::string::npos) << message;
    EXPECT_NE(message.find("1234"), std::string::npos) << message;
    EXPECT_EQ(message.find("another user"), std::string::npos) << message;
}

TEST(LinuxIoPriorityTest, EpermOtherwiseNamesAnotherUser)
{
    constexpr std::array<IoPriorityClass, 3> CLASSES{IoPriorityClass::None, IoPriorityClass::BestEffort, IoPriorityClass::Idle};
    for (const IoPriorityClass ioClass : CLASSES)
    {
        SCOPED_TRACE(static_cast<int>(ioClass));
        const std::string message = ioPriorityErrorMessage(EPERM, ioClass, 1234);
        EXPECT_NE(message.find("belongs to another user"), std::string::npos) << message;
        EXPECT_NE(message.find("Run TaskSmack as root"), std::string::npos) << message;
        EXPECT_EQ(message.find("ionice"), std::string::npos) << message; // ionice -p changes one thread
    }
}

TEST(LinuxIoPriorityTest, EsrchAndEinvalMapToTheirMessages)
{
    EXPECT_EQ(ioPriorityErrorMessage(ESRCH, IoPriorityClass::BestEffort, 1), "Process not found - may have already exited");
    EXPECT_NE(ioPriorityErrorMessage(EINVAL, IoPriorityClass::BestEffort, 1).find("Invalid I/O priority"), std::string::npos);
    EXPECT_FALSE(ioPriorityErrorMessage(EIO, IoPriorityClass::Idle, 1).empty());
}

} // namespace
} // namespace Platform
