/// @file test_StartupModel.cpp
/// @brief Domain::StartupModel with a mock probe (#801): publication versions, name order, immutable
/// generations, the unsupported probe, and each entry's Startup impact and its read cadence (#3073).

#include "Domain/SamplingConfig.h"
#include "Domain/StartupImpact.h"
#include "Domain/StartupModel.h"
#include "Mocks/MockStartupProbe.h"
#include "Platform/IStartupProbe.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
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

TEST(StartupModelTest, NameOrderFoldsAsciiLettersOnlyAndKeepsTheProbesOrderForTies)
{
    // Mixed case folds; a byte of 0x80 and up sorts as itself, after every ASCII letter, whatever the
    // locale (#2566): 0xE4 (Latin-1's ä) after 0xC4 (Ä), never folded into a tie with it. Names equal
    // ignoring case keep the probe's order (the sort is stable). An empty name first.
    auto probe = std::make_unique<Mocks::MockStartupProbe>();
    probe->setEntries({entry("\xE4pp"), entry("teams", false), entry("\xC4pp"), entry("TEAMS"), entry(""), entry("Zoom")});
    StartupModel model(std::move(probe));

    model.sample();

    const auto publication = model.publication();
    ASSERT_EQ(publication->entries.size(), 6U);
    EXPECT_EQ(publication->entries[0].name, "");
    EXPECT_EQ(publication->entries[1].name, "teams");
    EXPECT_FALSE(publication->entries[1].enabled);
    EXPECT_EQ(publication->entries[2].name, "TEAMS");
    EXPECT_EQ(publication->entries[3].name, "Zoom");
    EXPECT_EQ(publication->entries[4].name, "\xC4pp");
    EXPECT_EQ(publication->entries[5].name, "\xE4pp");
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

// ========== Startup impact (#3073) ==========

[[nodiscard]] Platform::StartupCapabilities withImpact()
{
    return {.canEnumerate = true,
            .hasEnabledState = true,
            .hasDisabledTime = true,
            .hasPublisher = true,
            .canResolveShortcuts = true,
            .hasImpact = true,
            .unavailableReason = {}};
}

/// A sign-in at which Busy.exe used 2 s of CPU and Quiet.exe 1 ms.
[[nodiscard]] Platform::StartupImpactReading signIn(std::uint64_t busyCpuUs = 2'000'000)
{
    Platform::StartupImpactReading reading;
    reading.processes = {
        {.path = R"(C:\Apps\Busy.exe)",
         .pid = 10,
         .parentPid = 1,
         .startTicks = 5,
         .parentStartTicks = 1,
         .cpuUs = busyCpuUs,
         .diskBytes = 0},
        {.path = R"(C:\Apps\Quiet.exe)", .pid = 11, .parentPid = 1, .startTicks = 6, .parentStartTicks = 1, .cpuUs = 1000, .diskBytes = 0},
    };
    reading.signInUnixSeconds = 1792774334;
    reading.windowSeconds = 90;
    return reading;
}

[[nodiscard]] Platform::StartupEntry program(const char* name, bool enabled = true)
{
    auto startup = entry(name, enabled);
    startup.executablePath = std::string(R"(C:\Apps\)") + name + ".exe";
    return startup;
}

/// A fake clock the test moves.
struct ImpactClock
{
    StartupModel::Clock::time_point now;

    [[nodiscard]] StartupModel::NowFunction function()
    {
        return [this]
        {
            return now;
        };
    }
};

TEST(StartupModelTest, PublishesEachEntrysImpactByIndex)
{
    auto probe = std::make_unique<Mocks::MockStartupProbe>(withImpact());
    probe->setEntries({program("Quiet"), program("Busy"), program("Off", false), program("Absent")});
    probe->setImpact(signIn());
    StartupModel model(std::move(probe));

    model.sample();

    const auto publication = model.publication();
    ASSERT_EQ(publication->entries.size(), 4U);
    ASSERT_EQ(publication->impacts.size(), 4U);
    // Name order: Absent, Busy, Off, Quiet -- and the impacts follow it.
    EXPECT_EQ(publication->entries[1].name, "Busy");
    EXPECT_EQ(publication->impacts[0].rating, StartupImpact::NotMeasured);
    EXPECT_EQ(publication->impacts[1].rating, StartupImpact::High);
    EXPECT_EQ(publication->impacts[1].cpuUs, 2'000'000U);
    EXPECT_EQ(publication->impacts[2].rating, StartupImpact::None);
    EXPECT_EQ(publication->impacts[3].rating, StartupImpact::Low);
    EXPECT_EQ(publication->impactSignInUnixSeconds, 1792774334U);
    EXPECT_TRUE(publication->impactUnavailableReason.empty());
}

TEST(StartupModelTest, ReadsTheSignInMeasurementOnlyEverySoOften)
{
    auto probe = std::make_unique<Mocks::MockStartupProbe>(withImpact());
    auto* raw = probe.get();
    raw->setEntries({program("Busy")});
    raw->setImpact(signIn());
    ImpactClock clock;
    StartupModel model(std::move(probe), clock.function());

    model.sample();
    EXPECT_EQ(raw->impactReadCount(), 1);
    const auto first = model.publication();

    // A new measurement appears; samples within the refresh interval keep the one read.
    raw->setImpact(signIn(1000));
    clock.now += std::chrono::seconds(Sampling::STARTUP_IMPACT_REFRESH_SECONDS - 1);
    model.sample();
    EXPECT_EQ(raw->enumerateCount(), 2);
    EXPECT_EQ(raw->impactReadCount(), 1);
    EXPECT_EQ(model.publication()->impacts[0].rating, StartupImpact::High);

    clock.now += std::chrono::seconds(1);
    model.sample();
    EXPECT_EQ(raw->impactReadCount(), 2);
    EXPECT_EQ(model.publication()->impacts[0].rating, StartupImpact::Low);
    // A held generation never changes.
    EXPECT_EQ(first->impacts[0].rating, StartupImpact::High);
}

TEST(StartupModelTest, AnUnreadableMeasurementLeavesEveryEntryNotMeasuredWithTheReason)
{
    auto probe = std::make_unique<Mocks::MockStartupProbe>(withImpact());
    probe->setEntries({program("Busy"), program("Off", false)});
    Platform::StartupImpactReading unreadable;
    unreadable.unavailableReason = "Windows hasn't measured a sign-in for this account yet";
    probe->setImpact(unreadable);
    StartupModel model(std::move(probe));

    model.sample();

    const auto publication = model.publication();
    ASSERT_EQ(publication->impacts.size(), 2U);
    EXPECT_EQ(publication->impacts[0].rating, StartupImpact::NotMeasured);
    EXPECT_EQ(publication->impacts[1].rating, StartupImpact::None);
    EXPECT_EQ(publication->impactSignInUnixSeconds, 0U);
    EXPECT_EQ(publication->impactUnavailableReason, "Windows hasn't measured a sign-in for this account yet");
}

TEST(StartupModelTest, WithoutImpactDataNothingIsReadOrPublished)
{
    // The Linux probe's capabilities: no impact data, so the column is hidden and nothing is read.
    auto probe = std::make_unique<Mocks::MockStartupProbe>();
    auto* raw = probe.get();
    raw->setEntries({program("Busy")});
    raw->setImpact(signIn());
    StartupModel model(std::move(probe));

    model.sample();

    EXPECT_EQ(raw->impactReadCount(), 0);
    EXPECT_TRUE(model.publication()->impacts.empty());
    EXPECT_FALSE(model.capabilities().hasImpact);
}

} // namespace
} // namespace Domain
