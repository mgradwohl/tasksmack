/// @file test_ProcessConnectionsView.cpp
/// @brief The Connections section's state, without ImGui (#799): it reads only while drawn open, once
/// on opening and then at PROCESS_CONNECTIONS_REFRESH_MS, through the injected reader for the given
/// target; a selection change drops the rows and the open frame and reads afresh; it formats each
/// read once and sorts it (by state, then remote, by default, or by the chosen column); and each read
/// status is kept for the status line.

#include "App/Panels/ProcessConnectionsView.h"
#include "Domain/SamplingConfig.h"
#include "Mocks/MockProbes.h"
#include "Platform/IProcessActions.h"
#include "Platform/IProcessConnections.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

namespace App
{
namespace
{

using Platform::ConnectionFamily;
using Platform::ConnectionProtocol;
using Platform::ConnectionState;
using Platform::ProcessConnection;

constexpr Platform::ProcessTarget TARGET{.pid = 4242, .startTimeTicks = 777};
constexpr float REFRESH_SECONDS = static_cast<float>(Domain::Sampling::PROCESS_CONNECTIONS_REFRESH_MS) / 1000.0F;

[[nodiscard]] ProcessConnection tcp4(std::array<std::uint8_t, 16> local,
                                     std::uint16_t localPort,
                                     std::array<std::uint8_t, 16> remote,
                                     std::uint16_t remotePort,
                                     ConnectionState state)
{
    return {.protocol = ConnectionProtocol::Tcp,
            .family = ConnectionFamily::IPv4,
            .local = {.address = local, .port = localPort},
            .remote = {.address = remote, .port = remotePort},
            .state = state};
}

[[nodiscard]] Platform::ConnectionsReadResult okResult(std::vector<ProcessConnection> connections)
{
    return {.status = Platform::ConnectionsReadStatus::Ok, .connections = std::move(connections)};
}

/// A small mixed set: a listener, two established connections, a TIME_WAIT one and a UDP socket.
[[nodiscard]] Platform::ConnectionsReadResult sampleResult()
{
    ProcessConnection udp{.protocol = ConnectionProtocol::Udp,
                          .family = ConnectionFamily::IPv6,
                          .local = {.address = {}, .port = 5353},
                          .remote = {},
                          .state = ConnectionState::Closed};
    return okResult({tcp4({10, 0, 0, 5}, 50000, {10, 0, 0, 10}, 443, ConnectionState::TimeWait),
                     tcp4({127, 0, 0, 1}, 631, {}, 0, ConnectionState::Listen),
                     udp,
                     tcp4({10, 0, 0, 5}, 50001, {10, 0, 0, 10}, 443, ConnectionState::Established),
                     tcp4({10, 0, 0, 5}, 50002, {10, 0, 0, 9}, 443, ConnectionState::Established)});
}

/// One frame as the panel runs it: update (reads if due), then render drew the section open or not.
bool frame(ProcessConnectionsView& view, TestMocks::MockProcessConnectionsReader& reader, float deltaSeconds, bool drawnOpen)
{
    const bool read = view.update(&reader, TARGET, deltaSeconds);
    if (drawnOpen)
    {
        view.markDrawnOpen();
    }
    return read;
}

// ========== Read cadence ==========

TEST(ProcessConnectionsViewTest, NeverReadsWhileTheSectionIsClosed)
{
    ProcessConnectionsView view;
    TestMocks::MockProcessConnectionsReader reader;
    for (int i = 0; i < 10; ++i)
    {
        EXPECT_FALSE(frame(view, reader, REFRESH_SECONDS, false));
    }
    EXPECT_EQ(reader.readCount(), 0);
    EXPECT_FALSE(view.hasRead());
}

TEST(ProcessConnectionsViewTest, ReadsOnOpeningThenAtTheRefreshCadence)
{
    ProcessConnectionsView view;
    TestMocks::MockProcessConnectionsReader reader;
    reader.setResult(sampleResult());

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

    // Past the interval: one more.
    EXPECT_TRUE(frame(view, reader, 0.2F, true));
    EXPECT_EQ(reader.readCount(), 2);
}

TEST(ProcessConnectionsViewTest, RefreshesFasterThanTheEnvironmentButNotEverySample)
{
    EXPECT_EQ(Domain::Sampling::PROCESS_CONNECTIONS_REFRESH_MS, 2000);
    EXPECT_LT(Domain::Sampling::PROCESS_CONNECTIONS_REFRESH_MS, Domain::Sampling::PROCESS_ENVIRONMENT_REFRESH_MS);
}

TEST(ProcessConnectionsViewTest, StopsReadingWhenClosedAndReadsAgainWhenReopenedLater)
{
    ProcessConnectionsView view;
    TestMocks::MockProcessConnectionsReader reader;
    static_cast<void>(frame(view, reader, 0.016F, true));
    static_cast<void>(frame(view, reader, 0.016F, false)); // read, then the section is closed
    ASSERT_EQ(reader.readCount(), 1);

    // Closed for longer than the interval: nothing read meanwhile.
    for (int i = 0; i < 5; ++i)
    {
        EXPECT_FALSE(frame(view, reader, REFRESH_SECONDS, false));
    }
    EXPECT_EQ(reader.readCount(), 1);

    // Reopened: the interval has long passed, so it reads at once.
    static_cast<void>(frame(view, reader, 0.016F, true));
    EXPECT_TRUE(frame(view, reader, 0.016F, true));
    EXPECT_EQ(reader.readCount(), 2);
}

TEST(ProcessConnectionsViewTest, SelectionChangeDropsTheRowsAndReadsAfreshWithoutWaitingForTheInterval)
{
    ProcessConnectionsView view;
    TestMocks::MockProcessConnectionsReader reader;
    reader.setResult(sampleResult());
    static_cast<void>(frame(view, reader, 0.016F, true));
    static_cast<void>(frame(view, reader, 0.016F, true));
    ASSERT_EQ(reader.readCount(), 1);
    ASSERT_FALSE(view.rows().empty());

    view.onSelectionChanged();
    EXPECT_FALSE(view.hasRead());
    EXPECT_TRUE(view.rows().empty()) << "nothing of the previous process is shown for the next";
    view.markDrawnOpen();
    EXPECT_TRUE(view.update(&reader, TARGET, 0.016F));
    EXPECT_EQ(reader.readCount(), 2);
}

TEST(ProcessConnectionsViewTest, SelectionChangeForgetsThePreviousProcesssOpenFrame)
{
    ProcessConnectionsView view;
    TestMocks::MockProcessConnectionsReader reader;
    static_cast<void>(frame(view, reader, 0.016F, true)); // the previous process's section drawn open
    ASSERT_EQ(reader.readCount(), 0);

    // The selection changes before update() consumes that frame: the new process's section has not
    // been drawn open yet, so nothing may be read for it.
    view.onSelectionChanged();
    EXPECT_FALSE(view.update(&reader, TARGET, 0.016F));
    EXPECT_EQ(reader.readCount(), 0);
}

TEST(ProcessConnectionsViewTest, NoReaderOrNoSupportMeansNoRead)
{
    ProcessConnectionsView view;
    view.markDrawnOpen();
    EXPECT_FALSE(view.update(nullptr, TARGET, 1.0F));

    TestMocks::MockProcessConnectionsReader reader;
    reader.setHasConnections(false);
    view.markDrawnOpen();
    EXPECT_FALSE(view.update(&reader, TARGET, 1.0F));
    EXPECT_EQ(reader.readCount(), 0);
}

// ========== Formatting and order ==========

TEST(ProcessConnectionsViewTest, FormatsEachRowOnceAndSortsByStateThenRemoteByDefault)
{
    ProcessConnectionsView view;
    view.applyResult(sampleResult());
    EXPECT_EQ(view.sortColumn(), Detail::ConnectionsColumn::State);
    EXPECT_TRUE(view.sortAscending());

    const auto rows = view.rows();
    ASSERT_EQ(rows.size(), 5U);
    // Established first, the nearer remote (10.0.0.9, numerically before 10.0.0.10) first among them.
    EXPECT_EQ(rows[0].state, "ESTABLISHED");
    EXPECT_EQ(rows[0].remote, "10.0.0.9:443");
    EXPECT_EQ(rows[0].local, "10.0.0.5:50002");
    EXPECT_EQ(rows[0].protocol, "TCP");
    EXPECT_EQ(rows[1].state, "ESTABLISHED");
    EXPECT_EQ(rows[1].remote, "10.0.0.10:443");
    // Then the listener, then the unconnected UDP socket, TIME_WAIT last.
    EXPECT_EQ(rows[2].state, "LISTEN");
    EXPECT_EQ(rows[2].local, "127.0.0.1:631");
    EXPECT_EQ(rows[2].remote, "0.0.0.0:*");
    EXPECT_EQ(rows[3].state, "UNCONN");
    EXPECT_EQ(rows[3].protocol, "UDP6");
    EXPECT_EQ(rows[3].local, "[::]:5353");
    EXPECT_EQ(rows[4].state, "TIME_WAIT");
}

TEST(ProcessConnectionsViewTest, SortsByTheChosenColumnEitherWayAndKeepsItAcrossReReadsAndSelections)
{
    ProcessConnectionsView view;
    view.applyResult(sampleResult());

    view.setSort(Detail::ConnectionsColumn::Local, true);
    auto rows = view.rows();
    ASSERT_EQ(rows.size(), 5U);
    EXPECT_EQ(rows[0].local, "10.0.0.5:50000");
    EXPECT_EQ(rows[1].local, "10.0.0.5:50001");
    EXPECT_EQ(rows[2].local, "10.0.0.5:50002");
    EXPECT_EQ(rows[3].local, "127.0.0.1:631");
    EXPECT_EQ(rows[4].local, "[::]:5353"); // IPv6 after IPv4

    view.setSort(Detail::ConnectionsColumn::Local, false);
    rows = view.rows();
    EXPECT_EQ(rows[0].local, "[::]:5353");
    EXPECT_EQ(rows[4].local, "10.0.0.5:50000");

    // A re-read comes back in the chosen order, and a new selection keeps it.
    view.applyResult(sampleResult());
    EXPECT_EQ(view.rows()[0].local, "[::]:5353");
    view.onSelectionChanged();
    EXPECT_EQ(view.sortColumn(), Detail::ConnectionsColumn::Local);
    EXPECT_FALSE(view.sortAscending());

    view.setSort(Detail::ConnectionsColumn::Protocol, true);
    view.applyResult(sampleResult());
    EXPECT_EQ(view.rows()[0].protocol, "TCP");
    EXPECT_EQ(view.rows()[4].protocol, "UDP6");
}

TEST(ProcessConnectionsViewTest, KeepsEachReadStatusForItsStatusLine)
{
    for (const auto status : {Platform::ConnectionsReadStatus::PermissionDenied,
                              Platform::ConnectionsReadStatus::ProcessExited,
                              Platform::ConnectionsReadStatus::Unsupported,
                              Platform::ConnectionsReadStatus::IdentityUnknown,
                              Platform::ConnectionsReadStatus::Failed})
    {
        ProcessConnectionsView view;
        view.applyResult({.status = status, .connections = {}});
        EXPECT_TRUE(view.hasRead());
        EXPECT_EQ(view.status(), status);
        EXPECT_TRUE(view.rows().empty());
        EXPECT_FALSE(Detail::connectionsStatusText(status).empty());
    }
    EXPECT_TRUE(Detail::connectionsStatusText(Platform::ConnectionsReadStatus::Ok).empty());
    EXPECT_EQ(Detail::connectionsStatusText(Platform::ConnectionsReadStatus::PermissionDenied), "Not permitted (another user's process)");
}

TEST(ProcessConnectionsViewTest, CountTextIsSingularForOne)
{
    EXPECT_EQ(Detail::connectionsCountText(1), "1 connection");
    EXPECT_EQ(Detail::connectionsCountText(0), "0 connections");
    EXPECT_EQ(Detail::connectionsCountText(12), "12 connections");
}

} // namespace
} // namespace App
