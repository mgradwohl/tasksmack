/// @file test_ServiceModel.cpp
/// @brief Domain::ServiceModel with a mock probe (#800): publication versions, name order, and the
/// unsupported probe.

#include "Domain/ServiceModel.h"
#include "Mocks/MockServiceProbe.h"
#include "Platform/IServiceProbe.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <utility>

namespace Domain
{
namespace
{

[[nodiscard]] Platform::ServiceInfo service(const char* name, Platform::ServiceState state, std::uint32_t pid = 0)
{
    Platform::ServiceInfo info;
    info.name = name;
    info.displayName = name;
    info.state = state;
    info.pid = pid;
    return info;
}

TEST(ServiceModelTest, PublishesNothingBeforeTheFirstSample)
{
    const ServiceModel model(std::make_unique<Mocks::MockServiceProbe>());
    EXPECT_EQ(model.version(), 0U);
    EXPECT_TRUE(model.publication()->services.empty());
    EXPECT_TRUE(model.capabilities().canEnumerate);
}

TEST(ServiceModelTest, SamplePublishesServicesOrderedByNameIgnoringCase)
{
    auto probe = std::make_unique<Mocks::MockServiceProbe>();
    probe->setServices({
        service("Winmgmt", Platform::ServiceState::Running, 1234),
        service("bits", Platform::ServiceState::Stopped),
        service("EventLog", Platform::ServiceState::Running, 42),
    });
    ServiceModel model(std::move(probe));

    model.sample();

    const auto publication = model.publication();
    EXPECT_EQ(publication->version, 1U);
    ASSERT_EQ(publication->services.size(), 3U);
    EXPECT_EQ(publication->services[0].name, "bits");
    EXPECT_EQ(publication->services[1].name, "EventLog");
    EXPECT_EQ(publication->services[2].name, "Winmgmt");
    EXPECT_EQ(publication->services[2].pid, 1234U);
}

TEST(ServiceModelTest, EachSampleIsANewImmutableGeneration)
{
    auto probe = std::make_unique<Mocks::MockServiceProbe>();
    auto* raw = probe.get();
    raw->setServices({service("a", Platform::ServiceState::Stopped)});
    ServiceModel model(std::move(probe));

    model.sample();
    const auto first = model.publication();
    raw->setServices({service("a", Platform::ServiceState::Running, 7)});
    model.sample();

    EXPECT_EQ(model.version(), 2U);
    EXPECT_EQ(raw->enumerateCount(), 2);
    EXPECT_EQ(first->services[0].state, Platform::ServiceState::Stopped); // a held generation never changes
    EXPECT_EQ(model.publication()->services[0].state, Platform::ServiceState::Running);
}

TEST(ServiceModelTest, AFailedReadKeepsTheLastListMarkedStaleWithItsReason)
{
    auto probe = std::make_unique<Mocks::MockServiceProbe>();
    auto* raw = probe.get();
    raw->setServices({service("a", Platform::ServiceState::Running, 7), service("b", Platform::ServiceState::Stopped)});
    ServiceModel model(std::move(probe));

    model.sample();
    EXPECT_FALSE(model.publication()->stale);

    raw->setFailure("Access to the Service Control Manager was denied");
    model.sample();
    const auto failed = model.publication();
    EXPECT_EQ(failed->version, 2U);
    EXPECT_TRUE(failed->stale);
    EXPECT_EQ(failed->failureReason, "Access to the Service Control Manager was denied");
    ASSERT_EQ(failed->services.size(), 2U); // the last good rows, not an empty list
    EXPECT_EQ(failed->services[0].pid, 7U);

    raw->setServices({service("c", Platform::ServiceState::Running, 9)});
    model.sample();
    const auto recovered = model.publication();
    EXPECT_FALSE(recovered->stale);
    EXPECT_TRUE(recovered->failureReason.empty());
    ASSERT_EQ(recovered->services.size(), 1U);
    EXPECT_EQ(recovered->services[0].name, "c");
}

TEST(ServiceModelTest, AFailedFirstReadPublishesNoRowsMarkedStale)
{
    auto probe = std::make_unique<Mocks::MockServiceProbe>();
    probe->setFailure("The Service Control Manager could not list the services (error 1722)");
    ServiceModel model(std::move(probe));
    model.sample();
    EXPECT_EQ(model.version(), 1U);
    EXPECT_TRUE(model.publication()->stale);
    EXPECT_TRUE(model.publication()->services.empty());
}

TEST(ServiceModelTest, UnsupportedProbeReportsNoEnumeration)
{
    ServiceModel model(std::make_unique<Platform::UnsupportedServiceProbe>());
    EXPECT_FALSE(model.capabilities().canEnumerate);
    EXPECT_FALSE(model.capabilities().unavailableReason.empty());
    model.sample();
    EXPECT_TRUE(model.publication()->services.empty());
    EXPECT_TRUE(model.publication()->stale);
    EXPECT_EQ(model.publication()->failureReason, model.capabilities().unavailableReason);
}

TEST(ServiceModelTest, NullProbeThrows)
{
    EXPECT_THROW(ServiceModel(nullptr), std::invalid_argument);
}

} // namespace
} // namespace Domain
