// The Connections tab's Linux reader (#3066): a fake /proc tree with the socket tables and one process
// holding two of the sockets.

#include "Platform/IProcessConnections.h"
#include "Platform/ISystemConnections.h"
#include "Platform/Linux/LinuxSystemConnectionsReader.h"
#include "Platform/ScopedTempDir.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace Platform
{
namespace
{

constexpr std::string_view TCP_HEADER =
    "  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode\n";

class LinuxSystemConnectionsReaderTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        std::filesystem::create_directories(m_Proc.path / "net");
        // 127.0.0.1:631 listening (inode 1001), 10.0.0.5:52144 -> 142.250.80.46:443 established (inode
        // 1002), and a TIME_WAIT socket no file holds (inode 0).
        write("net/tcp",
              std::string(TCP_HEADER) +
                  "   0: 0100007F:0277 00000000:0000 0A 00000000:00000000 00:00000000 00000000     0        0 1001 1 0 100 0 0 10 0\n"
                  "   1: 0500000A:CBB0 2E50FA8E:01BB 01 00000000:00000000 00:00000000 00000000  1000        0 1002 1 0 20 4 30 10 -1\n"
                  "   2: 0500000A:CBB1 2E50FA8E:01BB 06 00000000:00000000 00:00000000 00000000     0        0 0 3 0\n");
        // No UDP or IPv6 tables at all: those families are disabled here, which isn't a failure.
        std::filesystem::create_directories(m_Proc.path / "100" / "fd");
        write("100/stat", "100 (cupsd) S 1 100 100 0 -1 4194560 10 0 0 0 5 6 0 0 20 0 1 0 4242 1000 50\n");
        write("100/comm", "cupsd\n");
        std::filesystem::create_symlink("socket:[1001]", m_Proc.path / "100" / "fd" / "3");
        std::filesystem::create_symlink("socket:[1002]", m_Proc.path / "100" / "fd" / "4");
        std::filesystem::create_symlink("/dev/null", m_Proc.path / "100" / "fd" / "5");
    }

    void write(const std::string& path, std::string_view contents) const
    {
        std::ofstream(m_Proc.path / path) << contents;
    }

    TestSupport::ScopedTempDir m_Proc{"ts_test_proc_sysconn"};
};

TEST_F(LinuxSystemConnectionsReaderTest, ListsEverySocketWithItsHolder)
{
    LinuxSystemConnectionsReader reader(m_Proc.path.string());
    EXPECT_TRUE(reader.hasConnections());
    const SystemConnectionsRead read = reader.read();
    ASSERT_EQ(read.status, SystemConnectionsStatus::Ok) << read.detail;
    ASSERT_EQ(read.connections.size(), 3U);

    const auto byPort = [&read](std::uint16_t port)
    {
        return std::ranges::find_if(read.connections, [port](const SystemConnection& c) { return c.connection.local.port == port; });
    };
    const auto listening = byPort(631);
    ASSERT_NE(listening, read.connections.end());
    EXPECT_EQ(listening->pid, 100);
    EXPECT_EQ(listening->startTimeTicks, 4242U);
    EXPECT_EQ(listening->processName, "cupsd");
    EXPECT_EQ(listening->connection.state, ConnectionState::Listen);

    const auto established = byPort(52144);
    ASSERT_NE(established, read.connections.end());
    EXPECT_EQ(established->pid, 100);
    EXPECT_EQ(established->connection.remote.port, 443);
    EXPECT_EQ(established->connection.state, ConnectionState::Established);

    // TIME_WAIT: no file holds it, so no holder.
    const auto timeWait = byPort(52145);
    ASSERT_NE(timeWait, read.connections.end());
    EXPECT_EQ(timeWait->pid, 0);
    EXPECT_TRUE(timeWait->processName.empty());
}

TEST_F(LinuxSystemConnectionsReaderTest, NoReadableTableIsAFailure)
{
    std::filesystem::remove_all(m_Proc.path / "net");
    std::filesystem::create_directories(m_Proc.path / "net" / "tcp"); // a directory: the read fails, not ENOENT
    std::filesystem::create_directories(m_Proc.path / "net" / "tcp6");
    std::filesystem::create_directories(m_Proc.path / "net" / "udp");
    std::filesystem::create_directories(m_Proc.path / "net" / "udp6");
    LinuxSystemConnectionsReader reader(m_Proc.path.string());
    EXPECT_EQ(reader.read().status, SystemConnectionsStatus::Failed);
}

TEST(LinuxSystemConnectionsReaderRealTest, ReadsTheMachinesSockets)
{
    LinuxSystemConnectionsReader reader;
    const SystemConnectionsRead read = reader.read();
    EXPECT_EQ(read.status, SystemConnectionsStatus::Ok) << read.detail; // empty is fine in a bare container
}

} // namespace
} // namespace Platform
