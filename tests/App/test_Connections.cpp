// The Connections tab (#3066): its model, the table's rows, filter and sort, and the panel on display
// (sampling only while shown; a row click selects its process).

#include "App/Panels/ConnectionsPanel.h"
#include "App/Panels/ConnectionsView.h"
#include "Core/ApplicationEvents.h"
#include "Core/Event.h"
#include "Domain/ConnectionsModel.h"
#include "Platform/IProcessConnections.h"
#include "Platform/ISystemConnections.h"

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <atomic>
#include <cfloat>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace App
{
namespace
{

using Platform::ConnectionFamily;
using Platform::ConnectionProtocol;
using Platform::ConnectionState;
using Platform::SystemConnection;

[[nodiscard]] SystemConnection socket(std::int32_t pid,
                                      const char* name,
                                      ConnectionProtocol protocol,
                                      std::array<std::uint8_t, 4> local,
                                      std::uint16_t localPort,
                                      std::array<std::uint8_t, 4> remote,
                                      std::uint16_t remotePort,
                                      ConnectionState state)
{
    SystemConnection c;
    c.pid = pid;
    c.startTimeTicks = pid == 0 ? 0 : 1000U + static_cast<std::uint64_t>(pid);
    c.processName = name;
    c.connection.protocol = protocol;
    c.connection.family = ConnectionFamily::IPv4;
    std::ranges::copy(local, c.connection.local.address.begin());
    c.connection.local.port = localPort;
    std::ranges::copy(remote, c.connection.remote.address.begin());
    c.connection.remote.port = remotePort;
    c.connection.state = state;
    return c;
}

[[nodiscard]] std::vector<SystemConnection> sampleSockets()
{
    return {socket(0, "", ConnectionProtocol::Tcp, {10, 0, 0, 5}, 52145, {142, 250, 80, 46}, 443, ConnectionState::TimeWait),
            socket(300, "firefox", ConnectionProtocol::Tcp, {10, 0, 0, 5}, 52144, {142, 250, 80, 46}, 443, ConnectionState::Established),
            socket(100, "cupsd", ConnectionProtocol::Tcp, {127, 0, 0, 1}, 631, {0, 0, 0, 0}, 0, ConnectionState::Listen),
            socket(200, "avahi-daemon", ConnectionProtocol::Udp, {0, 0, 0, 0}, 5353, {0, 0, 0, 0}, 0, ConnectionState::Unknown)};
}

class FakeReader final : public Platform::ISystemConnectionsReader
{
  public:
    explicit FakeReader(std::shared_ptr<std::atomic<int>> reads, bool has = true) : m_Reads(std::move(reads)), m_Has(has)
    {}
    [[nodiscard]] bool hasConnections() const override
    {
        return m_Has;
    }
    [[nodiscard]] Platform::SystemConnectionsRead read() override
    {
        ++*m_Reads;
        return {.status = Platform::SystemConnectionsStatus::Ok, .connections = sampleSockets(), .detail = {}};
    }

  private:
    std::shared_ptr<std::atomic<int>> m_Reads;
    bool m_Has = true;
};

TEST(ConnectionsModelTest, PublishesByHolderNameWithHolderlessSocketsLast)
{
    auto reads = std::make_shared<std::atomic<int>>(0);
    Domain::ConnectionsModel model(std::make_unique<FakeReader>(reads));
    EXPECT_TRUE(model.hasConnections());
    EXPECT_EQ(model.publication()->version, 0U);
    model.sample();
    const auto pub = model.publication();
    ASSERT_EQ(pub->connections.size(), 4U);
    EXPECT_EQ(pub->connections[0].processName, "avahi-daemon");
    EXPECT_EQ(pub->connections[1].processName, "cupsd");
    EXPECT_EQ(pub->connections[2].processName, "firefox");
    EXPECT_EQ(pub->connections[3].pid, 0);
    EXPECT_EQ(pub->version, 1U);
}

TEST(ConnectionsViewTest, RowsFormatEndpointsStateAndHolder)
{
    Domain::ConnectionsPublication pub;
    pub.version = 1;
    pub.connections = sampleSockets();
    const std::vector<ConnectionRow> rows = buildConnectionRows(pub);
    ASSERT_EQ(rows.size(), 4U);
    EXPECT_EQ(rows[0].process, "Unknown");
    EXPECT_TRUE(rows[0].pid.empty());
    EXPECT_EQ(rows[1].process, "firefox");
    EXPECT_EQ(rows[1].pid, "300");
    EXPECT_EQ(rows[1].local, "10.0.0.5:52144");
    EXPECT_EQ(rows[1].remote, "142.250.80.46:443");
    EXPECT_EQ(rows[2].remote, "*");     // listening: no remote end
    EXPECT_EQ(rows[2].state, "LISTEN"); // netstat's names, as Process Details' Connections section shows them
}

TEST(ConnectionsViewTest, FilterMatchesProcessPidAddressOrStateAndSortOrders)
{
    Domain::ConnectionsPublication pub;
    pub.version = 1;
    pub.connections = sampleSockets();
    const std::vector<ConnectionRow> rows = buildConnectionRows(pub);
    EXPECT_EQ(orderConnectionRows(rows, "FIREFOX", ConnectionsColumn::Process, true).size(), 1U);
    EXPECT_EQ(orderConnectionRows(rows, "142.250", ConnectionsColumn::Process, true).size(), 2U);
    EXPECT_EQ(orderConnectionRows(rows, ":631", ConnectionsColumn::Process, true).size(), 1U);
    EXPECT_EQ(orderConnectionRows(rows, "listen", ConnectionsColumn::Process, true).size(), 1U);
    EXPECT_EQ(orderConnectionRows(rows, "", ConnectionsColumn::Process, true).size(), 4U);

    const std::vector<std::size_t> byLocal = orderConnectionRows(rows, "", ConnectionsColumn::Local, true);
    ASSERT_EQ(byLocal.size(), 4U);
    EXPECT_EQ(rows[byLocal.front()].local, "0.0.0.0:5353"); // numerically first
    const std::vector<std::size_t> byPidDescending = orderConnectionRows(rows, "", ConnectionsColumn::Pid, false);
    EXPECT_EQ(rows[byPidDescending.front()].pid, "300");
}

TEST(ConnectionsViewTest, ASocketKeepsItsSelectionKeyAcrossReads)
{
    const std::vector<SystemConnection> a = sampleSockets();
    const std::vector<SystemConnection> b = sampleSockets();
    EXPECT_EQ(selectionKey(a[1]), selectionKey(b[1]));
    EXPECT_NE(selectionKey(a[1]), selectionKey(a[0])); // another port
}

// ---- The panel, on display -------------------------------------------------------------------------

class ConnectionsPanelRenderTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Context = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.DisplaySize = ImVec2(1280.0F, 720.0F);
        io.DeltaTime = 1.0F / 60.0F;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        io.Fonts->AddFontDefault();
    }
    void TearDown() override
    {
        ImGui::DestroyContext(m_Context);
    }

    static std::string frame(Panel& panel, ImVec2* firstRow = nullptr)
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        ImGui::Begin("Shell", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
        ImGui::LogToBuffer();
        panel.renderContent();
        std::string captured = GImGui->LogBuffer.c_str();
        ImGui::LogFinish();
        if (firstRow != nullptr)
        {
            if (const ImGuiTable* table = ImGui::TableFindByID(ImGui::GetID("##ConnectionsTable")); table != nullptr)
            {
                const float rowHeight = ImGui::GetTextLineHeight() + (ImGui::GetStyle().CellPadding.y * 2.0F);
                *firstRow = ImVec2((table->Columns[0].MinX + table->Columns[0].MaxX) * 0.5F, table->OuterRect.Min.y + (rowHeight * 1.5F));
            }
        }
        ImGui::End();
        ImGui::Render();
        return captured;
    }

    static std::string frameUntil(Panel& panel, const std::string& needle)
    {
        const auto giveUp = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        std::string text = frame(panel);
        while (!text.contains(needle) && std::chrono::steady_clock::now() < giveUp)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            text = frame(panel);
        }
        return text;
    }

    static void show(Panel& panel, const char* tab)
    {
        Core::ActiveTabChangedEvent event(tab);
        panel.onEvent(event);
    }

  private:
    ImGuiContext* m_Context = nullptr;
};

TEST_F(ConnectionsPanelRenderTest, SamplesOnlyOnceShownAndAClickSelectsTheHolder)
{
    auto reads = std::make_shared<std::atomic<int>>(0);
    std::vector<std::int32_t> selected;
    ConnectionsPanel panel([reads] { return std::make_unique<FakeReader>(reads); },
                           [&selected](Core::Event& event)
                           {
                               if (auto* e = dynamic_cast<Core::ProcessSelectedEvent*>(&event))
                               {
                                   selected.push_back(e->getPid());
                               }
                           });
    panel.onAttach();
    EXPECT_TRUE(frame(panel).contains("Reading connections..."));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_EQ(reads->load(), 0) << "sampled with the tab hidden";

    show(panel, "Connections");
    const std::string text = frameUntil(panel, "firefox");
    EXPECT_TRUE(text.contains("142.250.80.46:443")) << text;
    EXPECT_TRUE(text.contains("4 of 4 connections")) << text;
    EXPECT_GE(reads->load(), 1);

    // The first row (by process name: avahi-daemon, PID 200) clicked: its process is selected.
    ImVec2 row(-1.0F, -1.0F);
    static_cast<void>(frame(panel, &row));
    ASSERT_GT(row.y, 0.0F);
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(row.x, row.y);
    static_cast<void>(frame(panel));
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    static_cast<void>(frame(panel));
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    static_cast<void>(frame(panel));
    io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
    static_cast<void>(frame(panel));
    ASSERT_FALSE(selected.empty());
    EXPECT_EQ(selected.back(), 200);

    panel.onDetach();
    EXPECT_TRUE(frame(panel).empty());
}

TEST_F(ConnectionsPanelRenderTest, AnUnsupportedReaderSaysSoAndNeverSamples)
{
    auto reads = std::make_shared<std::atomic<int>>(0);
    ConnectionsPanel panel([reads] { return std::make_unique<FakeReader>(reads, false); }, [](Core::Event&) {});
    panel.onAttach();
    show(panel, "Connections");
    EXPECT_TRUE(frame(panel).contains("Connections aren't available"));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_EQ(reads->load(), 0);
    panel.onDetach();
}

} // namespace
} // namespace App
