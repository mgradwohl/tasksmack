/// @file test_ScopeFail.cpp
/// @brief Domain::ScopeFail (#4518): the cleanup runs when an exception leaves the scope, not on a normal exit,
/// the exception still propagates, and the guard is safe nested and while another exception is unwinding.

#include "Domain/ScopeFail.h"

#include <gtest/gtest.h>

#include <stdexcept>

namespace Domain
{
namespace
{

TEST(ScopeFailTest, DoesNotRunOnANormalExit)
{
    int runs = 0;
    {
        const ScopeFail guard([&runs] { ++runs; });
    }
    EXPECT_EQ(runs, 0);
}

TEST(ScopeFailTest, RunsWhenAnExceptionLeavesTheScopeAndTheExceptionPropagates)
{
    int runs = 0;
    EXPECT_THROW(
        {
            const ScopeFail guard([&runs] { ++runs; });
            throw std::runtime_error("boom");
        },
        std::runtime_error);
    EXPECT_EQ(runs, 1);
}

TEST(ScopeFailTest, ACleanupThatThrowsIsSwallowedAndTheOriginalExceptionStillPropagates)
{
    EXPECT_THROW(
        {
            const ScopeFail guard([] { throw std::logic_error("cleanup"); });
            throw std::runtime_error("boom");
        },
        std::runtime_error);
}

TEST(ScopeFailTest, NestedGuardsEachRunOnceForOneException)
{
    int outer = 0;
    int inner = 0;
    EXPECT_THROW(
        {
            const ScopeFail outerGuard([&outer] { ++outer; });
            {
                const ScopeFail innerGuard([&inner] { ++inner; });
                throw std::runtime_error("boom");
            }
        },
        std::runtime_error);
    EXPECT_EQ(inner, 1);
    EXPECT_EQ(outer, 1);
}

TEST(ScopeFailTest, AGuardMadeInACatchHandlerDoesNotFireForTheExceptionBeingHandled)
{
    int runs = 0;
    try
    {
        throw std::runtime_error("handled");
    }
    catch (const std::runtime_error&)
    {
        const ScopeFail guard([&runs] { ++runs; });
    }
    EXPECT_EQ(runs, 0);
}

TEST(ScopeFailTest, AGuardMadeWhileUnwindingFiresOnlyForItsOwnException)
{
    struct Unwinder
    {
        explicit Unwinder(int* counter) noexcept : runs(counter)
        {}
        Unwinder(const Unwinder&) = delete;
        Unwinder& operator=(const Unwinder&) = delete;
        Unwinder(Unwinder&&) = delete;
        Unwinder& operator=(Unwinder&&) = delete;
        int* runs;
        ~Unwinder() noexcept
        {
            // Made during another exception's unwinding: it must not fire on a normal exit.
            const ScopeFail guard([r = runs] { ++*r; });
        }
    };
    int runs = 0;
    EXPECT_THROW(
        {
            const Unwinder unwinder(&runs);
            throw std::runtime_error("boom");
        },
        std::runtime_error);
    EXPECT_EQ(runs, 0);
}

} // namespace
} // namespace Domain
