#pragma once

// The Windows client of the privileged broker (#2311; #2181): has the helper started under a fresh
// random pipe name, opens that pipe once the helper has made it, checks the pipe's server is the helper
// it started (BrokerPipe.h has the pipe's security), and speaks BrokerProtocol to it. The twin of the
// Linux PkexecBroker, with the same semantics:
//   - connect() blocks while the helper starts (behind the UAC prompt) and for the handshake, and call()
//     for one round trip: both run on a worker (ProcessActionRun, #2305), never the UI thread;
//   - one request at a time; calls from several threads are serialised;
//   - a callIfIdle() that times out keeps the session (#2406): the helper still answers, and that late
//     reply is dropped, by the next call or, before it asks again, by callIfIdle();
//   - any other failure, a reply cut off partway included (#2451), closes the pipe, and the helper
//     exits; it also exits after a few idle minutes on its own, after which connected() is false and
//     connect() starts it again.
//
// How the helper is started is the Launcher's: the app's will run TaskSmackHelper.exe through
// ShellExecuteExW("runas") (#2517); the tests serve the pipe in-process.

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
#include <cstdint>
#include <expected>
#include <functional>
#include <mutex>
#include <stop_token>
#include <string>

namespace Platform::Windows
{

class PipeBroker final : public IPrivilegedBroker
{
  public:
    /// Starts the helper, telling it to serve @p pipeName, and returns its process, opened with at least
    /// SYNCHRONIZE and PROCESS_QUERY_LIMITED_INFORMATION: the pipe's server must be that process. Blocks
    /// while the UAC prompt is up. The error says why there is no helper (the prompt was dismissed, the
    /// helper isn't there).
    using Launcher =
        std::function<std::expected<UniqueHandle, std::string>(const std::wstring& pipeName, const std::stop_token& stopToken)>;

    /// How long connect() waits for the started helper to make its pipe and answer the handshake.
    static constexpr std::chrono::seconds CONNECT_TIMEOUT{120};
    /// How long call() waits for a response once the helper is running, and for it to take a request.
    static constexpr std::chrono::seconds CALL_TIMEOUT{10};

    explicit PipeBroker(Launcher launcher);
    ~PipeBroker() override;
    PipeBroker(const PipeBroker&) = delete;
    PipeBroker& operator=(const PipeBroker&) = delete;
    PipeBroker(PipeBroker&&) = delete;
    PipeBroker& operator=(PipeBroker&&) = delete;

    /// Starts the helper and does the handshake, unless already connected. @p timeout bounds what
    /// follows the launch: the pipe appearing, and the Hello's answer.
    [[nodiscard]] std::expected<void, std::string> connectWithin(const std::stop_token& stopToken, std::chrono::milliseconds timeout);

    /// connectWithin(), waiting up to CONNECT_TIMEOUT: the interface's connect().
    [[nodiscard]] std::expected<void, std::string> connect(const std::stop_token& stopToken) override
    {
        return connectWithin(stopToken, CONNECT_TIMEOUT);
    }

    /// Closes the pipe, so the helper exits; connected() is false after.
    void disconnect();

    [[nodiscard]] bool connected() const noexcept override;
    [[nodiscard]] std::expected<Broker::Response, std::string> call(const Broker::Request& request,
                                                                    const std::stop_token& stopToken) override;
    [[nodiscard]] std::expected<Broker::Response, std::string> callIfIdle(const Broker::Request& request,
                                                                          std::chrono::milliseconds timeout) override;

  private:
    /// Under m_Mutex: sends @p request and waits up to @p timeout for the response with its id, dropping
    /// the late reply to m_Unanswered on the way. On a timeout, the request becomes m_Unanswered.
    [[nodiscard]] std::expected<Broker::Response, std::string>
    roundTrip(const Broker::Request& request, std::chrono::milliseconds timeout, const std::stop_token& stopToken);
    /// Under m_Mutex: takes m_Unanswered's late reply if it has begun to come, without waiting for it to.
    /// The error is the pipe's failure, or a reply that isn't the one awaited.
    [[nodiscard]] std::expected<void, std::string> settleUnansweredLocked(std::chrono::milliseconds timeout);
    /// Under m_Mutex: closes the pipe and lets go of the helper's process.
    void closeLocked() const noexcept;

    Launcher m_Launcher;
    mutable std::mutex m_Mutex;
    // Mutable: connected() lets go of a helper that has exited, as PkexecBroker's does (#2449).
    mutable UniqueHandle m_Pipe;
    mutable UniqueHandle m_Helper;
    std::uint32_t m_NextRequestId = 1;
    mutable std::uint32_t m_Unanswered = 0;       // a timed-out callIfIdle() request whose reply is still to come, or 0 (#2406)
    std::atomic<bool> m_Connecting{false};        // a connect is starting the helper (#2391)
    mutable std::atomic<bool> m_Connected{false}; // cleared by connected() too, once the helper has gone
};

} // namespace Platform::Windows

#endif // _WIN32
