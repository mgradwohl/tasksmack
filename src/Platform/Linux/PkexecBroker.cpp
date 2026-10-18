#include "PkexecBroker.h"

#include "Platform/BrokerProtocol.h"
#include "Platform/Linux/BrokerSocket.h"

#include <array>
#include <chrono>
#include <expected>
#include <format>
#include <mutex>
#include <stop_token>
#include <string>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include <poll.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

extern char**
    environ; // NOLINT(readability-redundant-declaration,cppcoreguidelines-avoid-non-const-global-variables) - POSIX: the process environment, for posix_spawn

// NOLINTBEGIN(misc-include-cleaner) - POSIX headers: include-cleaner lacks mappings for AF_UNIX, SOCK_*, WNOHANG and W* macros
namespace Platform
{

namespace
{

/// How long the destructor waits for the launcher to exit once the socket is closed.
constexpr int REAP_ATTEMPTS = 50;
constexpr useconds_t REAP_STEP_MICROSECONDS = 20'000;
/// roundTrip()'s errors while the launcher may still be running (the prompt still up): it isn't waited for.
constexpr const char* HELPER_TIMED_OUT = "the helper didn't answer in time";
constexpr const char* CANCELLED = "cancelled";

} // namespace

PkexecBroker::PkexecBroker(std::string helperPath, std::vector<std::string> launcher)
    : m_HelperPath(std::move(helperPath)), m_Launcher(std::move(launcher))
{}

PkexecBroker::~PkexecBroker()
{
    try
    {
        const std::scoped_lock lock(m_Mutex);
        closeLocked();
        // The helper exits once its socket closes, and pkexec with it; wait briefly so none is left a
        // zombie. One that outlives the wait is reaped by init when TaskSmack exits.
        for (int attempt = 0; attempt < REAP_ATTEMPTS && !m_Abandoned.empty(); ++attempt)
        {
            reapAbandonedLocked();
            if (!m_Abandoned.empty())
            {
                ::usleep(REAP_STEP_MICROSECONDS);
            }
        }
    }
    catch (...) // NOLINT(bugprone-empty-catch) - a destructor mustn't throw; what it couldn't reap init does
    {}
}

bool PkexecBroker::connected() const noexcept
{
    if (!m_Connected.load())
    {
        return false;
    }
    // The helper also ends on its own (its idle exit, a crash), and then its end of the socket hangs up
    // (#2368). Looked at when no call holds the lock; a call in progress finds out for itself.
    const std::unique_lock lock(m_Mutex, std::try_to_lock);
    if (lock.owns_lock() && m_Fd >= 0)
    {
        pollfd peer{.fd = m_Fd, .events = POLLRDHUP, .revents = 0};
        if (::poll(&peer, 1, 0) > 0 && (peer.revents & (POLLHUP | POLLRDHUP | POLLERR)) != 0)
        {
            m_Connected = false; // the next connect() closes the socket and reaps the launcher
        }
    }
    return m_Connected.load();
}

std::expected<void, std::string> PkexecBroker::connectWithin(const std::stop_token& stopToken, std::chrono::milliseconds timeout)
{
    const std::scoped_lock lock(m_Mutex);
    if (m_Connected.load())
    {
        return {};
    }
    closeLocked();

    std::array<int, 2> fds{-1, -1};
    if (::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, fds.data()) != 0)
    {
        return std::unexpected(std::format("couldn't make the helper's socket: {}", std::system_category().message(errno)));
    }
    // The helper's end becomes its stdin; dup2 clears close-on-exec there, and only there.
    posix_spawn_file_actions_t files{};
    posix_spawn_file_actions_init(&files);
    posix_spawn_file_actions_adddup2(&files, fds[1], STDIN_FILENO);
    std::vector<std::string> arguments = m_Launcher;
    arguments.push_back(m_HelperPath);
    arguments.emplace_back("--broker");
    std::vector<char*> argv;
    argv.reserve(arguments.size() + 1);
    for (std::string& argument : arguments)
    {
        argv.push_back(argument.data());
    }
    argv.push_back(nullptr);
    pid_t child = -1;
    const int spawned = ::posix_spawnp(&child, argv.front(), &files, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&files);
    ::close(fds[1]);
    if (spawned != 0)
    {
        ::close(fds[0]);
        return std::unexpected(std::format("couldn't start {}: {}", arguments.front(), std::system_category().message(spawned)));
    }
    m_Fd = fds[0];
    m_Child = child;

    const auto reply = roundTrip(Broker::Hello{}, timeout, stopToken);
    if (!reply.has_value())
    {
        // A launcher that closed the socket, or never read it, has exited and says why; one still
        // prompting (a timeout, a stop) isn't waited for.
        const bool mayStillRun = reply.error() == HELPER_TIMED_OUT || reply.error() == CANCELLED;
        const std::string reason = mayStillRun ? std::string{} : launcherExitReason();
        closeLocked();
        return std::unexpected(reason.empty() ? reply.error() : reason);
    }
    const auto* const hello = std::get_if<Broker::HelloReply>(&*reply);
    if (hello == nullptr || !hello->accepted)
    {
        closeLocked();
        return std::unexpected(
            hello == nullptr
                ? std::string("the helper didn't answer the handshake")
                : std::format("the helper speaks protocol version {}, TaskSmack {}", hello->version, Broker::PROTOCOL_VERSION));
    }
    m_Connected = true;
    return {};
}

void PkexecBroker::disconnect()
{
    const std::scoped_lock lock(m_Mutex);
    closeLocked();
}

std::expected<Broker::Response, std::string> PkexecBroker::call(const Broker::Request& request, const std::stop_token& stopToken)
{
    const std::scoped_lock lock(m_Mutex);
    if (!m_Connected.load())
    {
        return std::unexpected(std::string("not connected"));
    }
    auto response = roundTrip(request, CALL_TIMEOUT, stopToken);
    if (!response.has_value())
    {
        closeLocked(); // a channel that failed once isn't trusted again; connect() starts a fresh helper
    }
    return response;
}

std::expected<Broker::Response, std::string>
PkexecBroker::roundTrip(const Broker::Request& request, std::chrono::milliseconds timeout, const std::stop_token& stopToken)
{
    const std::uint32_t id = m_NextRequestId++;
    std::string error;
    if (!BrokerSocket::sendFrame(m_Fd, Broker::encodeRequest(id, request), error))
    {
        return std::unexpected(std::format("the helper's socket failed: {}", error));
    }
    const auto frame = BrokerSocket::receiveFrame(m_Fd, timeout, stopToken);
    if (!frame.has_value())
    {
        switch (frame.error())
        {
        case BrokerSocket::ReceiveEnd::Closed:
            return std::unexpected(std::string("the helper exited"));
        case BrokerSocket::ReceiveEnd::TimedOut:
            return std::unexpected(std::string(HELPER_TIMED_OUT));
        case BrokerSocket::ReceiveEnd::Stopped:
            return std::unexpected(std::string(CANCELLED));
        case BrokerSocket::ReceiveEnd::Failed:
            break;
        }
        return std::unexpected(std::string("the helper's socket failed"));
    }
    auto decoded = Broker::decodeResponse(*frame);
    if (!decoded.has_value())
    {
        return std::unexpected(std::format("the helper's answer didn't decode: {}", decoded.error()));
    }
    if (decoded->requestId != id)
    {
        return std::unexpected(std::string("the helper answered another request"));
    }
    return std::move(decoded->message);
}

void PkexecBroker::closeLocked()
{
    m_Connected = false;
    if (m_Fd >= 0)
    {
        ::close(m_Fd);
        m_Fd = -1;
    }
    // A launcher still running (a prompt still up, a helper still exiting) is kept and reaped later, by
    // the next connect() or close or the destructor, never forgotten: forgotten, it would stay this
    // process's zombie once it exited, and its child for anything that waits for any child (#2333).
    if (m_Child > 0)
    {
        m_Abandoned.push_back(m_Child);
        m_Child = -1;
    }
    reapAbandonedLocked();
}

void PkexecBroker::reapAbandonedLocked()
{
    std::erase_if(m_Abandoned, [](pid_t pid) { return ::waitpid(pid, nullptr, WNOHANG) != 0; });
}

std::string PkexecBroker::launcherExitReason()
{
    if (m_Child <= 0)
    {
        return {};
    }
    // The launcher closes its end as it exits; give it a moment to be reapable.
    for (int attempt = 0; attempt < REAP_ATTEMPTS; ++attempt)
    {
        int status = 0;
        const pid_t reaped = ::waitpid(m_Child, &status, WNOHANG);
        if (reaped == m_Child)
        {
            m_Child = -1;
            if (!WIFEXITED(status))
            {
                return "the helper was ended by a signal";
            }
            switch (WEXITSTATUS(status))
            {
            case 126:
                return "the password prompt was dismissed";
            case 127:
                return "not authorised, or pkexec isn't available";
            default:
                return std::format("the helper exited with status {}", WEXITSTATUS(status));
            }
        }
        if (reaped != 0)
        {
            return {};
        }
        ::usleep(REAP_STEP_MICROSECONDS);
    }
    return {};
}

} // namespace Platform
// NOLINTEND(misc-include-cleaner)
