/// @file test_ProcessEnvironmentView.cpp
/// @brief The Environment section's state, without ImGui (#179): it reads only while drawn open, once
/// on opening and then at PROCESS_ENVIRONMENT_REFRESH_MS, through the injected reader for the given
/// target; it sorts and masks what it reads; each read status is kept for the status line; reveals
/// last until the selection changes; and the filter never searches a masked value.

#include "App/Panels/ProcessEnvironmentView.h"
#include "Domain/SamplingConfig.h"
#include "Mocks/MockProbes.h"
#include "Platform/IProcessActions.h"
#include "Platform/IProcessEnvironment.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace App
{
namespace
{

constexpr Platform::ProcessTarget TARGET{.pid = 4242, .startTimeTicks = 777};
constexpr float REFRESH_SECONDS = static_cast<float>(Domain::Sampling::PROCESS_ENVIRONMENT_REFRESH_MS) / 1000.0F;

[[nodiscard]] Platform::EnvironmentReadResult okResult(std::vector<Platform::EnvironmentVariable> variables)
{
    return {.status = Platform::EnvironmentReadStatus::Ok, .variables = std::move(variables)};
}

/// One frame as the panel runs it: update (reads if due), then render drew the section open or not.
bool frame(ProcessEnvironmentView& view, TestMocks::MockProcessEnvironmentReader& reader, float deltaSeconds, bool drawnOpen)
{
    const bool read = view.update(&reader, TARGET, deltaSeconds);
    if (drawnOpen)
    {
        view.markDrawnOpen();
    }
    return read;
}

// ========== Read cadence ==========

TEST(ProcessEnvironmentViewTest, NeverReadsWhileTheSectionIsClosed)
{
    ProcessEnvironmentView view;
    TestMocks::MockProcessEnvironmentReader reader;
    for (int i = 0; i < 10; ++i)
    {
        EXPECT_FALSE(frame(view, reader, REFRESH_SECONDS, false));
    }
    EXPECT_EQ(reader.readCount(), 0);
    EXPECT_FALSE(view.hasRead());
}

TEST(ProcessEnvironmentViewTest, ReadsOnOpeningThenAtTheRefreshCadence)
{
    ProcessEnvironmentView view;
    TestMocks::MockProcessEnvironmentReader reader;
    reader.setResult(okResult({{.name = "FOO", .value = "bar"}}));

    // Frame 1 draws it open; the read happens in the next frame's update, for the target given.
    EXPECT_FALSE(frame(view, reader, 0.016F, true));
    EXPECT_TRUE(frame(view, reader, 0.016F, true));
    EXPECT_EQ(reader.readCount(), 1);
    EXPECT_EQ(reader.lastTarget().pid, TARGET.pid);
    EXPECT_EQ(reader.lastTarget().startTimeTicks, TARGET.startTimeTicks);
    EXPECT_TRUE(view.hasRead());

    // Many frames within the interval: no further read.
    float elapsed = 0.0F;
    while (elapsed + 0.1F < REFRESH_SECONDS)
    {
        EXPECT_FALSE(frame(view, reader, 0.1F, true));
        elapsed += 0.1F;
    }
    EXPECT_EQ(reader.readCount(), 1);

    // Past it: one more.
    EXPECT_TRUE(frame(view, reader, 0.2F, true));
    EXPECT_EQ(reader.readCount(), 2);
}

TEST(ProcessEnvironmentViewTest, StopsReadingWhenClosedAndReadsAgainWhenReopenedLater)
{
    ProcessEnvironmentView view;
    TestMocks::MockProcessEnvironmentReader reader;
    static_cast<void>(frame(view, reader, 0.016F, true));
    static_cast<void>(frame(view, reader, 0.016F, true));
    ASSERT_EQ(reader.readCount(), 1);

    // Closed (the first update still sees last frame's open section, well within the interval) ...
    EXPECT_FALSE(frame(view, reader, 0.016F, false));
    // ... for longer than the interval: nothing read meanwhile ...
    for (int i = 0; i < 5; ++i)
    {
        static_cast<void>(frame(view, reader, REFRESH_SECONDS, false));
    }
    EXPECT_EQ(reader.readCount(), 1);

    // ... and the stale data is refreshed as soon as it is shown again.
    static_cast<void>(frame(view, reader, 0.016F, true));
    EXPECT_TRUE(frame(view, reader, 0.016F, true));
    EXPECT_EQ(reader.readCount(), 2);
}

TEST(ProcessEnvironmentViewTest, SelectionChangeReadsAfreshWithoutWaitingForTheInterval)
{
    ProcessEnvironmentView view;
    TestMocks::MockProcessEnvironmentReader reader;
    static_cast<void>(frame(view, reader, 0.016F, true));
    static_cast<void>(frame(view, reader, 0.016F, true));
    ASSERT_EQ(reader.readCount(), 1);

    view.onSelectionChanged();
    EXPECT_FALSE(view.hasRead());
    view.markDrawnOpen();
    EXPECT_TRUE(view.update(&reader, TARGET, 0.016F));
    EXPECT_EQ(reader.readCount(), 2);
}

TEST(ProcessEnvironmentViewTest, NoReaderOrNoSupportMeansNoRead)
{
    ProcessEnvironmentView view;
    view.markDrawnOpen();
    EXPECT_FALSE(view.update(nullptr, TARGET, 1.0F));

    TestMocks::MockProcessEnvironmentReader reader;
    reader.setHasEnvironment(false);
    view.markDrawnOpen();
    EXPECT_FALSE(view.update(&reader, TARGET, 1.0F));
    EXPECT_EQ(reader.readCount(), 0);
}

// ========== Results ==========

TEST(ProcessEnvironmentViewTest, SortsByNameAndFlagsSecrets)
{
    ProcessEnvironmentView view;
    view.applyResult(okResult({
        {.name = "ZED", .value = "z"},
        {.name = "MY_API_TOKEN", .value = "supersecret"},
        {.name = "FOO", .value = "bar"},
        {.name = "PWD", .value = "/home/me"},
    }));
    ASSERT_EQ(view.rows().size(), 4U);
    EXPECT_EQ(view.rows()[0].name, "FOO");
    EXPECT_EQ(view.rows()[1].name, "MY_API_TOKEN");
    EXPECT_EQ(view.rows()[2].name, "PWD");
    EXPECT_EQ(view.rows()[3].name, "ZED");
    EXPECT_FALSE(view.rows()[0].secret);
    EXPECT_TRUE(view.rows()[1].secret);
    EXPECT_FALSE(view.rows()[2].secret);
    EXPECT_TRUE(view.isMasked(view.rows()[1]));
    EXPECT_FALSE(view.isMasked(view.rows()[0]));
}

TEST(ProcessEnvironmentViewTest, KeepsEachReadStatusForItsStatusLine)
{
    struct Case
    {
        Platform::EnvironmentReadStatus status;
        std::string_view text;
    };
    constexpr std::array<Case, 4> CASES{{
        {.status = Platform::EnvironmentReadStatus::PermissionDenied, .text = "Not readable (permission denied)"},
        {.status = Platform::EnvironmentReadStatus::ProcessExited, .text = "Process exited"},
        {.status = Platform::EnvironmentReadStatus::Unsupported, .text = "Not available on this platform"},
        {.status = Platform::EnvironmentReadStatus::Failed, .text = "Could not be read"},
    }};
    for (const Case& c : CASES)
    {
        SCOPED_TRACE(std::string(c.text));
        ProcessEnvironmentView view;
        TestMocks::MockProcessEnvironmentReader reader;
        reader.setResult({.status = c.status, .variables = {}});
        view.markDrawnOpen();
        ASSERT_TRUE(view.update(&reader, TARGET, 0.0F));
        EXPECT_TRUE(view.hasRead());
        EXPECT_EQ(view.status(), c.status);
        EXPECT_TRUE(view.rows().empty());
        EXPECT_EQ(Detail::environmentStatusText(view.status()), c.text);
    }
    EXPECT_TRUE(Detail::environmentStatusText(Platform::EnvironmentReadStatus::Ok).empty());
}

// ========== Reveal ==========

TEST(ProcessEnvironmentViewTest, RevealLastsAcrossReReadsAndEndsOnSelectionChange)
{
    ProcessEnvironmentView view;
    const auto result = okResult({{.name = "MY_API_TOKEN", .value = "supersecret"}, {.name = "DB_PASSWORD", .value = "hunter2"}});
    view.applyResult(result);

    view.toggleReveal("MY_API_TOKEN");
    EXPECT_TRUE(view.isRevealed("MY_API_TOKEN"));
    EXPECT_FALSE(view.isRevealed("DB_PASSWORD")); // one row only
    EXPECT_FALSE(view.isMasked(view.rows()[1]));  // MY_API_TOKEN sorts after DB_PASSWORD
    EXPECT_TRUE(view.isMasked(view.rows()[0]));

    view.applyResult(result); // the periodic re-read
    EXPECT_TRUE(view.isRevealed("MY_API_TOKEN"));

    view.toggleReveal("MY_API_TOKEN"); // pressed again: hidden again
    EXPECT_FALSE(view.isRevealed("MY_API_TOKEN"));
    view.toggleReveal("MY_API_TOKEN");

    view.onSelectionChanged();
    EXPECT_FALSE(view.isRevealed("MY_API_TOKEN"));
    EXPECT_TRUE(view.rows().empty());
}

// ========== Filter ==========

TEST(ProcessEnvironmentViewTest, FilterMatchesNamesAndPlainValuesButNeverMaskedValues)
{
    ProcessEnvironmentView view;
    view.applyResult(okResult({
        {.name = "FOO", .value = "bar"},
        {.name = "MY_API_TOKEN", .value = "supersecret"},
        {.name = "EDITOR", .value = "vim"},
    }));
    EXPECT_EQ(view.filteredRows().size(), 3U);

    view.setFilter("foo"); // by name, ignoring case
    ASSERT_EQ(view.filteredRows().size(), 1U);
    EXPECT_EQ(view.rows()[view.filteredRows()[0]].name, "FOO");

    view.setFilter("VIM"); // by a plain value
    ASSERT_EQ(view.filteredRows().size(), 1U);
    EXPECT_EQ(view.rows()[view.filteredRows()[0]].name, "EDITOR");

    view.setFilter("supersecret"); // a masked value is not searched: no probing a secret by guessing
    EXPECT_TRUE(view.filteredRows().empty());

    view.toggleReveal("MY_API_TOKEN"); // once revealed, it is
    EXPECT_EQ(view.filteredRows().size(), 1U);

    view.setFilter("token"); // the name of a masked row always matches
    EXPECT_EQ(view.filteredRows().size(), 1U);
}

} // namespace
} // namespace App
