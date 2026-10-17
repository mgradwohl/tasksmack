// tasksmack-helper: TaskSmack's privileged broker on Linux (#2307; #2181). TaskSmack's UI never runs as
// root. When the user authenticates for an action the UI can't do itself, TaskSmack starts this with
// pkexec, which asks for the password (polkit action app.tasksmack.TaskSmack.helper) and runs it as root
// with one end of a socketpair as its stdin. It answers a closed set of BrokerProtocol requests on that
// socket and exits when TaskSmack closes it, or after a few idle minutes.
//
// It links only Platform: no SDL, ImGui, GL or config file. Before it reads a single request it checks
// that the socket's peer is the process that started it (pkexec execs this in its own place, so that is
// TaskSmack), and the user pkexec authenticated (PKEXEC_UID) or, run directly as the tests do, its own.

#include "Platform/BrokerProtocol.h"
#include "Platform/ISystemInfoProbe.h"
#include "Platform/Linux/BrokerServer.h"
#include "Platform/Linux/BrokerSocket.h"
#include "Platform/Linux/FirmwareFileRead.h"
#include "Platform/Linux/LinuxCoredumps.h"
#include "Platform/Linux/LinuxProcessActions.h"
#include "Platform/Linux/LinuxProcessConnectionsReader.h"
#include "Platform/Linux/LinuxProcessEnvironmentReader.h"
#include "Platform/Linux/LinuxProcessModulesReader.h"
#include "Platform/Linux/LinuxProcessOpenFilesReader.h"
#include "Platform/Linux/MemoryRollupRead.h"
#include "Platform/Linux/SystemdJournal.h"

#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <optional>
#include <stop_token>
#include <string_view>
#include <system_error>

#include <sys/types.h>
#include <unistd.h>

namespace
{

/// The socket TaskSmack passes: its stdin.
constexpr int CLIENT_FD = 0;
/// How long the helper waits for a request before it exits; TaskSmack starts it again when needed.
constexpr std::chrono::minutes IDLE_TIMEOUT{5};

// Exit statuses.
constexpr int EXIT_DONE = 0;
constexpr int EXIT_FAILED = 1;
constexpr int EXIT_USAGE = 2;
constexpr int EXIT_UNTRUSTED_PEER = 3;

/// The user whose client this may serve: the one pkexec authenticated, else this process's own.
[[nodiscard]] std::optional<uid_t> expectedClientUid()
{
    const char* const pkexecUid = std::getenv("PKEXEC_UID"); // NOLINT(concurrency-mt-unsafe) - read once, before any thread
    if (pkexecUid == nullptr)
    {
        return ::getuid();
    }
    const std::string_view text(pkexecUid);
    uid_t uid = 0;
    const char* const first = text.data();
    const char* const last = first + text.size();
    const auto [ptr, ec] = std::from_chars(first, last, uid);
    if (ec != std::errc{} || ptr != last || text.empty())
    {
        return std::nullopt;
    }
    return uid;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 2 || std::string_view(argv[1]) != "--broker") // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic) - argv
    {
        std::fputs("tasksmack-helper: started by TaskSmack only (--broker, with its socket as stdin)\n", stderr);
        return EXIT_USAGE;
    }
    namespace Socket = Platform::BrokerSocket;
    const std::optional<uid_t> expectedUid = expectedClientUid();
    const std::optional<Socket::PeerCredentials> peer =
        Socket::isSeqPacketSocket(CLIENT_FD) ? Socket::peerCredentials(CLIENT_FD) : std::nullopt;
    if (!expectedUid.has_value() || !peer.has_value() || peer->pid != ::getppid() || peer->uid != *expectedUid)
    {
        std::fputs("tasksmack-helper: stdin isn't a socket from the process that started this, as its user\n", stderr);
        return EXIT_UNTRUSTED_PEER;
    }
    try
    {
        Platform::LinuxProcessActions actions;
        // Process Details' reads of another user's process (#2309).
        Platform::LinuxProcessEnvironmentReader environment;
        Platform::LinuxProcessModulesReader modules;
        Platform::LinuxProcessOpenFilesReader openFiles;
        Platform::LinuxProcessConnectionsReader connections;
        const Platform::BrokerServer::Readers readers{
            .environment = &environment,
            .modules = &modules,
            .openFiles = &openFiles,
            .connections = &connections,
            .memoryRollup = [](std::int32_t pid, std::uint64_t startTimeTicks)
            { return Platform::Linux::readMemoryRollupAt("/proc", pid, startTimeTicks); },
            // System Information's root-only firmware files (#2310): only the ones FirmwareFile names
            .firmwareFile = [](Platform::Broker::FirmwareFile file) { return Platform::Linux::readFirmwareFileAt("/", file); },
            // Every user's crashes: the journal and core files as root (#2310)
            .crashes =
                []
            {
                Platform::CrashesInfo info;
                const std::time_t now = std::time(nullptr);
                Platform::LinuxCoredumps::readCrashFacts(
                    "/", now > 0 ? static_cast<std::uint64_t>(now) : 0, info, Platform::SystemdJournal::makeCoredumpJournalReader());
                return info;
            },
        };
        const std::stop_source never;
        switch (Platform::BrokerServer::serve(CLIENT_FD, actions, IDLE_TIMEOUT, never.get_token(), readers))
        {
        case Platform::BrokerServer::SessionEnd::ClientClosed:
        case Platform::BrokerServer::SessionEnd::Idle:
        case Platform::BrokerServer::SessionEnd::Stopped:
            return EXIT_DONE;
        case Platform::BrokerServer::SessionEnd::HandshakeFailed:
        case Platform::BrokerServer::SessionEnd::Broken:
            break;
        }
    }
    catch (...)
    {
        std::fputs("tasksmack-helper: failed\n", stderr); // an allocation failure or the like: TaskSmack sees the socket close
    }
    return EXIT_FAILED;
}
