/// @file test_ProcessIoPriorityViewRender.cpp
/// @brief The I/O priority control's real ImGui render, headless (#803), drawn as Process Details draws
/// it: by ProcessPriorityView, under the nice control. It is hidden (and nothing is read) without the
/// capability, as on Windows; it reads the shown process's I/O priority; Apply is disabled until an
/// edit; a click on Apply sets the edit on the selected process once; an edit made for another process
/// is dropped before it can be applied; and a failed apply shows the error line.

#include "App/Panels/ProcessPriorityView.h"
#include "Mocks/MockProbes.h"
#include "Platform/IProcessActions.h"

#include <gtest/gtest.h>
#include <imgui.h>

#include <cmath>
#include <cstdint>
#include <functional>
#include <optional>

namespace App
{
namespace
{

using Platform::IoPriority;
using Platform::IoPriorityClass;

constexpr Platform::ProcessTarget TARGET_A{.pid = 1001, .startTimeTicks = 5000};
constexpr Platform::ProcessTarget TARGET_B{.pid = 2002, .startTimeTicks = 6000};

constexpr Platform::ProcessActionCapabilities NICE_ONLY{.canSetPriority = true};
constexpr Platform::ProcessActionCapabilities NICE_AND_IO{.canSetPriority = true, .canSetIoPriority = true};

constexpr IoPriority BEST_EFFORT_4{.ioClass = IoPriorityClass::BestEffort, .level = 4};
constexpr IoPriority IDLE{.ioClass = IoPriorityClass::Idle, .level = 0};
constexpr std::optional<std::int32_t> NICE = 0;

class ProcessIoPriorityViewRenderTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Context = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.DisplaySize = ImVec2(1600.0F, 1000.0F);
        io.DeltaTime = 1.0F / 60.0F;
        // No renderer: let ImGui build and own the font atlas itself.
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        io.Fonts->AddFontDefault();
    }

    void TearDown() override
    {
        ImGui::DestroyContext(m_Context);
    }

    /// One frame of a window like the Process Details pane, running @p body inside it.
    static void runFrame(const std::function<void()>& body, float windowWidth = 1600.0F)
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
        ImGui::SetNextWindowSize(ImVec2(windowWidth, 1000.0F));
        ImGui::Begin("Details", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
        body();
        ImGui::End();
        ImGui::Render();
    }

    /// Renders @p view for @p target and returns the centre of the I/O Apply button: the last item
    /// drawn while neither error line shows.
    static ImVec2
    renderAndFindIoApply(ProcessPriorityView& view, TestMocks::MockProcessActions& mock, const Platform::ProcessTarget& target)
    {
        ImVec2 centre;
        runFrame(
            [&]
            {
                view.render(&mock, NICE_AND_IO, NICE, target);
                const ImVec2 min = ImGui::GetItemRectMin();
                const ImVec2 max = ImGui::GetItemRectMax();
                centre = ImVec2((min.x + max.x) * 0.5F, (min.y + max.y) * 0.5F);
            });
        return centre;
    }

    /// Clicks at @p pos over three frames (move, press, release) while @p body draws.
    static void click(ImVec2 pos, const std::function<void()>& body)
    {
        ImGuiIO& io = ImGui::GetIO();
        io.AddMousePosEvent(pos.x, pos.y);
        runFrame(body);
        io.AddMouseButtonEvent(0, true);
        runFrame(body);
        io.AddMouseButtonEvent(0, false);
        runFrame(body);
    }

  private:
    ImGuiContext* m_Context = nullptr;
};

TEST_F(ProcessIoPriorityViewRenderTest, HiddenAndNotReadWithoutTheCapability)
{
    TestMocks::MockProcessActions mock;
    ProcessPriorityView view;
    runFrame([&] { view.render(&mock, NICE_ONLY, NICE, TARGET_A); });
    EXPECT_EQ(mock.getIoPriorityCount(), 0);
    EXPECT_FALSE(view.ioPriorityView().currentIoPriority().has_value());
}

TEST_F(ProcessIoPriorityViewRenderTest, RenderReadsAndShowsTheProcessValue)
{
    TestMocks::MockProcessActions mock;
    mock.setIoPriorityReadResult(IDLE);
    ProcessPriorityView view;
    runFrame([&] { view.render(&mock, NICE_AND_IO, NICE, TARGET_A); });
    EXPECT_EQ(mock.getIoPriorityCount(), 1);
    EXPECT_EQ(view.ioPriorityView().shownIoPriority(), IDLE);
    EXPECT_FALSE(view.ioPriorityView().hasPendingEdit());
}

TEST_F(ProcessIoPriorityViewRenderTest, ApplyIsDisabledUntilAnEdit)
{
    TestMocks::MockProcessActions mock;
    mock.setIoPriorityReadResult(BEST_EFFORT_4);
    ProcessPriorityView view;
    const ImVec2 apply = renderAndFindIoApply(view, mock, TARGET_A);
    click(apply, [&] { view.render(&mock, NICE_AND_IO, NICE, TARGET_A); });
    EXPECT_EQ(mock.setIoPriorityCount(), 0);
    EXPECT_EQ(mock.setPriorityCount(), 0);
}

TEST_F(ProcessIoPriorityViewRenderTest, ClickingApplySetsTheEditOnTheSelectedProcessOnce)
{
    TestMocks::MockProcessActions mock;
    mock.setIoPriorityReadResult(BEST_EFFORT_4);
    ProcessPriorityView view;
    runFrame([&] { view.render(&mock, NICE_AND_IO, NICE, TARGET_A); }); // Reads the current value
    view.ioPriorityView().editIoPriority(IDLE, TARGET_A);
    // Found with the edit shown: Idle has no level slider, so Apply sits further left than for Best-effort.
    const ImVec2 apply = renderAndFindIoApply(view, mock, TARGET_A);

    click(apply, [&] { view.render(&mock, NICE_AND_IO, NICE, TARGET_A); });
    ASSERT_EQ(mock.setIoPriorityCount(), 1);
    EXPECT_EQ(mock.setPriorityCount(), 0); // The nice control's Apply is a different button
    EXPECT_EQ(mock.lastTarget().pid, TARGET_A.pid);
    EXPECT_EQ(mock.lastTarget().startTimeTicks, TARGET_A.startTimeTicks);
    EXPECT_EQ(mock.lastSetIoPriority(), IDLE);

    click(apply, [&] { view.render(&mock, NICE_AND_IO, NICE, TARGET_A); }); // Disabled again
    EXPECT_EQ(mock.setIoPriorityCount(), 1);
}

TEST_F(ProcessIoPriorityViewRenderTest, AnEditForAnotherProcessIsDroppedBeforeApply)
{
    TestMocks::MockProcessActions mock;
    mock.setIoPriorityReadResult(BEST_EFFORT_4);
    ProcessPriorityView view;
    // Found before the edit, with Best-effort's level slider shown: where Apply is once B (also
    // Best-effort) is shown and the edit for A dropped.
    const ImVec2 apply = renderAndFindIoApply(view, mock, TARGET_A);
    view.ioPriorityView().editIoPriority(IDLE, TARGET_A);

    click(apply, [&] { view.render(&mock, NICE_AND_IO, NICE, TARGET_B); });
    EXPECT_EQ(mock.setIoPriorityCount(), 0);
    EXPECT_FALSE(view.ioPriorityView().hasPendingEdit());
}

TEST_F(ProcessIoPriorityViewRenderTest, AFailedApplyShowsTheErrorLine)
{
    TestMocks::MockProcessActions mock;
    mock.setIoPriorityReadResult(BEST_EFFORT_4);
    mock.setIoPriorityResult(Platform::ProcessActionResult::error("Permission denied"));
    ProcessPriorityView view;
    const ImVec2 apply = renderAndFindIoApply(view, mock, TARGET_A);
    view.ioPriorityView().editIoPriority({.ioClass = IoPriorityClass::Realtime, .level = 0}, TARGET_A);

    click(apply, [&] { view.render(&mock, NICE_AND_IO, NICE, TARGET_A); });
    EXPECT_EQ(mock.setIoPriorityCount(), 1);
    EXPECT_EQ(view.ioPriorityView().error(), "Permission denied");
    EXPECT_EQ(view.ioPriorityView().shownIoPriority(), BEST_EFFORT_4);
    EXPECT_TRUE(view.error().empty()); // The nice control's error line is its own
}

// As wide as its label, as the nice control's Apply and Terminate and Kill are (#1537): its old em
// floor made it a wide muted bar while disabled.
TEST_F(ProcessIoPriorityViewRenderTest, ApplyIsAsWideAsItsLabel)
{
    TestMocks::MockProcessActions mock;
    mock.setIoPriorityReadResult(BEST_EFFORT_4);
    ProcessPriorityView view;
    float applyWidth = 0.0F;
    float labelWidth = 0.0F;
    runFrame(
        [&]
        {
            view.render(&mock, NICE_AND_IO, NICE, TARGET_A);
            applyWidth = ImGui::GetItemRectSize().x; // The I/O Apply is the last item while no error line shows
            labelWidth = ImGui::CalcTextSize("Apply").x + (2.0F * ImGui::GetStyle().FramePadding.x);
        });
    EXPECT_GT(applyWidth, 0.0F);
    EXPECT_LE(applyWidth, std::ceil(labelWidth));
}

TEST_F(ProcessIoPriorityViewRenderTest, ApplyStaysInsideANarrowPanel)
{
    // Process Details does not scroll horizontally: at a narrow width the row shrinks, then wraps,
    // so Apply is never clipped off the right edge.
    TestMocks::MockProcessActions mock;
    mock.setIoPriorityReadResult(BEST_EFFORT_4); // Best-effort: combo, slider and Apply on the row
    ProcessPriorityView view;
    for (const float width : {420.0F, 300.0F, 160.0F})
    {
        SCOPED_TRACE(width);
        float applyRight = 0.0F;
        float windowRight = 0.0F;
        const auto body = [&]
        {
            view.render(&mock, NICE_AND_IO, NICE, TARGET_A);
            applyRight = ImGui::GetItemRectMax().x;
            windowRight = ImGui::GetWindowPos().x + ImGui::GetWindowSize().x;
        };
        runFrame(body, width);
        runFrame(body, width);
        EXPECT_LE(applyRight, windowRight);
    }
}

} // namespace
} // namespace App
