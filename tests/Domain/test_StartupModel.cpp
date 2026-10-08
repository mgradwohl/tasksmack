/// @file test_StartupModel.cpp
/// @brief Domain::StartupModel with a mock probe (#801): publication versions, name order, immutable
/// generations, and the unsupported probe.

#include "Domain/StartupModel.h"
#include "Mocks/MockStartupProbe.h"
#include "Platform/IStartupProbe.h"

#include <gtest/gtest.h>

#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace Domain
{
namespace
{

[[nodiscard]] Platform::StartupEntry entry(const char* name, bool enabled = true)
{
    Platform::StartupEntry startup;
    startup.name = name;
    startup.command = std::string(R"("C:\Apps\)") + name + R"(.exe")";
    startup.enabled = enabled;
    return startup;
}

TEST(StartupModelTest, PublishesNothingBeforeTheFirstSample)
{
    const StartupModel model(std::make_unique<Mocks::MockStartupProbe>());
    EXPECT_EQ(model.version(), 0U);
    EXPECT_TRUE(model.publication()->entries.empty());
    EXPECT_TRUE(model.capabilities().canEnumerate);
}

TEST(StartupModelTest, SamplePublishesEntriesOrderedByNameIgnoringCase)
{
    auto probe = std::make_unique<Mocks::MockStartupProbe>();
    probe->setEntries({entry("Zoom"), entry("onedrive", false), entry("Discord")});
    StartupModel model(std::move(probe));

    model.sample();

    const auto publication = model.publication();
    EXPECT_EQ(publication->version, 1U);
    ASSERT_EQ(publication->entries.size(), 3U);
    EXPECT_EQ(publication->entries[0].name, "Discord");
    EXPECT_EQ(publication->entries[1].name, "onedrive");
    EXPECT_FALSE(publication->entries[1].enabled);
    EXPECT_EQ(publication->entries[2].name, "Zoom");
}

TEST(StartupModelTest, EachSampleIsANewImmutableGeneration)
{
    auto probe = std::make_unique<Mocks::MockStartupProbe>();
    auto* raw = probe.get();
    raw->setEntries({entry("a")});
    StartupModel model(std::move(probe));

    model.sample();
    const auto first = model.publication();
    raw->setEntries({entry("a", false), entry("b")});
    model.sample();

    EXPECT_EQ(model.version(), 2U);
    EXPECT_EQ(raw->enumerateCount(), 2);
    ASSERT_EQ(first->entries.size(), 1U); // a held generation never changes
    EXPECT_TRUE(first->entries[0].enabled);
    EXPECT_EQ(model.publication()->entries.size(), 2U);
    EXPECT_FALSE(model.publication()->entries[0].enabled);
}

TEST(StartupModelTest, UnsupportedProbeReportsNoEnumeration)
{
    StartupModel model(std::make_unique<Platform::UnsupportedStartupProbe>());
    EXPECT_FALSE(model.capabilities().canEnumerate);
    EXPECT_FALSE(model.capabilities().unavailableReason.empty());
    model.sample();
    EXPECT_TRUE(model.publication()->entries.empty());
}

TEST(StartupModelTest, NullProbeThrows)
{
    EXPECT_THROW(StartupModel(nullptr), std::invalid_argument);
}

} // namespace
} // namespace Domain
