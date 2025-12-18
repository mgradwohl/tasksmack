/// @file test_ProcessActionsContract.cpp
/// @brief Cross-platform contract tests for Platform::IProcessActions via Platform::makeProcessActions()

#include "Platform/Factory.h"
#include "Platform/IProcessActions.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>

namespace Platform
{

TEST(ProcessActionsContractTest, FactoryConstructs)
{
    auto actions = makeProcessActions();
    ASSERT_NE(actions, nullptr);
}

TEST(ProcessActionsContractTest, NonExistentPidFailsGracefully)
{
    auto actions = makeProcessActions();
    ASSERT_NE(actions, nullptr);

    const auto caps = actions->actionCapabilities();
    const int32_t nonExistentPid = std::numeric_limits<int32_t>::max();

    if (caps.canTerminate)
    {
        const auto result = actions->terminate(nonExistentPid);
        EXPECT_FALSE(result.success);
        EXPECT_GT(result.errorMessage.size(), 0ULL);
    }

    if (caps.canKill)
    {
        const auto result = actions->kill(nonExistentPid);
        EXPECT_FALSE(result.success);
        EXPECT_GT(result.errorMessage.size(), 0ULL);
    }

    if (caps.canStop)
    {
        const auto result = actions->stop(nonExistentPid);
        EXPECT_FALSE(result.success);
        EXPECT_GT(result.errorMessage.size(), 0ULL);
    }

    if (caps.canContinue)
    {
        const auto result = actions->resume(nonExistentPid);
        EXPECT_FALSE(result.success);
        EXPECT_GT(result.errorMessage.size(), 0ULL);
    }
}

} // namespace Platform
