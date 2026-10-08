/// @file test_ProcessConnectionsViewRender.cpp
/// @brief The Connections section's real ImGui render, headless (#799). The text it hands ImGui is
/// captured through ImGui's own logging (LogToBuffer(), which also turns clipping off), so the tests
/// can say what was drawn: nothing where the platform has no support, nothing read while collapsed,
/// "Reading..." until the first read, the count, headers and rows after it, each failed read's status
/// line instead of an empty table, a header click re-sorting the rows, and a table that stays within a
/// narrow pane.

#include "App/Panels/ProcessConnectionsView.h"
#include "Mocks/MockProbes.h"
#include "Platform/IProcessActions.h"
#include "Platform/IProcessConnections.h"

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <array>
#include <cfloat>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <string>

namespace App
{
namespace
{

using Platform::ConnectionFamily;
using Platform::ConnectionProtocol;
using Platform::ConnectionState;
using Platform::ProcessConnection;

constexpr Platform::ProcessTarget TARGET{.pid = 4242, .startTimeTicks = 777};
constexpr float WINDOW_WIDTH = 1200.0F;
constexpr float WINDOW_HEIGHT = 800.0F;

[[nodiscard]] Platform::ConnectionsReadResult sampleConnections()
{
    const auto tcp4 = [](std::uint8_t lastOctet, std::uint16_t localPort, std::uint16_t remotePort, ConnectionState state)
    {
        return ProcessConnection{.protocol = ConnectionProtocol::Tcp,
                                 .family = ConnectionFamily::IPv4,
                                 .local = {.address = {10, 0, 0, 5}, .port = localPort},
                                 .remote = {.address = {93, 184, 216, lastOctet}, .port = remotePort},
                                 .state = state};
    };
    return {.status = Platform::ConnectionsReadStatus::Ok,
            .connections = {tcp4(34, 55026, 443, ConnectionState::Established),
                            ProcessConnection{.protocol = ConnectionProtocol::Tcp,
                                              .family = ConnectionFamily::IPv6,
                                              .local = {.address = {}, .port = 22},
                                              .remote = {},
                                              .state = ConnectionState::Listen},
                            tcp4(35, 55027, 80, ConnectionState::CloseWait)}};
}

class ProcessConnectionsViewRenderTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Context = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.DisplaySize = ImVec2(WINDOW_WIDTH, WINDOW_HEIGHT);
        io.DeltaTime = 1.0F / 60.0F;
        // No renderer: let ImGui build and own the font atlas itself.
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        io.Fonts->AddFontDefault();
    }

    void TearDown() override
    {
        ImGui::DestroyContext(m_Context);
    }

    /// One frame of a window like the Process Details pane, @p width wide, running @p body inside it.
    static void runFrame(const std::function<void()>& body, float width = WINDOW_WIDTH)
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
        ImGui::SetNextWindowSize(ImVec2(width, WINDOW_HEIGHT));
        ImGui::Begin("Details", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
        body();
        ImGui::End();
        ImGui::Render();
    }

    /// Renders one frame of @p view with its header held @p open, and returns all the text it drew.
    static std::string renderAndCapture(ProcessConnectionsView& view, bool hasConnections, bool open = true)
    {
        std::string captured;
        runFrame(
            [&]
            {
                ImGui::LogToBuffer();
                ImGui::SetNextItemOpen(open, ImGuiCond_Always);
                view.render(hasConnections);
                captured = GImGui->LogBuffer.c_str();
                ImGui::LogFinish();
            });
        return captured;
    }

    /// Runs frames as the panel does -- update, then render open -- until the view has read @p reader.
    static void openAndRead(ProcessConnectionsView& view, TestMocks::MockProcessConnectionsReader& reader)
    {
        for (int i = 0; i < 3 && !view.hasRead(); ++i)
        {
            static_cast<void>(view.update(&reader, TARGET, 1.0F / 60.0F));
            static_cast<void>(renderAndCapture(view, true));
        }
        ASSERT_TRUE(view.hasRead());
    }

    /// The table ImGui keeps for "##ConnectionsTable" after a frame that drew it open in a pane
    /// @p width wide: where a column's header is, and how far the table reaches.
    struct TableGeometry
    {
        bool found = false;
        float headerCentreY = 0.0F;
        std::array<float, 4> columnCentreX{};
        float outerMaxX = 0.0F;
    };

    static TableGeometry tableGeometry(ProcessConnectionsView& view, float width = WINDOW_WIDTH)
    {
        TableGeometry geometry;
        runFrame(
            [&]
            {
                ImGui::SetNextItemOpen(true, ImGuiCond_Always);
                view.render(true);
                const ImGuiTable* table = ImGui::TableFindByID(ImGui::GetID("##ConnectionsTable"));
                if (table != nullptr)
                {
                    geometry.found = true;
                    const float headerHeight = ImGui::GetTextLineHeight() + (ImGui::GetStyle().CellPadding.y * 2.0F);
                    geometry.headerCentreY = table->OuterRect.Min.y + (headerHeight * 0.5F);
                    for (int column = 0; column < 4; ++column)
                    {
                        geometry.columnCentreX.at(static_cast<std::size_t>(column)) =
                            (table->Columns[column].MinX + table->Columns[column].MaxX) * 0.5F;
                    }
                    geometry.outerMaxX = table->OuterRect.Max.x;
                }
            },
            width);
        return geometry;
    }

    /// A left click at @p pos over two frames (press, release), the section drawn open throughout.
    static void clickAt(ProcessConnectionsView& view, ImVec2 pos)
    {
        ImGuiIO& io = ImGui::GetIO();
        io.AddMousePosEvent(pos.x, pos.y);
        static_cast<void>(renderAndCapture(view, true)); // hover first, so the press lands on the header
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        static_cast<void>(renderAndCapture(view, true));
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        static_cast<void>(renderAndCapture(view, true));
        io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
        static_cast<void>(renderAndCapture(view, true)); // the frame that applies the new sort specs
    }

  private:
    ImGuiContext* m_Context = nullptr;
};

TEST_F(ProcessConnectionsViewRenderTest, DrawsNothingWithoutConnectionsSupport)
{
    ProcessConnectionsView view;
    view.applyResult(sampleConnections());
    const std::string text = renderAndCapture(view, false);
    EXPECT_FALSE(text.contains("Connections"));
    EXPECT_FALSE(text.contains("ESTABLISHED"));

    // And, never drawn open, it is never read.
    TestMocks::MockProcessConnectionsReader reader;
    EXPECT_FALSE(view.update(&reader, TARGET, 10.0F));
    EXPECT_EQ(reader.readCount(), 0);
}

TEST_F(ProcessConnectionsViewRenderTest, CollapsedHeaderDrawsAndReadsNothing)
{
    ProcessConnectionsView view;
    TestMocks::MockProcessConnectionsReader reader;
    reader.setResult(sampleConnections());
    for (int i = 0; i < 3; ++i)
    {
        static_cast<void>(view.update(&reader, TARGET, 10.0F));
        const std::string text = renderAndCapture(view, true, false);
        EXPECT_TRUE(text.contains("Connections"));
        EXPECT_FALSE(text.contains("ESTABLISHED"));
    }
    EXPECT_EQ(reader.readCount(), 0);
}

TEST_F(ProcessConnectionsViewRenderTest, ShowsReadingUntilTheFirstRead)
{
    ProcessConnectionsView view;
    EXPECT_TRUE(renderAndCapture(view, true).contains("Reading..."));
}

TEST_F(ProcessConnectionsViewRenderTest, DrawsTheCountHeadersAndEveryRow)
{
    ProcessConnectionsView view;
    TestMocks::MockProcessConnectionsReader reader;
    reader.setResult(sampleConnections());
    openAndRead(view, reader);

    const std::string text = renderAndCapture(view, true);
    EXPECT_TRUE(text.contains("3 connections"));
    for (const char* header : {"PROTO", "LOCAL ADDRESS", "REMOTE ADDRESS", "STATE"})
    {
        EXPECT_TRUE(text.contains(header)) << header;
    }
    EXPECT_TRUE(text.contains("10.0.0.5:55026"));
    EXPECT_TRUE(text.contains("93.184.216.34:443"));
    EXPECT_TRUE(text.contains("ESTABLISHED"));
    EXPECT_TRUE(text.contains("TCP6"));
    EXPECT_TRUE(text.contains("[::]:22"));
    EXPECT_TRUE(text.contains("[::]:*"));
    EXPECT_TRUE(text.contains("LISTEN"));
    EXPECT_TRUE(text.contains("CLOSE_WAIT"));
}

TEST_F(ProcessConnectionsViewRenderTest, FailedReadsDrawTheirStatusInsteadOfAnEmptyTable)
{
    for (const auto status : {Platform::ConnectionsReadStatus::PermissionDenied,
                              Platform::ConnectionsReadStatus::ProcessExited,
                              Platform::ConnectionsReadStatus::IdentityUnknown,
                              Platform::ConnectionsReadStatus::Failed})
    {
        ProcessConnectionsView view;
        TestMocks::MockProcessConnectionsReader reader;
        reader.setResult({.status = status, .connections = {}});
        openAndRead(view, reader);
        const std::string text = renderAndCapture(view, true);
        EXPECT_TRUE(text.contains(std::string(Detail::connectionsStatusText(status))));
        EXPECT_FALSE(text.contains("LOCAL ADDRESS"));
    }

    ProcessConnectionsView none;
    TestMocks::MockProcessConnectionsReader reader;
    openAndRead(none, reader);
    EXPECT_TRUE(renderAndCapture(none, true).contains("No TCP or UDP sockets"));
}

TEST_F(ProcessConnectionsViewRenderTest, ClickingAColumnHeaderSortsByIt)
{
    ProcessConnectionsView view;
    TestMocks::MockProcessConnectionsReader reader;
    reader.setResult(sampleConnections());
    openAndRead(view, reader);
    static_cast<void>(renderAndCapture(view, true)); // the default sort specs, applied once
    EXPECT_EQ(view.sortColumn(), Detail::ConnectionsColumn::State);
    EXPECT_EQ(view.rows()[0].state, "ESTABLISHED");

    const TableGeometry geometry = tableGeometry(view);
    ASSERT_TRUE(geometry.found);
    clickAt(view, ImVec2(geometry.columnCentreX[2], geometry.headerCentreY)); // REMOTE ADDRESS
    EXPECT_EQ(view.sortColumn(), Detail::ConnectionsColumn::Remote);
    EXPECT_TRUE(view.sortAscending());
    EXPECT_EQ(view.rows()[0].remote, "93.184.216.34:443");
    EXPECT_EQ(view.rows()[2].remote, "[::]:*");

    clickAt(view, ImVec2(geometry.columnCentreX[2], geometry.headerCentreY)); // again: descending
    EXPECT_EQ(view.sortColumn(), Detail::ConnectionsColumn::Remote);
    EXPECT_FALSE(view.sortAscending());
    EXPECT_EQ(view.rows()[0].remote, "[::]:*");
}

TEST_F(ProcessConnectionsViewRenderTest, StaysWithinANarrowPane)
{
    constexpr float NARROW = 320.0F;
    ProcessConnectionsView view;
    view.applyResult(sampleConnections());
    const TableGeometry geometry = tableGeometry(view, NARROW);
    ASSERT_TRUE(geometry.found);
    EXPECT_LE(geometry.outerMaxX, NARROW) << "the columns shrink to the pane; long addresses clip, with a tooltip";
}

} // namespace
} // namespace App
