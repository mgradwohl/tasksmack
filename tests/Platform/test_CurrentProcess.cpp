/// @file test_CurrentProcess.cpp
/// @brief Platform::currentProcessId() (#804): the calling process's own PID, from
/// GetCurrentProcessId() on Windows and getpid() elsewhere (#1566).

#include "Platform/CurrentProcess.h"

#include <gtest/gtest.h>

#ifdef _WIN32
// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <processthreadsapi.h>
// clang-format on
#else
#include <unistd.h>
#endif

#include <cstdint>

namespace Platform
{
namespace
{

TEST(CurrentProcessTest, IsThisProcessesPid)
{
#ifdef _WIN32
    const auto expected = static_cast<std::int64_t>(GetCurrentProcessId());
#else
    const auto expected = static_cast<std::int64_t>(getpid());
#endif
    const std::int32_t pid = currentProcessId();
    EXPECT_GT(pid, 0);
    EXPECT_EQ(static_cast<std::int64_t>(pid), expected);
}

TEST(CurrentProcessTest, IsStableAcrossCalls)
{
    EXPECT_EQ(currentProcessId(), currentProcessId());
}

} // namespace
} // namespace Platform
