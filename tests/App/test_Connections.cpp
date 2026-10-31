// The Connections tab (#3066): its model, the table's rows, filter and sort, and the panel on display
// (sampling only while shown; a row click selects its process).

#include "App/Panels/ConnectionCloseControl.h"
#include "App/Panels/ConnectionsPanel.h"
#include "App/Panels/ConnectionsView.h"
#include "Core/ApplicationEvents.h"
#include "Core/Event.h"
#include "Domain/ConnectionsModel.h"
#include "Domain/ProcessConnections.h"
#include "Mocks/MockProbes.h"
#include "Platform/IProcessActions.h"
#include "Platform/IProcessConnections.h"
#include "Platform/ISystemConnections.h"

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <array>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace App
{
namespace
{

using Platform::ConnectionFamily;
using Platform::ConnectionProtocol;
using Platform::ConnectionState;
using Platform::SystemConnection;
using TestMocks::MockProcessActions;

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
    c.socketInode = pid == 0 ? 0 : 50000U + static_cast<std::uint64_t>(pid); // TIME_WAIT: no file holds it
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
    // #3186: the other columns sort too.
    EXPECT_EQ(rows[orderConnectionRows(rows, "", ConnectionsColumn::Protocol, false).front()].protocol, "UDP");
    EXPECT_EQ(rows[orderConnectionRows(rows, "", ConnectionsColumn::Remote, false).front()].remote, "142.250.80.46:443");
    EXPECT_EQ(rows[orderConnectionRows(rows, "", ConnectionsColumn::State, true).front()].state, "ESTABLISHED");
}

TEST(ConnectionsViewTest, ASocketKeepsItsSelectionKeyAcrossReads)
{
    const std::vector<SystemConnection> a = sampleSockets();
    const std::vector<SystemConnection> b = sampleSockets();
    EXPECT_EQ(selectionKey(a[1]), selectionKey(b[1]));
    EXPECT_NE(selectionKey(a[1]), selectionKey(a[0])); // another port
}

// ---- The panel, on display -------------------------------------------------------------------------

/// Reads back the sockets it was given, or a failure with @p detail when that is set.
class ListReader final : public Platform::ISystemConnectionsReader
{
  public:
    explicit ListReader(std::vector<SystemConnection> sockets, std::string failure = {})
        : m_Sockets(std::move(sockets)), m_Failure(std::move(failure))
    {}
    [[nodiscard]] bool hasConnections() const override
    {
        return true;
    }
    [[nodiscard]] Platform::SystemConnectionsRead read() override
    {
        if (!m_Failure.empty())
        {
            return {.status = Platform::SystemConnectionsStatus::Failed, .connections = {}, .detail = m_Failure};
        }
        return {.status = Platform::SystemConnectionsStatus::Ok, .connections = m_Sockets, .detail = {}};
    }

  private:
    std::vector<SystemConnection> m_Sockets;
    std::string m_Failure;
};

// #3186: a model needs a reader.
TEST(ConnectionsModelTest, ANullReaderIsRefused)
{
    EXPECT_THROW(Domain::ConnectionsModel(nullptr), std::invalid_argument);
}

// #3186: one holder name's sockets order by PID, then protocol, then local endpoint.
TEST(ConnectionsModelTest, SameNameSocketsOrderByPidProtocolThenLocalEndpoint)
{
    Domain::ConnectionsModel model(std::make_unique<ListReader>(std::vector<SystemConnection>{
        socket(9, "svc", ConnectionProtocol::Tcp, {10, 0, 0, 1}, 80, {0, 0, 0, 0}, 0, ConnectionState::Listen),
        socket(3, "svc", ConnectionProtocol::Udp, {10, 0, 0, 1}, 53, {0, 0, 0, 0}, 0, ConnectionState::Unknown),
        socket(3, "svc", ConnectionProtocol::Tcp, {10, 0, 0, 2}, 22, {0, 0, 0, 0}, 0, ConnectionState::Listen),
        socket(3, "svc", ConnectionProtocol::Tcp, {10, 0, 0, 1}, 22, {0, 0, 0, 0}, 0, ConnectionState::Listen)}));
    model.sample();
    const auto pub = model.publication();
    ASSERT_EQ(pub->connections.size(), 4U);
    EXPECT_EQ(pub->connections[0].pid, 3);
    EXPECT_EQ(pub->connections[0].connection.protocol, ConnectionProtocol::Tcp);
    EXPECT_EQ(pub->connections[0].connection.local.address[3], 1);
    EXPECT_EQ(pub->connections[1].connection.local.address[3], 2);
    EXPECT_EQ(pub->connections[2].connection.protocol, ConnectionProtocol::Udp);
    EXPECT_EQ(pub->connections[3].pid, 9);
}

// #3066: the first generation lights nothing; a socket that appears after it is tinted, fading out over
// NEW_SECONDS; one that goes is kept, marked closed, for CLOSED_SECONDS, unless it opens again.
TEST(ConnectionChangeTrackerTest, NewSocketsFadeAndClosedOnesStayAMoment)
{
    Domain::ConnectionsPublication first;
    first.version = 1;
    first.connections = sampleSockets();
    std::vector<ConnectionRow> rows = buildConnectionRows(first);
    ConnectionChangeTracker tracker;
    tracker.onGeneration(rows, 10.0);
    for (const ConnectionRow& row : rows)
    {
        EXPECT_FALSE(tracker.newAlpha(selectionKey(row.connection), 10.0).has_value());
    }

    // firefox's connection closes; sshd starts listening.
    Domain::ConnectionsPublication second = first;
    second.version = 2;
    second.connections.erase(second.connections.begin() + 1);
    second.connections.push_back(socket(400, "sshd", ConnectionProtocol::Tcp, {0, 0, 0, 0}, 22, {0, 0, 0, 0}, 0, ConnectionState::Listen));
    rows = buildConnectionRows(second);
    tracker.onGeneration(rows, 11.0);
    const std::uint64_t sshd = selectionKey(rows.back().connection);
    ASSERT_TRUE(tracker.newAlpha(sshd, 11.0).has_value());
    EXPECT_FLOAT_EQ(tracker.newAlpha(sshd, 11.0).value_or(0.0F), ConnectionChanges::NEW_MAX_ALPHA);
    EXPECT_LT(tracker.newAlpha(sshd, 13.5).value_or(1.0F), ConnectionChanges::NEW_MAX_ALPHA);
    const std::vector<ConnectionRow> closed = tracker.closedRows();
    ASSERT_EQ(closed.size(), 1U);
    EXPECT_TRUE(closed[0].closed);
    EXPECT_EQ(closed[0].process, "firefox");

    EXPECT_FALSE(tracker.expire(12.0)); // the closed row stays CLOSED_SECONDS
    EXPECT_TRUE(tracker.expire(11.0 + ConnectionChanges::CLOSED_SECONDS));
    EXPECT_TRUE(tracker.closedRows().empty());
    static_cast<void>(tracker.expire(11.0 + ConnectionChanges::NEW_SECONDS));
    EXPECT_FALSE(tracker.newAlpha(sshd, 11.0 + ConnectionChanges::NEW_SECONDS).has_value());

    // A socket that closes and opens again with the same endpoints is just live.
    tracker.onGeneration(buildConnectionRows(first), 20.0);
    tracker.onGeneration(rows, 21.0);
    tracker.onGeneration(buildConnectionRows(first), 22.0);
    for (const ConnectionRow& row : tracker.closedRows())
    {
        EXPECT_NE(row.process, "firefox");
    }
}

// #3066: the table's rows are the generation's sockets, then the closed ones still shown; the count
// leaves the closed ones out, and they go once CLOSED_SECONDS have passed.
TEST(ConnectionsViewTest, RowsHoldTheClosedSocketsUntilTheyExpire)
{
    ConnectionsViewState state;
    Domain::ConnectionsPublication pub;
    pub.version = 1;
    pub.connections = sampleSockets();
    refreshConnectionRows(pub, state, 1.0);
    EXPECT_EQ(state.rows.size(), 4U);
    EXPECT_EQ(state.liveCount, 4U);

    pub.version = 2;
    pub.connections.pop_back();
    refreshConnectionRows(pub, state, 2.0);
    EXPECT_EQ(state.liveCount, 3U);
    ASSERT_EQ(state.rows.size(), 4U);
    EXPECT_TRUE(state.rows.back().closed);
    EXPECT_EQ(state.visible.size(), 4U);

    refreshConnectionRows(pub, state, 2.0 + ConnectionChanges::CLOSED_SECONDS);
    EXPECT_EQ(state.rows.size(), 3U);
    EXPECT_EQ(state.visible.size(), 3U);
}

[[nodiscard]] SystemConnection withBytes(SystemConnection c, std::uint64_t sent, std::uint64_t received)
{
    c.bytesSent = sent;
    c.bytesReceived = received;
    c.hasByteCounters = true;
    return c;
}

// #3079: a rate needs two readings of the same socket's counters, going up; UDP (no counters), a new socket and
// a counter that went backwards have none.
TEST(ConnectionRatesTest, RatesComeFromTwoReadingsOfTheSameSocket)
{
    const SystemConnection tcp =
        socket(300, "firefox", ConnectionProtocol::Tcp, {10, 0, 0, 5}, 52144, {142, 250, 80, 46}, 443, ConnectionState::Established);
    const SystemConnection fresh =
        socket(301, "curl", ConnectionProtocol::Tcp, {10, 0, 0, 5}, 52200, {1, 1, 1, 1}, 443, ConnectionState::Established);
    const SystemConnection udp =
        socket(200, "avahi-daemon", ConnectionProtocol::Udp, {0, 0, 0, 0}, 5353, {0, 0, 0, 0}, 0, ConnectionState::Unknown);
    std::unordered_map<std::uint64_t, Domain::ConnectionBytes> previous;
    previous[Domain::Connections::socketKey(tcp.connection)] = {.sent = 1000, .received = 5000};
    const std::vector<SystemConnection> now{withBytes(tcp, 3000, 9000), withBytes(fresh, 10, 10), udp};
    const std::vector<Domain::ConnectionRate> rates = Domain::connectionRates(now, previous, 2.0);
    ASSERT_EQ(rates.size(), 3U);
    EXPECT_TRUE(rates[0].known);
    EXPECT_DOUBLE_EQ(rates[0].sentPerSec, 1000.0);
    EXPECT_DOUBLE_EQ(rates[0].receivedPerSec, 2000.0);
    EXPECT_FALSE(rates[1].known);                                                            // first reading
    EXPECT_FALSE(rates[2].known);                                                            // no counters
    EXPECT_FALSE(Domain::connectionRates({withBytes(tcp, 10, 10)}, previous, 2.0)[0].known); // went backwards
    EXPECT_FALSE(Domain::connectionRates(now, previous, 0.0)[0].known);                      // no time passed
}

// #3079: the model keeps each reading's counters for the next, and the view shows the rates, sortable.
TEST(ConnectionRatesTest, TheModelPublishesRatesFromItsSecondSampleAndTheViewSortsByThem)
{
    auto list = std::make_shared<std::vector<SystemConnection>>();
    class SharedReader final : public Platform::ISystemConnectionsReader
    {
      public:
        explicit SharedReader(std::shared_ptr<std::vector<SystemConnection>> sockets) : m_Sockets(std::move(sockets))
        {}
        [[nodiscard]] bool hasConnections() const override
        {
            return true;
        }
        [[nodiscard]] Platform::SystemConnectionsRead read() override
        {
            return {.status = Platform::SystemConnectionsStatus::Ok, .connections = *m_Sockets, .detail = {}};
        }

      private:
        std::shared_ptr<std::vector<SystemConnection>> m_Sockets;
    };
    const SystemConnection a =
        socket(300, "firefox", ConnectionProtocol::Tcp, {10, 0, 0, 5}, 52144, {142, 250, 80, 46}, 443, ConnectionState::Established);
    const SystemConnection b =
        socket(301, "curl", ConnectionProtocol::Tcp, {10, 0, 0, 5}, 52200, {1, 1, 1, 1}, 443, ConnectionState::Established);
    *list = {withBytes(a, 0, 0), withBytes(b, 0, 0)};
    Domain::ConnectionsModel model(std::make_unique<SharedReader>(list));
    model.sample();
    EXPECT_FALSE(model.publication()->rates.at(0).known);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    *list = {withBytes(a, 100, 100), withBytes(b, 100000, 100000)};
    model.sample();
    const auto pub = model.publication();
    ASSERT_EQ(pub->rates.size(), 2U);
    EXPECT_TRUE(pub->rates[0].known);
    EXPECT_TRUE(pub->rates[1].known);

    const std::vector<ConnectionRow> rows = buildConnectionRows(*pub);
    EXPECT_FALSE(rows[0].sent.empty());
    const std::vector<std::size_t> bySent = orderConnectionRows(rows, "", ConnectionsColumn::Sent, false);
    EXPECT_EQ(rows[bySent.front()].process, "curl"); // the busier socket first
}

// --- Close connection (#3066) ------------------------------------------------------------------------

/// @p connection as the table formats it.
[[nodiscard]] ConnectionRow rowFor(const SystemConnection& connection)
{
    Domain::ConnectionsPublication publication;
    publication.version = 1;
    publication.status = Platform::SystemConnectionsStatus::Ok;
    publication.connections = {connection};
    return buildConnectionRows(publication).front();
}

[[nodiscard]] SystemConnection firefoxToGoogle()
{
    return socket(300, "firefox", ConnectionProtocol::Tcp, {10, 0, 0, 5}, 52144, {142, 250, 80, 46}, 443, ConnectionState::Established);
}

TEST(ConnectionCloseTest, OnlyAHeldTcpConnectionWithAPeerCanBeClosed)
{
    EXPECT_EQ(closeConnectionRefusal(firefoxToGoogle()), "");
    for (const ConnectionState state : {ConnectionState::SynSent, ConnectionState::SynReceived, ConnectionState::CloseWait})
    {
        SystemConnection opening = firefoxToGoogle();
        opening.connection.state = state;
        EXPECT_EQ(closeConnectionRefusal(opening), "") << static_cast<int>(state);
    }
    SystemConnection udp = firefoxToGoogle();
    udp.connection.protocol = ConnectionProtocol::Udp;
    EXPECT_EQ(closeConnectionRefusal(udp), "Only a TCP connection can be closed");
    SystemConnection listening = firefoxToGoogle();
    listening.connection.state = ConnectionState::Listen;
    EXPECT_EQ(closeConnectionRefusal(listening), "A listening socket has no connection to close");
    for (const ConnectionState state :
         {ConnectionState::TimeWait, ConnectionState::FinWait1, ConnectionState::Closed, ConnectionState::Unknown})
    {
        SystemConnection closing = firefoxToGoogle();
        closing.connection.state = state;
        EXPECT_EQ(closeConnectionRefusal(closing), "The connection is already closing") << static_cast<int>(state);
    }
    SystemConnection noHolder = firefoxToGoogle();
    noHolder.pid = 0;
    SystemConnection noStartTime = firefoxToGoogle();
    noStartTime.startTimeTicks = 0;
    SystemConnection noInode = firefoxToGoogle();
    noInode.socketInode = 0;
    for (const SystemConnection& nobody : {noHolder, noStartTime, noInode})
    {
        EXPECT_EQ(closeConnectionRefusal(nobody), "No process TaskSmack can see holds this socket");
    }
}

[[nodiscard]] Platform::ProcessActionCapabilities closingCapabilities(bool authorize)
{
    Platform::ProcessActionCapabilities caps;
    caps.canCloseConnections = true;
    caps.canAuthorizeInteractively = authorize;
    return caps;
}

TEST(ConnectionCloseTest, AConfirmedCloseNamesTheSocketAndReportsTheResult)
{
    MockProcessActions actions;
    actions.setCapabilities(closingCapabilities(false));
    actions.setCloseConnectionResult(Platform::ProcessActionResult::ok());
    ConnectionCloseControl control;
    const ConnectionRow row = rowFor(firefoxToGoogle());

    control.request(row);
    ASSERT_TRUE(control.pending().has_value());
    EXPECT_TRUE(control.showConfirm());
    EXPECT_TRUE(control.question().starts_with("Close the connection firefox (PID 300) has from 10.0.0.5:52144 to 142.250.80.46:443?"))
        << control.question();

    control.confirm(&actions);
    EXPECT_FALSE(control.pending().has_value());
    EXPECT_FALSE(control.showConfirm());
    control.update(0.0F, true);
    EXPECT_EQ(actions.closeConnectionCount(), 1);
    EXPECT_EQ(actions.lastSocketInode(), 50300U);
    EXPECT_EQ(actions.lastTarget().pid, 300);
    EXPECT_EQ(actions.lastTarget().startTimeTicks, 1300U);
    EXPECT_TRUE(control.result().ok);
    EXPECT_EQ(control.result().text, "Closed firefox's connection to 142.250.80.46:443");
    EXPECT_TRUE(control.takeClosed());
    EXPECT_FALSE(control.takeClosed()) << "once";
    EXPECT_FALSE(control.offersAuthenticateAndRetry());

    // The line goes after a while.
    control.update(ConnectionCloseControl::RESULT_SECONDS + 1.0F);
    EXPECT_TRUE(control.result().empty());
}

TEST(ConnectionCloseTest, ARefusalOffersAuthenticateAndRetryThroughTheHelper)
{
    MockProcessActions actions;
    actions.setCapabilities(closingCapabilities(true));
    actions.setCloseConnectionResult(Platform::ProcessActionResult::denied("Operation not permitted"));
    actions.setRunAuthorizedResult(Platform::ProcessActionResult::ok());
    ConnectionCloseControl control;
    control.request(rowFor(firefoxToGoogle()));
    control.confirm(&actions);
    control.update(0.0F, true);
    EXPECT_FALSE(control.result().ok);
    EXPECT_EQ(control.result().text, "Couldn't close firefox's connection to 142.250.80.46:443: Operation not permitted");
    EXPECT_FALSE(control.takeClosed());
    ASSERT_TRUE(control.offersAuthenticateAndRetry());

    control.retryAuthorized();
    control.update(0.0F, true);
    EXPECT_EQ(actions.runAuthorizedCount(), 1);
    EXPECT_EQ(actions.lastAuthorizedAction().kind, Platform::AuthorizedAction::Kind::CloseConnection);
    EXPECT_EQ(actions.lastAuthorizedAction().socketInode, 50300U);
    EXPECT_EQ(actions.lastAuthorizedAction().target.pid, 300);
    EXPECT_TRUE(control.result().ok);
    EXPECT_TRUE(control.takeClosed());
    EXPECT_FALSE(control.offersAuthenticateAndRetry());

    // Without interactive authorization there is nothing to offer.
    MockProcessActions plain;
    plain.setCapabilities(closingCapabilities(false));
    plain.setCloseConnectionResult(Platform::ProcessActionResult::denied("Operation not permitted"));
    control.request(rowFor(firefoxToGoogle()));
    control.confirm(&plain);
    control.update(0.0F, true);
    EXPECT_FALSE(control.offersAuthenticateAndRetry());
}

TEST(ConnectionCloseTest, NothingIsClosedWhenCancelledRefusedOrUnavailable)
{
    MockProcessActions actions;
    actions.setCapabilities(closingCapabilities(false));
    ConnectionCloseControl control;

    control.request(rowFor(firefoxToGoogle()));
    control.cancel();
    EXPECT_FALSE(control.pending().has_value());
    control.confirm(&actions); // nothing pending
    control.update(0.0F, true);
    EXPECT_EQ(actions.closeConnectionCount(), 0);
    EXPECT_TRUE(control.result().empty());

    SystemConnection listening = firefoxToGoogle();
    listening.connection.state = ConnectionState::Listen;
    control.request(rowFor(listening));
    EXPECT_FALSE(control.pending().has_value()) << "a socket that can't be closed opens no confirm";

    // Actions that can't close connections: said so, nothing called.
    MockProcessActions unable;
    control.request(rowFor(firefoxToGoogle()));
    control.confirm(&unable);
    EXPECT_EQ(unable.closeConnectionCount(), 0);
    EXPECT_FALSE(control.result().ok);
    EXPECT_TRUE(control.result().text.contains("isn't available")) << control.result().text;
    control.request(rowFor(firefoxToGoogle()));
    control.confirm(nullptr);
    EXPECT_TRUE(control.result().text.contains("isn't available"));
}

/// Right-clicks the firefox row of @p panel's table (the second row by process name: avahi-daemon, cupsd,
/// firefox, Unknown) and picks the menu's first item, Close connection...
void closeFirefoxFromTheMenu(ImVec2 firstRow, const std::function<std::string()>& nextFrame)
{
    const float rowHeight = ImGui::GetTextLineHeight() + (ImGui::GetStyle().CellPadding.y * 2.0F);
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(firstRow.x, firstRow.y + (rowHeight * 2.0F));
    static_cast<void>(nextFrame());
    io.AddMouseButtonEvent(ImGuiMouseButton_Right, true);
    static_cast<void>(nextFrame());
    io.AddMouseButtonEvent(ImGuiMouseButton_Right, false);
    static_cast<void>(nextFrame());
    ASSERT_GT(GImGui->OpenPopupStack.Size, 0) << "the row menu didn't open";
    const ImGuiWindow* menu = GImGui->OpenPopupStack.back().Window;
    ASSERT_NE(menu, nullptr);
    const ImVec2 item(menu->Pos.x + (menu->Size.x * 0.5F),
                      menu->Pos.y + ImGui::GetStyle().WindowPadding.y + (ImGui::GetTextLineHeight() * 0.5F));
    io.AddMousePosEvent(item.x, item.y);
    static_cast<void>(nextFrame());
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    static_cast<void>(nextFrame());
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    static_cast<void>(nextFrame());
    io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
}

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

// #3186: a failed read says so with the kernel's message; a new refresh rate, and showing the tab again,
// reach the running sampler; a double-click asks for Process Details.
TEST_F(ConnectionsPanelRenderTest, AFailedReadSaysWhyAndADoubleClickShowsDetails)
{
    ConnectionsPanel failing([] { return std::make_unique<ListReader>(std::vector<SystemConnection>{}, "Permission denied"); },
                             [](Core::Event&) {});
    failing.onAttach();
    show(failing, "Connections");
    const std::string failed = frameUntil(failing, "couldn't be read");
    EXPECT_TRUE(failed.contains("Connections couldn't be read")) << failed;
    EXPECT_TRUE(failed.contains("Permission denied")) << failed;
    failing.onDetach();

    auto reads = std::make_shared<std::atomic<int>>(0);
    int detailsShown = 0;
    ConnectionsPanel panel([reads] { return std::make_unique<FakeReader>(reads); },
                           [&detailsShown](Core::Event& event)
                           {
                               if (dynamic_cast<Core::ShowProcessDetailsEvent*>(&event) != nullptr)
                               {
                                   ++detailsShown;
                               }
                           });
    panel.onAttach();
    show(panel, "Connections");
    static_cast<void>(frameUntil(panel, "firefox"));
    Core::RefreshRateChangedEvent faster(250);
    panel.onEvent(faster);

    // Hidden, then shown again: the running sampler reads at once.
    show(panel, "Processes");
    static_cast<void>(frame(panel));
    const int before = reads->load();
    show(panel, "Connections");
    const auto giveUp = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (reads->load() == before && std::chrono::steady_clock::now() < giveUp)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_GT(reads->load(), before);

    ImVec2 row(-1.0F, -1.0F);
    static_cast<void>(frame(panel, &row));
    ASSERT_GT(row.y, 0.0F);
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(row.x, row.y);
    static_cast<void>(frame(panel));
    for (int click = 0; click < 2; ++click)
    {
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        static_cast<void>(frame(panel));
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        static_cast<void>(frame(panel));
    }
    io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
    static_cast<void>(frame(panel));
    EXPECT_GE(detailsShown, 1);
    panel.onDetach();
}

// #3066: the row menu's Close connection... opens the confirm for the row right-clicked, a confirm closes it
// through the panel's actions, and the result line says so. Without the capability there is no menu.
TEST_F(ConnectionsPanelRenderTest, TheRowMenuClosesAConnectionAfterTheConfirm)
{
    auto reads = std::make_shared<std::atomic<int>>(0);
    MockProcessActions* actions = nullptr;
    ConnectionsPanel panel([reads] { return std::make_unique<FakeReader>(reads); },
                           [](Core::Event&) {},
                           [&actions]
                           {
                               auto made = std::make_unique<MockProcessActions>();
                               made->setCapabilities(closingCapabilities(false));
                               made->setCloseConnectionResult(Platform::ProcessActionResult::ok());
                               actions = made.get();
                               return made;
                           });
    panel.onAttach();
    ASSERT_NE(actions, nullptr);
    show(panel, "Connections");
    static_cast<void>(frameUntil(panel, "firefox"));
    ImVec2 row(-1.0F, -1.0F);
    static_cast<void>(frame(panel, &row));
    ASSERT_GT(row.y, 0.0F);

    closeFirefoxFromTheMenu(row, [&panel] { return frame(panel); });
    ASSERT_TRUE(panel.closeControl().pending().has_value());
    EXPECT_EQ(panel.closeControl().pending()->process, "firefox");
    const std::string confirm = frame(panel);
    EXPECT_TRUE(confirm.contains("Close the connection firefox (PID 300)")) << confirm;

    // Confirmed (as the modal's button does), then taken in: the line says it closed.
    panel.closeControl().confirm(actions);
    panel.closeControl().update(0.0F, true);
    EXPECT_EQ(actions->closeConnectionCount(), 1);
    EXPECT_EQ(actions->lastSocketInode(), 50300U);
    const std::string done = frame(panel);
    EXPECT_TRUE(done.contains("Closed firefox's connection to 142.250.80.46:443")) << done;
    panel.onDetach();

    // No actions that can close connections: right-clicking opens no menu.
    ConnectionsPanel unable([reads] { return std::make_unique<FakeReader>(reads); }, [](Core::Event&) {});
    unable.onAttach();
    show(unable, "Connections");
    static_cast<void>(frameUntil(unable, "firefox"));
    static_cast<void>(frame(unable, &row));
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(row.x, row.y);
    static_cast<void>(frame(unable));
    io.AddMouseButtonEvent(ImGuiMouseButton_Right, true);
    static_cast<void>(frame(unable));
    io.AddMouseButtonEvent(ImGuiMouseButton_Right, false);
    static_cast<void>(frame(unable));
    EXPECT_EQ(GImGui->OpenPopupStack.Size, 0) << "a row menu opened";
    EXPECT_FALSE(unable.closeControl().pending().has_value());
    unable.onDetach();
}

} // namespace
} // namespace App
