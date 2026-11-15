// Tests for the broker's session rules (#2550, #3783): each request kind's one RequestKind trait, its category
// (the Hello, an action or a read) and whether it puts off the helper's idle exit. Which category a kind is in
// is a security property, so every kind is pinned here, at compile time and through a Request at run time.
// The pipe's session loop that acts on them has its own tests: test_BrokerPipe.cpp (Windows).

#include "Platform/BrokerProtocol.h"
#include "Platform/BrokerSessionRules.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <utility>
#include <variant>

namespace Platform::Broker
{
namespace
{

// Pinned at compile time: moving a kind to another category fails to build until this list moves with it.
static_assert(RequestKind<Hello>::CATEGORY == RequestCategory::Hello);
static_assert(RequestKind<SignalRequest>::CATEGORY == RequestCategory::Action);
static_assert(RequestKind<PriorityRequest>::CATEGORY == RequestCategory::Action);
static_assert(RequestKind<IoPriorityRequest>::CATEGORY == RequestCategory::Action);
static_assert(RequestKind<AffinityRequest>::CATEGORY == RequestCategory::Action);
static_assert(RequestKind<EfficiencyModeRequest>::CATEGORY == RequestCategory::Action);
static_assert(RequestKind<CloseConnectionRequest>::CATEGORY == RequestCategory::Action);
static_assert(RequestKind<TaskManagerReplacementRequest>::CATEGORY == RequestCategory::Action);
static_assert(RequestKind<SessionActionRequest>::CATEGORY == RequestCategory::Action);
static_assert(RequestKind<ThreadActionRequest>::CATEGORY == RequestCategory::Action);
static_assert(RequestKind<EnvironmentRequest>::CATEGORY == RequestCategory::Read);
static_assert(RequestKind<ModulesRequest>::CATEGORY == RequestCategory::Read);
static_assert(RequestKind<OpenFilesRequest>::CATEGORY == RequestCategory::Read);
static_assert(RequestKind<ConnectionsRequest>::CATEGORY == RequestCategory::Read);
static_assert(RequestKind<ThreadsRequest>::CATEGORY == RequestCategory::Read);
static_assert(RequestKind<OpenFilesBatchRequest>::CATEGORY == RequestCategory::Read);
static_assert(RequestKind<MemoryMapRequest>::CATEGORY == RequestCategory::Read);

// A new kind gets a trait, and a pin above: this count moves only with both.
static_assert(std::variant_size_v<Request> == 17, "pin the new request kind's category above");

/// One alternative of Request, value-initialized, as a Request.
template<std::size_t Index> [[nodiscard]] Request kindAt()
{
    return Request{std::in_place_index<Index>};
}

template<std::size_t... Indices> [[nodiscard]] std::size_t countOf(RequestCategory category, std::index_sequence<Indices...> /*indices*/)
{
    return (static_cast<std::size_t>(isRequestOf(kindAt<Indices>(), category)) + ...);
}

template<std::size_t... Indices> [[nodiscard]] std::size_t countPuttingOff(std::index_sequence<Indices...> /*indices*/)
{
    return (static_cast<std::size_t>(putsOffTheIdleExit(kindAt<Indices>())) + ...);
}

template<std::size_t... Indices> [[nodiscard]] bool eachInExactlyOneCategory(std::index_sequence<Indices...> /*indices*/)
{
    const auto exactlyOne = [](const Request& request)
    {
        return static_cast<int>(isRequestOf(request, RequestCategory::Hello)) +
                   static_cast<int>(isRequestOf(request, RequestCategory::Action)) +
                   static_cast<int>(isRequestOf(request, RequestCategory::Read)) ==
               1;
    };
    return (exactlyOne(kindAt<Indices>()) && ...);
}

constexpr auto EVERY_KIND = std::make_index_sequence<std::variant_size_v<Request>>{};

TEST(BrokerSessionRulesTest, EachKindIsTheHelloAnActionOrARead)
{
    EXPECT_TRUE(eachInExactlyOneCategory(EVERY_KIND));
    EXPECT_EQ(countOf(RequestCategory::Hello, EVERY_KIND), 1U);
    EXPECT_EQ(countOf(RequestCategory::Action, EVERY_KIND), 9U);
    EXPECT_EQ(countOf(RequestCategory::Read, EVERY_KIND), 7U);
}

TEST(BrokerSessionRulesTest, TheHelloIsTheHello)
{
    EXPECT_TRUE(isRequestOf(Hello{}, RequestCategory::Hello));
    EXPECT_TRUE(putsOffTheIdleExit(Hello{}));
}

TEST(BrokerSessionRulesTest, EveryActionIsAnActionAndPutsOffTheIdleExit)
{
    const auto action = [](const Request& request)
    {
        return isRequestOf(request, RequestCategory::Action) && putsOffTheIdleExit(request);
    };
    EXPECT_TRUE(action(SignalRequest{}));
    EXPECT_TRUE(action(PriorityRequest{}));
    EXPECT_TRUE(action(IoPriorityRequest{})) << "#3362";
    EXPECT_TRUE(action(AffinityRequest{}));
    EXPECT_TRUE(action(EfficiencyModeRequest{})) << "#3071";
    EXPECT_TRUE(action(CloseConnectionRequest{})) << "#3358";
    EXPECT_TRUE(action(TaskManagerReplacementRequest{})) << "#3072";
    EXPECT_TRUE(action(SessionActionRequest{})) << "#3076";
    EXPECT_TRUE(action(ThreadActionRequest{})) << "#3069";
}

TEST(BrokerSessionRulesTest, EveryReadIsAReadAndLeavesTheIdleExit)
{
    const auto read = [](const Request& request)
    {
        return isRequestOf(request, RequestCategory::Read) && !putsOffTheIdleExit(request);
    };
    EXPECT_TRUE(read(EnvironmentRequest{}));
    EXPECT_TRUE(read(ModulesRequest{}));
    EXPECT_TRUE(read(OpenFilesRequest{}));
    EXPECT_TRUE(read(ConnectionsRequest{}));
    EXPECT_TRUE(read(ThreadsRequest{})) << "#3069";
    EXPECT_TRUE(read(OpenFilesBatchRequest{})) << "#3578";
    EXPECT_TRUE(read(MemoryMapRequest{})) << "#3665";
}

TEST(BrokerSessionRulesTest, OnlyTheHelloAndActionsPutOffTheIdleExit)
{
    EXPECT_EQ(countPuttingOff(EVERY_KIND), 10U);
}

} // namespace
} // namespace Platform::Broker
