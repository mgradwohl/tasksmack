// Process Details' Threads section (#3069): its cadence, CPU % from two reads, sort, filter and statuses
// against a mock reader, and its real ImGui render, headless.

#include "App/Panels/ProcessThreadsView.h"
#include "Platform/IProcessActions.h"
#include "Platform/IProcessThreads.h"

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace App
{
namespace
{

using Platform::ProcessThread;
using Platform::ThreadsReadResult;
using Platform::ThreadsReadStatus;

constexpr Platform::ProcessTarget TARGET{.pid = 100, .startTimeTicks = 4242};

class MockThreadsReader final : public Platform::IProcessThreadsReader
{
  public:
    ThreadsReadResult result;
    std::atomic<int> reads{0};

    [[nodiscard]] bool hasThreads() const override
    {
        return true;
    }
    [[nodiscard]] ThreadsReadResult readThreads(const Platform::ProcessTarget& /*target*/) override
    {
        ++reads;
        return result;
    }
};

[[nodiscard]] ThreadsReadResult threads(std::vector<ProcessThread> list)
{
    return {.status = ThreadsReadStatus::Ok, .threads = std::move(list), .detail = {}, .ticksPerSecond = 100};
}

[[nodiscard]] ProcessThread thread(std::int32_t tid, const char* name, std::uint64_t ticks, std::uint64_t start = 10)
{
    return {.tid = tid, .name = name, .state = 'S', .userTicks = ticks, .systemTicks = 0, .nice = 0, .startTicks = start};
}

TEST(ProcessThreadsViewTest, CpuPercentIsAShareOfTheWholeMachine)
{
    // 50 ticks at 100 a second is 0.5 s of CPU in 1 s, on 2 CPUs: 25 %.
    EXPECT_DOUBLE_EQ(Detail::threadCpuPercent(50, 100, 1.0, 2).value_or(-1.0), 25.0);
    EXPECT_DOUBLE_EQ(Detail::threadCpuPercent(1000, 100, 1.0, 1).value_or(-1.0), 100.0); // clamped
    EXPECT_FALSE(Detail::threadCpuPercent(5, 100, 0.0, 1).has_value());
    EXPECT_FALSE(Detail::threadCpuPercent(5, 0, 1.0, 1).has_value());
}

TEST(ProcessThreadsViewTest, CpuPercentNeedsTwoReadsOfTheSameThread)
{
    ProcessThreadsView view(2);
    view.applyResult(threads({thread(100, "main", 100), thread(101, "worker", 0)}), 10.0);
    ASSERT_EQ(view.rows().size(), 2U);
    for (const ProcessThreadsView::Row& row : view.rows())
    {
        EXPECT_FALSE(row.cpuPercent.has_value()) << row.thread.name; // the first read: the dash
        EXPECT_EQ(row.cpu, Detail::THREAD_CPU_UNKNOWN);
    }

    // 2 s later: main used 0 ticks more, worker 200 (2 s of CPU on 2 CPUs: 50 %); TID 102 is new, and
    // TID 100 now names a thread that started later (a reused TID): neither has a CPU % yet.
    view.applyResult(threads({thread(100, "main", 5, 99), thread(101, "worker", 200), thread(102, "new", 7)}), 12.0);
    ASSERT_EQ(view.rows().size(), 3U);
    // Sorted by CPU %, highest first, by default.
    EXPECT_EQ(view.rows()[0].thread.tid, 101);
    EXPECT_DOUBLE_EQ(view.rows()[0].cpuPercent.value_or(-1.0), 50.0);
    EXPECT_FALSE(view.rows()[1].cpuPercent.has_value());
    EXPECT_FALSE(view.rows()[2].cpuPercent.has_value());
}

TEST(ProcessThreadsViewTest, ReadsOnlyWhileOpenAtTheSettingsInterval)
{
    ProcessThreadsView view(1);
    MockThreadsReader reader;
    reader.result = threads({thread(100, "main", 1)});

    EXPECT_FALSE(view.update(&reader, TARGET, 10.0F, 1000, 0.0)); // never drawn open: never read
    view.markDrawnOpen();
    EXPECT_TRUE(view.update(&reader, TARGET, 0.0F, 1000, 0.0));
    view.finishPendingRead(TARGET, 0.0);
    EXPECT_TRUE(view.hasRead());
    view.markDrawnOpen();
    EXPECT_FALSE(view.update(&reader, TARGET, 0.5F, 1000, 0.5)); // not yet due at a 1 s interval
    view.markDrawnOpen();
    EXPECT_TRUE(view.update(&reader, TARGET, 0.6F, 1000, 1.1));
    view.finishPendingRead(TARGET, 1.1);
    // A 5 s interval chosen in Settings: the next read waits for it.
    view.markDrawnOpen();
    EXPECT_FALSE(view.update(&reader, TARGET, 2.0F, 5000, 3.1));
    view.markDrawnOpen();
    EXPECT_TRUE(view.update(&reader, TARGET, 3.0F, 5000, 6.1));
    view.finishPendingRead(TARGET, 6.1);
    EXPECT_EQ(reader.reads.load(), 3);

    view.onSelectionChanged();
    EXPECT_FALSE(view.hasRead());
    EXPECT_TRUE(view.rows().empty());
}

TEST(ProcessThreadsViewTest, SortAndFilterByNameOrTid)
{
    ProcessThreadsView view(1);
    view.applyResult(threads({thread(300, "gmain", 1), thread(100, "server", 2), thread(205, "pool-2", 3)}), 0.0);
    view.setSort(Detail::ThreadsColumn::Tid, true);
    ASSERT_EQ(view.rows().size(), 3U);
    EXPECT_EQ(view.rows()[0].thread.tid, 100);
    EXPECT_EQ(view.rows()[2].thread.tid, 300);
    view.setSort(Detail::ThreadsColumn::Name, true);
    EXPECT_EQ(view.rows()[0].thread.name, "gmain");

    view.setFilter("POOL");
    ASSERT_EQ(view.filteredRows().size(), 1U);
    EXPECT_EQ(view.rows()[view.filteredRows()[0]].thread.tid, 205);
    view.setFilter("30");
    ASSERT_EQ(view.filteredRows().size(), 1U);
    EXPECT_EQ(view.rows()[view.filteredRows()[0]].thread.tid, 300);
}

TEST(ProcessThreadsViewTest, AFailedReadShowsItsStatusAndForgetsTheCpuTimes)
{
    ProcessThreadsView view(1);
    view.applyResult(threads({thread(100, "main", 1)}), 0.0);
    view.applyResult({.status = ThreadsReadStatus::PermissionDenied, .threads = {}, .detail = {}}, 1.0);
    EXPECT_EQ(view.status(), ThreadsReadStatus::PermissionDenied);
    EXPECT_TRUE(view.rows().empty());
    EXPECT_FALSE(Detail::threadsStatusText(ThreadsReadStatus::PermissionDenied).empty());
    EXPECT_TRUE(Detail::threadsStatusText(ThreadsReadStatus::Ok).empty());
    // The next good read starts CPU % afresh.
    view.applyResult(threads({thread(100, "main", 50)}), 2.0);
    EXPECT_FALSE(view.rows()[0].cpuPercent.has_value());
}

TEST(ProcessThreadsViewRenderTest, DrawsTheCountAndEachThread)
{
    ImGuiContext* context = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(1280.0F, 720.0F);
    io.DeltaTime = 1.0F / 60.0F;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    io.Fonts->AddFontDefault();

    ProcessThreadsView view(1);
    view.applyResult(threads({thread(100, "server", 1), thread(105, "pool-1", 2)}), 0.0);
    std::string captured;
    for (int frame = 0; frame < 2; ++frame)
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("Details", nullptr, ImGuiWindowFlags_NoDecoration);
        ImGui::SetNextItemOpen(true);
        ImGui::LogToBuffer();
        view.render(true);
        captured = GImGui->LogBuffer.c_str();
        ImGui::LogFinish();
        ImGui::End();
        ImGui::Render();
    }
    EXPECT_TRUE(captured.contains("Threads (2)")) << captured;
    EXPECT_TRUE(captured.contains("server")) << captured;
    EXPECT_TRUE(captured.contains("pool-1")) << captured;
    EXPECT_TRUE(captured.contains("Sleeping")) << captured;
    ImGui::DestroyContext(context);
}

} // namespace
} // namespace App
