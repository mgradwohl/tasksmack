/// @file test_ProcessCrashHistoryView.cpp
/// @brief Process Details' Recent crashes line (#1675): its read cadence against a fake read function
/// (once when first drawn, then only when the shared cache is stale, never per selection or while not
/// drawn, and a System tab read counting as fresh), the text of each state (not read, none, some,
/// unreadable, no crash history on the platform), the tooltip, and its ImGui render, headless.

#include "App/Panels/ProcessCrashHistoryView.h"
#include "Domain/CrashHistory.h"
#include "Domain/SamplingConfig.h"
#include "Platform/ISystemInfoProbe.h"

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace App
{
namespace
{

using Platform::CrashesInfo;
using Platform::CrashEvent;
using Platform::OsFamily;

constexpr float REFRESH_SECONDS = static_cast<float>(Domain::Sampling::PROCESS_CRASH_HISTORY_REFRESH_MS) / 1000.0F;

[[nodiscard]] CrashEvent crash(std::string application, std::uint64_t unixSeconds, bool hang = false)
{
    CrashEvent event;
    event.application = std::move(application);
    event.unixSeconds = unixSeconds;
    event.hang = hang;
    return event;
}

[[nodiscard]] CrashesInfo listed(OsFamily family, std::vector<CrashEvent> events)
{
    CrashesInfo info;
    info.available = true;
    info.listed = true;
    info.family = family;
    info.events = std::move(events);
    return info;
}

[[nodiscard]] Domain::CrashHistorySnapshot snapshotOf(CrashesInfo crashes)
{
    Domain::CrashHistorySnapshot snapshot;
    snapshot.version = 1;
    snapshot.readAtUnixSeconds = 1'790'000'000;
    snapshot.crashes = std::move(crashes);
    return snapshot;
}

TEST(ProcessCrashHistoryViewTest, ReadsWhenFirstDrawnThenOnlyWhenStale)
{
    std::atomic<int> reads = 0;
    const auto history = std::make_shared<Domain::CrashHistory>(
        [&reads]
        {
            ++reads;
            return CrashesInfo{};
        });
    ProcessCrashHistoryView view;
    view.setHistory(history);

    EXPECT_FALSE(view.update(100.0F)); // never drawn: never read
    view.markDrawn();
    EXPECT_TRUE(view.update(0.0F));
    view.finishPendingRead();
    EXPECT_EQ(reads.load(), 1);
    EXPECT_EQ(history->version(), 1U);

    // Drawn every frame, and a new selection makes no difference: nothing until the cache is stale.
    for (int frame = 0; frame < 10; ++frame)
    {
        view.markDrawn();
        EXPECT_FALSE(view.update(REFRESH_SECONDS / 20.0F));
    }
    view.markDrawn();
    EXPECT_TRUE(view.update(REFRESH_SECONDS));
    view.finishPendingRead();
    EXPECT_EQ(reads.load(), 2);

    // Stale but not drawn (another tab): no read until it is drawn again.
    EXPECT_FALSE(view.update(0.0F)); // takes in the read just published: fresh from here
    EXPECT_FALSE(view.update(REFRESH_SECONDS * 2.0F));
    view.markDrawn();
    EXPECT_TRUE(view.update(0.0F));
    view.finishPendingRead();
    EXPECT_EQ(reads.load(), 3);
}

TEST(ProcessCrashHistoryViewTest, ASystemTabReadCountsAsFresh)
{
    std::atomic<int> reads = 0;
    const auto history = std::make_shared<Domain::CrashHistory>(
        [&reads]
        {
            ++reads;
            return CrashesInfo{};
        });
    history->publish(listed(OsFamily::Windows, {}), 1'790'000'000); // the System tab's read
    ProcessCrashHistoryView view;
    view.setHistory(history);
    view.markDrawn();
    EXPECT_FALSE(view.update(0.0F));
    view.markDrawn();
    EXPECT_FALSE(view.update(REFRESH_SECONDS * 0.9F));
    history->publish(listed(OsFamily::Windows, {}), 1'790'000'050); // a Refresh there restarts the clock
    view.markDrawn();
    EXPECT_FALSE(view.update(REFRESH_SECONDS * 0.5F));
    EXPECT_EQ(reads.load(), 0);
}

TEST(ProcessCrashHistoryViewTest, NoHistoryNeverReadsOrDraws)
{
    ProcessCrashHistoryView view;
    view.markDrawn();
    EXPECT_FALSE(view.update(REFRESH_SECONDS * 2.0F));
    EXPECT_FALSE(view.line("app.exe").visible);
}

TEST(ProcessCrashLineTest, NotReadYetSaysReading)
{
    const Detail::CrashLine line = Detail::crashLineFor(Domain::CrashHistorySnapshot{}, "app.exe");
    EXPECT_TRUE(line.visible);
    EXPECT_EQ(line.value, "Reading...");
    EXPECT_EQ(line.tone, Detail::CrashLineTone::Muted);
}

TEST(ProcessCrashLineTest, NoneIsMuted)
{
    const Detail::CrashLine line = Detail::crashLineFor(snapshotOf(listed(OsFamily::Windows, {crash("other.exe", 5)})), "app.exe");
    EXPECT_TRUE(line.visible);
    EXPECT_EQ(line.value, "None in the last 14 days");
    EXPECT_EQ(line.tone, Detail::CrashLineTone::Muted);
    EXPECT_TRUE(line.tooltip.contains("ignoring case: app.exe")) << line.tooltip;
}

TEST(ProcessCrashLineTest, CountsAndListsTheNewestOnes)
{
    CrashEvent faulted = crash("APP.exe", 1'790'000'000);
    faulted.appVersion = "1.2.3";
    faulted.module = "ntdll.dll";
    faulted.exceptionCode = "0xC0000005";
    CrashEvent hung = crash("app.exe", 1'789'000'000, true);
    hung.hangType = "Quiesce";
    const Detail::CrashLine line = Detail::crashLineFor(
        snapshotOf(listed(OsFamily::Windows, {faulted, crash("other.exe", 1'789'500'000), hung})), R"(C:\Apps\app.exe)");
    EXPECT_EQ(line.tone, Detail::CrashLineTone::Warning);
    EXPECT_TRUE(line.value.starts_with("1 crash, 1 hang in the last 14 days (last ")) << line.value;
    EXPECT_TRUE(line.tooltip.contains("APP.exe 1.2.3 crashed in ntdll.dll, exception 0xC0000005")) << line.tooltip;
    EXPECT_TRUE(line.tooltip.contains("hung (Quiesce)")) << line.tooltip;
    EXPECT_FALSE(line.tooltip.contains("other.exe")) << line.tooltip;
    EXPECT_TRUE(line.tooltip.contains("System tab")) << line.tooltip;

    const Detail::CrashLine hangsOnly = Detail::crashLineFor(snapshotOf(listed(OsFamily::Windows, {hung, hung})), "app.exe");
    EXPECT_TRUE(hangsOnly.value.starts_with("2 hangs in the last 14 days")) << hangsOnly.value;
}

TEST(ProcessCrashLineTest, MoreThanTheTooltipShowsAreCounted)
{
    std::vector<CrashEvent> events;
    events.reserve(7);
    for (std::uint64_t i = 0; i < 7; ++i)
    {
        events.push_back(crash("app.exe", 1'790'000'000 - i));
    }
    CrashesInfo info = listed(OsFamily::Windows, std::move(events));
    info.capped = true;
    const Detail::CrashLine line = Detail::crashLineFor(snapshotOf(std::move(info)), "app.exe");
    EXPECT_TRUE(line.value.starts_with("7 crashes")) << line.value;
    EXPECT_TRUE(line.tooltip.contains("...and 2 more")) << line.tooltip;
    EXPECT_TRUE(line.tooltip.contains("200 newest")) << line.tooltip;
}

TEST(ProcessCrashLineTest, LinuxSaysTheNameWasMatchedCut)
{
    const Detail::CrashLine line =
        Detail::crashLineFor(snapshotOf(listed(OsFamily::Linux, {crash("gnome-shell-cal", 1'790'000'000)})), "gnome-shell-calendar-server");
    EXPECT_TRUE(line.value.starts_with("1 crash in the last 14 days")) << line.value;
    EXPECT_TRUE(line.tooltip.contains("first 15 characters of gnome-shell-calendar-server")) << line.tooltip;
}

TEST(ProcessCrashLineTest, UnreadableAndUnsupportedStates)
{
    CrashesInfo denied;
    denied.available = true;
    denied.accessDenied = true;
    denied.unavailableReason = "Reading the Application log needs administrator rights";
    const Detail::CrashLine deniedLine = Detail::crashLineFor(snapshotOf(denied), "app.exe");
    EXPECT_TRUE(deniedLine.visible);
    EXPECT_EQ(deniedLine.value, "Not readable without more permission");
    EXPECT_EQ(deniedLine.tone, Detail::CrashLineTone::Muted);
    EXPECT_TRUE(deniedLine.tooltip.contains("administrator rights")) << deniedLine.tooltip;

    CrashesInfo failed;
    failed.available = true;
    EXPECT_EQ(Detail::crashLineFor(snapshotOf(failed), "app.exe").value, "Unavailable");

    // A platform without a crash history: no line.
    EXPECT_FALSE(Detail::crashLineFor(snapshotOf(CrashesInfo{}), "app.exe").visible);
}

class ProcessCrashHistoryViewRenderTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Context = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.DisplaySize = ImVec2(1200.0F, 800.0F);
        io.DeltaTime = 1.0F / 60.0F;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        io.Fonts->AddFontDefault();
    }

    void TearDown() override
    {
        ImGui::DestroyContext(m_Context);
    }

    [[nodiscard]] static std::string renderAndCapture(ProcessCrashHistoryView& view, std::string_view executable)
    {
        ImGui::NewFrame();
        ImGui::Begin("Details");
        ImGui::LogToBuffer();
        view.render(executable);
        std::string captured = GImGui->LogBuffer.c_str();
        ImGui::LogFinish();
        ImGui::End();
        ImGui::Render();
        return captured;
    }

  private:
    ImGuiContext* m_Context = nullptr;
};

TEST_F(ProcessCrashHistoryViewRenderTest, DrawsTheLineAndMarksItDrawn)
{
    const auto history = std::make_shared<Domain::CrashHistory>([] { return CrashesInfo{}; });
    history->publish(listed(OsFamily::Windows, {crash("app.exe", 1'790'000'000), crash("app.exe", 1'789'000'000, true)}), 1'790'000'100);
    ProcessCrashHistoryView view;
    view.setHistory(history);

    const std::string text = renderAndCapture(view, "app.exe");
    EXPECT_TRUE(text.contains("Recent crashes:")) << text;
    EXPECT_TRUE(text.contains("1 crash, 1 hang in the last 14 days")) << text;
    // Drawn, so a stale cache would be read now; this one is fresh.
    EXPECT_FALSE(view.update(0.0F));

    const std::string none = renderAndCapture(view, "quiet.exe");
    EXPECT_TRUE(none.contains("None in the last 14 days")) << none;
}

TEST_F(ProcessCrashHistoryViewRenderTest, DrawsNothingWithoutAHistoryOrOnAnUnsupportedPlatform)
{
    ProcessCrashHistoryView none;
    EXPECT_TRUE(renderAndCapture(none, "app.exe").empty());

    const auto history = std::make_shared<Domain::CrashHistory>([] { return CrashesInfo{}; });
    history->publish(CrashesInfo{}, 1); // read, and not available here
    ProcessCrashHistoryView unsupported;
    unsupported.setHistory(history);
    EXPECT_TRUE(renderAndCapture(unsupported, "app.exe").empty());
    EXPECT_FALSE(unsupported.update(REFRESH_SECONDS * 2.0F)); // not drawn: never re-read
}

} // namespace
} // namespace App
