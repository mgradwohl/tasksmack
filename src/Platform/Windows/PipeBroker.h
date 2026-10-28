#pragma once

// The Windows client of the privileged broker (#2311; #2181): has the helper started under a fresh
// random pipe name, opens that pipe once the helper has made it, checks the pipe's server is the helper
// it started (BrokerPipe.h has the pipe's security), and speaks BrokerProtocol to it. The twin of the
// Linux PkexecBroker, with the same semantics:
//   - connect() blocks while the helper starts (behind the UAC prompt) and for the handshake, and call()
//     for one round trip: both run on a worker (ProcessActionRun, #2305), never the UI thread;
//   - connect() is bounded as pkexec's is, by CONNECT_TIMEOUT for the prompt and again for the pipe and
//     the handshake, and gives up on its stop token; a dismissed prompt is its error, which callers
//     take as a refusal, so the UI can offer to try again;
//   - one request at a time; calls from several threads are serialised;
//   - a callIfIdle() that times out keeps the session (#2406): the helper still answers, and that late
//     reply is dropped, by the next call or, before it asks again, by callIfIdle();
//   - any other failure, a reply cut off partway included (#2451), closes the pipe, and the helper
//     exits; it also exits after a few idle minutes on its own, or, for an over-the-shoulder elevation,
//     once its one action is answered (BrokerHelper::sessionPolicy(), #2550), after which connected()
//     is false and connect() starts it again.
//
// The UAC prompt has no time limit, so the Launcher runs on a thread of its own (HELPER_LAUNCH_THREAD_NAME),
// outside the lock calls take (#2549; finding 3 of #2520's review). Meanwhile call() is "not connected"
// and callIfIdle() is "busy" (as a pkexec prompt holds PkexecBroker's lock), both at once; and the
// destructor tells the connect to give up, so neither waits for the prompt. A launch given up on is
// left to finish on its thread: Windows has no way to take a UAC prompt down (PkexecBroker can SIGTERM
// pkexec, #2370). Until that launch returns, connect() says ALREADY_ASKING rather than raise a second
// prompt. What becomes of a helper the prompt then starts depends on why its connect gave up (#2768):
//   - stopped by its caller: the next connect() uses that helper, while it still waits for its client
//     (BrokerPipeServer::ACCEPT_TIMEOUT), rather than ask again (#2572 review, finding 2);
//   - timed out (PROMPT_TIMED_OUT): the action was reported as failed, so the helper is dismissed as
//     soon as it has made its pipe: its launch thread opens the pipe and closes it again, which the
//     helper takes as its client's normal close, so it exits at once rather than wait out ACCEPT_TIMEOUT
//     for a client that won't come; and the next connect() asks again. The approved action isn't run
//     late: a kill landing minutes after the UI said the action failed would surprise more than it
//     helps, and the elevation flow is being redesigned anyway (#2728).
//
// How the helper is started is the Launcher's: the app's runs TaskSmackHelper.exe through
// ShellExecuteExW("runas") (HelperLauncher.h); the tests serve the pipe in-process.

#ifdef _WIN32
// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// clang-format on

#include "Platform/BrokerProtocol.h"
#include "Platform/IPrivilegedBroker.h"
#include "Platform/Windows/WindowsHandles.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <string_view>

namespace Platform::Windows
{

class PipeBroker final : public IPrivilegedBroker
{
  public:
    /// Starts the helper, telling it to serve @p pipeName, and returns its process, opened with at least
    /// SYNCHRONIZE and PROCESS_QUERY_LIMITED_INFORMATION: the pipe's server must be that process. Blocks
    /// while the UAC prompt is up. The error says why there is no helper (the prompt was dismissed, the
    /// helper isn't there). Called on a thread of its own (HELPER_LAUNCH_THREAD_NAME), which may outlive
    /// the PipeBroker, so it must own whatever it uses; @p stopToken is stopped once nobody waits for it.
    using Launcher =
        std::function<std::expected<UniqueHandle, std::string>(const std::wstring& pipeName, const std::stop_token& stopToken)>;

    /// How long connect() waits for the launcher (the UAC prompt), and then again for the started helper
    /// to make its pipe and answer the handshake: PkexecBroker's bound on its password prompt.
    static constexpr std::chrono::seconds CONNECT_TIMEOUT{120};
    /// How long call() waits for a response once the helper is running, and for it to take a request.
    static constexpr std::chrono::seconds CALL_TIMEOUT{10};
    /// connect()'s error while a UAC prompt it raised earlier is still up.
    static constexpr std::string_view ALREADY_ASKING =
        "already asking for an administrator's consent: the UAC prompt may be waiting behind TaskSmack or in the taskbar";
    /// connect()'s error when its UAC prompt wasn't answered within the timeout. The prompt stays up,
    /// but consent given after this starts a helper that is dismissed at once (#2768), never the action.
    static constexpr std::string_view PROMPT_TIMED_OUT =
        "the administrator's consent wasn't given in time; consent given later isn't acted on, so retry the action";

    /// @p readsOtherUsers: whether the helper it starts will run as TaskSmack's own user, and so read
    /// other users' processes (IPrivilegedBroker::readsOtherUsers(); HelperLauncher::readsOtherUsers()).
    explicit PipeBroker(Launcher launcher, bool readsOtherUsers = true);
    ~PipeBroker() override;
    PipeBroker(const PipeBroker&) = delete;
    PipeBroker& operator=(const PipeBroker&) = delete;
    PipeBroker(PipeBroker&&) = delete;
    PipeBroker& operator=(PipeBroker&&) = delete;

    /// Starts the helper and does the handshake, unless already connected. @p timeout bounds the wait
    /// for the launcher, and again what follows it: the pipe appearing, and the Hello's answer.
    /// @p stopToken, or destroying this, ends any of it early ("cancelled").
    [[nodiscard]] std::expected<void, std::string> connectWithin(const std::stop_token& stopToken, std::chrono::milliseconds timeout);

    /// connectWithin(), waiting up to CONNECT_TIMEOUT: the interface's connect().
    [[nodiscard]] std::expected<void, std::string> connect(const std::stop_token& stopToken) override
    {
        return connectWithin(stopToken, CONNECT_TIMEOUT);
    }

    /// Closes the pipe, so the helper exits; connected() is false after.
    void disconnect();

    [[nodiscard]] bool connected() const noexcept override;
    [[nodiscard]] bool lastSeenConnected() const noexcept override
    {
        return m_Connected.load();
    }
    [[nodiscard]] bool readsOtherUsers() const noexcept override
    {
        return m_ReadsOtherUsers;
    }
    /// One more for each helper connect() starts or adopts and does the handshake with (#2993).
    [[nodiscard]] std::uint64_t sessionsStarted() const noexcept override
    {
        return m_SessionsStarted.load();
    }
    [[nodiscard]] std::expected<Broker::Response, std::string> call(const Broker::Request& request,
                                                                    const std::stop_token& stopToken) override;
    [[nodiscard]] std::expected<Broker::Response, std::string> callIfIdle(const Broker::Request& request,
                                                                          std::chrono::milliseconds timeout) override;

  private:
    struct Launch;
    /// Whether a connect is running, shared with each connect so that its end is signalled through
    /// memory it owns too: the destructor waits on it while the members are still alive.
    struct ConnectState
    {
        std::mutex mutex;
        std::condition_variable done;
        bool active = false;
    };

    /// Runs m_Launcher on a thread of its own and waits up to @p timeout for it, or until @p giveUp is
    /// stopped. One given up on stays in m_Pending until it returns.
    [[nodiscard]] std::expected<UniqueHandle, std::string>
    launch(const std::wstring& pipeName, const std::stop_token& giveUp, std::chrono::milliseconds timeout);
    /// Under m_Mutex, with m_Helper the launched helper: opens its pipe and does the handshake.
    [[nodiscard]] std::expected<void, std::string>
    handshakeLocked(const std::wstring& pipeName, const std::stop_token& giveUp, std::chrono::milliseconds timeout);
    /// Under m_Mutex: sends @p request and waits up to @p timeout for the response with its id, dropping
    /// the late reply to m_Unanswered on the way. On a timeout, the request becomes m_Unanswered.
    [[nodiscard]] std::expected<Broker::Response, std::string>
    roundTrip(const Broker::Request& request, std::chrono::milliseconds timeout, const std::stop_token& stopToken);
    /// Under m_Mutex: takes m_Unanswered's late reply if it has begun to come, without waiting for it to.
    /// The error is the pipe's failure, or a reply that isn't the one awaited.
    [[nodiscard]] std::expected<void, std::string> settleUnansweredLocked(std::chrono::milliseconds timeout);
    /// Under m_Mutex: closes the pipe and lets go of the helper's process.
    void closeLocked() const noexcept;

    bool m_ReadsOtherUsers = true;
    std::shared_ptr<const Launcher> m_Launcher; // shared with each launch's thread, which may outlive this
    std::shared_ptr<ConnectState> m_ConnectState = std::make_shared<ConnectState>();
    std::shared_ptr<Launch> m_Pending; // a launch given up on that hasn't returned yet; under m_ConnectState's flag
    std::stop_source m_Closing;        // stopped by the destructor: a connect in progress gives up
    mutable std::mutex m_Mutex;
    // Mutable: connected() lets go of a helper that has exited, as PkexecBroker's does (#2449).
    mutable UniqueHandle m_Pipe;
    mutable UniqueHandle m_Helper;
    std::uint32_t m_NextRequestId = 1;
    mutable std::uint32_t m_Unanswered = 0;          // a timed-out callIfIdle() request whose reply is still to come, or 0 (#2406)
    std::atomic<bool> m_Connecting{false};           // a connect is starting the helper (#2391): callIfIdle() is "busy"
    mutable std::atomic<bool> m_Connected{false};    // cleared by connected() too, once the helper has gone
    std::atomic<std::uint64_t> m_SessionsStarted{0}; // raised under m_Mutex just before m_Connected is set (#2993)
};

} // namespace Platform::Windows

#endif // _WIN32
