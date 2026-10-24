#pragma once

// The Linux client of the privileged broker (#2307; #2181): starts tasksmack-helper through pkexec, which
// asks for the administrator's password (polkit action app.tasksmack.TaskSmack.helper, auth_admin_keep),
// with one end of a SOCK_SEQPACKET socketpair as its stdin, and speaks BrokerProtocol to it over the
// other end. The socket is never on the filesystem, so nothing but the helper this started can answer.
//
// connect() blocks for as long as the password prompt is up, and call() for one round trip: both run on a
// worker (ProcessActionRun, #2305), never the UI thread. One request at a time; calls from several
// threads are serialised. A callIfIdle() that times out keeps the session (#2406): the helper still
// answers, and that late reply is dropped, by the next call or, before it asks again, by callIfIdle().
// Any other failure closes the socket. The helper exits when this closes the socket (destruction or disconnect()),
// and after a few idle minutes on its own, after which connected() is false and connect() starts it again;
// once holdForSession() has succeeded, every helper this starts is held and has no idle exit (#2728).

#include "Platform/BrokerProtocol.h"
#include "Platform/IPrivilegedBroker.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <expected>
#include <mutex>
#include <stop_token>
#include <string>
#include <vector>

#include <sys/types.h>

namespace Platform
{

class PkexecBroker final : public IPrivilegedBroker
{
  public:
    /// Where the .deb and .rpm install the helper: the path the polkit policy's exec.path names, so the
    /// prompt is the app's own and auth_admin_keep applies.
    static constexpr const char* INSTALLED_HELPER_PATH = "/usr/libexec/tasksmack/tasksmack-helper";
    /// How long connect() waits for the helper's Hello reply: the user is typing a password meanwhile.
    static constexpr std::chrono::seconds CONNECT_TIMEOUT{120};
    /// How long call() waits for a response once the helper is running.
    static constexpr std::chrono::seconds CALL_TIMEOUT{10};

    /// @p launcher is the command the helper is started through, "pkexec" in the app; the tests pass
    /// none and run the helper directly. @p helperPath is the helper to start.
    explicit PkexecBroker(std::string helperPath, std::vector<std::string> launcher = {"pkexec"});
    ~PkexecBroker() override;
    PkexecBroker(const PkexecBroker&) = delete;
    PkexecBroker& operator=(const PkexecBroker&) = delete;
    PkexecBroker(PkexecBroker&&) = delete;
    PkexecBroker& operator=(PkexecBroker&&) = delete;

    /// Starts the helper (asking for the password) and does the handshake, unless already connected.
    /// The error says why there is no helper: the prompt was dismissed or refused, the helper isn't
    /// installed, it speaks another protocol version, or it didn't answer in @p timeout.
    [[nodiscard]] std::expected<void, std::string> connectWithin(const std::stop_token& stopToken, std::chrono::milliseconds timeout);

    /// connectWithin(), waiting up to CONNECT_TIMEOUT: the interface's connect().
    [[nodiscard]] std::expected<void, std::string> connect(const std::stop_token& stopToken) override
    {
        return connectWithin(stopToken, CONNECT_TIMEOUT);
    }

    /// Closes the socket, so the helper exits; connected() is false after.
    void disconnect();

    [[nodiscard]] std::expected<void, std::string> holdForSession(const std::stop_token& stopToken) override;
    [[nodiscard]] bool heldForSession() const noexcept override;

    [[nodiscard]] bool connected() const noexcept override;
    [[nodiscard]] std::expected<Broker::Response, std::string> callIfIdle(const Broker::Request& request,
                                                                          std::chrono::milliseconds timeout) override;
    [[nodiscard]] std::expected<Broker::Response, std::string> call(const Broker::Request& request,
                                                                    const std::stop_token& stopToken) override;

  private:
    /// Under m_Mutex: sends @p request and waits up to @p timeout for the response with its id, dropping
    /// the late reply to m_Unanswered on the way. On a timeout, the request becomes m_Unanswered.
    [[nodiscard]] std::expected<Broker::Response, std::string>
    roundTrip(const Broker::Request& request, std::chrono::milliseconds timeout, const std::stop_token& stopToken);
    /// Under m_Mutex: takes m_Unanswered's late reply if it has come, without waiting for it to. The error
    /// is the socket's failure, or a reply that isn't the one awaited.
    [[nodiscard]] std::expected<void, std::string> settleUnansweredLocked(std::chrono::milliseconds timeout);
    /// Under m_Mutex: closes the socket and reaps the launcher if it has exited.
    void closeLocked();
    /// Under m_Mutex, once the helper has hung up: closes the socket and reaps the launcher if it has exited.
    void releaseExitedLocked() const noexcept;
    /// Under m_Mutex: reaps the launchers in m_Abandoned that have exited.
    void reapAbandonedLocked();
    /// Why the launcher exited, once it has: pkexec's 126 (dismissed) and 127 (not authorised) in words.
    [[nodiscard]] std::string launcherExitReason();

    std::string m_HelperPath;
    std::vector<std::string> m_Launcher;
    mutable std::mutex m_Mutex;
    // Mutable: connected() lets go of a helper that has exited (#2449).
    mutable int m_Fd = -1;
    mutable pid_t m_Child = -1;
    std::vector<pid_t> m_Abandoned; // Launchers closed while still running, reaped once they exit (#2333)
    std::uint32_t m_NextRequestId = 1;
    std::uint32_t m_Unanswered = 0;               // a timed-out callIfIdle() request whose reply is still to come, or 0 (#2406)
    std::atomic<bool> m_Connecting{false};        // a connect is asking for the password (#2391)
    std::atomic<bool> m_ForSession{false};        // holdForSession() succeeded: each Hello asks to be held (#2728)
    mutable std::atomic<bool> m_Connected{false}; // cleared by connected() too, once the helper has hung up
};

} // namespace Platform
