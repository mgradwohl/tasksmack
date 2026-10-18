#include "PipeBroker.h"

#include "Platform/BrokerProtocol.h"
#include "Platform/Windows/BrokerPipe.h"
#include "Platform/Windows/WindowsHandles.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <expected>
#include <format>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>
#include <variant>

namespace Platform::Windows
{

namespace
{

constexpr const char* HELPER_TIMED_OUT = "the helper didn't answer in time";
constexpr const char* ALREADY_STARTING = "already starting the administrator helper";

} // namespace

PipeBroker::PipeBroker(Launcher launcher) : m_Launcher(std::move(launcher))
{}

PipeBroker::~PipeBroker()
{
    try
    {
        const std::scoped_lock lock(m_Mutex);
        closeLocked(); // the helper sees its pipe close and exits
    }
    catch (...) // NOLINT(bugprone-empty-catch) - a destructor mustn't throw; the handles close with the members anyway
    {}
}

bool PipeBroker::connected() const noexcept
{
    if (!m_Connected.load())
    {
        return false;
    }
    // The helper also ends on its own (its idle exit, a crash), and then the pipe breaks (#2368). Looked
    // at when no call holds the lock; a call in progress finds out for itself.
    const std::unique_lock lock(m_Mutex, std::try_to_lock);
    if (lock.owns_lock() && m_Pipe)
    {
        const bool pipeBroken = !BrokerPipe::bytesWaiting(m_Pipe.get()).has_value();
        const bool helperExited = WaitForSingleObject(m_Helper.get(), 0) == WAIT_OBJECT_0;
        if (pipeBroken || helperExited)
        {
            closeLocked(); // not left to the next connect(), which may never come (#2449)
        }
    }
    return m_Connected.load();
}

std::expected<void, std::string> PipeBroker::connectWithin(const std::stop_token& stopToken, std::chrono::milliseconds timeout)
{
    if (m_Connected.load())
    {
        return {};
    }
    // One start at a time (#2391): a second request while a UAC prompt is up says so at once, rather
    // than queue behind the lock, deaf to its stop token, and raise another prompt after it.
    if (m_Connecting.exchange(true))
    {
        return std::unexpected(std::string(ALREADY_STARTING));
    }
    struct ConnectingGuard
    {
        std::atomic<bool>& connecting;
        explicit ConnectingGuard(std::atomic<bool>& flag) noexcept : connecting(flag)
        {}
        ConnectingGuard(const ConnectingGuard&) = delete;
        ConnectingGuard& operator=(const ConnectingGuard&) = delete;
        ConnectingGuard(ConnectingGuard&&) = delete;
        ConnectingGuard& operator=(ConnectingGuard&&) = delete;
        ~ConnectingGuard()
        {
            connecting = false;
        }
    } const guard(m_Connecting);
    const std::scoped_lock lock(m_Mutex);
    if (m_Connected.load())
    {
        return {};
    }
    closeLocked();

    auto pipeName = BrokerPipe::makePipeName();
    if (!pipeName.has_value())
    {
        return std::unexpected(std::move(pipeName.error()));
    }
    auto helper = m_Launcher(*pipeName, stopToken);
    if (!helper.has_value())
    {
        return std::unexpected(std::move(helper.error()));
    }
    if (!helper->valid())
    {
        return std::unexpected(std::string("the helper started without a process to check"));
    }
    const DWORD helperPid = GetProcessId(helper->get());
    if (helperPid == 0)
    {
        return std::unexpected(std::string("couldn't tell the helper's process ID"));
    }
    // The helper's process stays open from here to closeLocked(), so its PID can't name another process
    // while the pipe's server is checked against it.
    m_Helper = std::move(*helper);
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    auto pipe = BrokerPipe::connectToServer(*pipeName, helperPid, m_Helper.get(), timeout, stopToken);
    if (!pipe.has_value())
    {
        closeLocked();
        return std::unexpected(std::move(pipe.error()));
    }
    m_Pipe = std::move(*pipe);

    const auto left = std::max(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()),
                               std::chrono::milliseconds{0});
    const auto reply = roundTrip(Broker::Hello{}, left, stopToken);
    if (!reply.has_value())
    {
        closeLocked();
        return std::unexpected(reply.error());
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

void PipeBroker::disconnect()
{
    const std::scoped_lock lock(m_Mutex);
    closeLocked();
}

std::expected<Broker::Response, std::string> PipeBroker::call(const Broker::Request& request, const std::stop_token& stopToken)
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

std::expected<Broker::Response, std::string> PipeBroker::callIfIdle(const Broker::Request& request, std::chrono::milliseconds timeout)
{
    const std::unique_lock lock(m_Mutex, std::try_to_lock);
    if (!lock.owns_lock())
    {
        return std::unexpected(std::string("busy")); // another call holds the helper: never queue behind it
    }
    if (!m_Connected.load())
    {
        return std::unexpected(std::string("not connected"));
    }
    if (m_Unanswered != 0)
    {
        // An earlier read that timed out (#2406): the helper answers one request at a time, so until its
        // reply is in, another request would only queue behind it. Skip this read instead.
        if (auto settled = settleUnansweredLocked(timeout); !settled.has_value())
        {
            closeLocked();
            return std::unexpected(std::move(settled.error()));
        }
        if (m_Unanswered != 0)
        {
            return std::unexpected(std::string("busy"));
        }
    }
    auto response = roundTrip(request, timeout, std::stop_token{});
    // A slow read is skipped, not the end of the session (#2406): the channel is still in step, and the
    // late reply is dropped when it comes. Anything else, a reply cut off partway included (#2451), is a
    // channel that failed, which, as in call(), isn't trusted again.
    if (!response.has_value() && m_Unanswered == 0)
    {
        closeLocked();
    }
    return response;
}

std::expected<void, std::string> PipeBroker::settleUnansweredLocked(std::chrono::milliseconds timeout)
{
    const std::optional<DWORD> waiting = BrokerPipe::bytesWaiting(m_Pipe.get());
    if (!waiting.has_value())
    {
        return std::unexpected(std::string("the helper exited"));
    }
    if (*waiting == 0)
    {
        return {}; // nothing yet
    }
    // The reply has begun to arrive; the rest follows at once, so @p timeout only bounds a helper that
    // stopped mid-frame.
    const auto frame = BrokerPipe::receiveFrame(m_Pipe.get(), timeout, std::stop_token{});
    if (!frame.has_value())
    {
        return std::unexpected(
            std::string(frame.error() == BrokerPipe::ReceiveEnd::Closed ? "the helper exited" : "the helper's pipe failed"));
    }
    const auto decoded = Broker::decodeResponse(*frame);
    if (!decoded.has_value())
    {
        return std::unexpected(std::format("the helper's answer didn't decode: {}", decoded.error()));
    }
    if (decoded->requestId != m_Unanswered)
    {
        return std::unexpected(std::string("the helper answered another request"));
    }
    m_Unanswered = 0;
    return {};
}

std::expected<Broker::Response, std::string>
PipeBroker::roundTrip(const Broker::Request& request, std::chrono::milliseconds timeout, const std::stop_token& stopToken)
{
    const std::uint32_t id = m_NextRequestId++;
    std::string error;
    if (!BrokerPipe::sendFrame(m_Pipe.get(), Broker::encodeRequest(id, request), CALL_TIMEOUT, stopToken, error))
    {
        return std::unexpected(std::format("the helper's pipe failed: {}", error));
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true)
    {
        const auto left = std::max(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()),
                                   std::chrono::milliseconds{0});
        const auto frame = BrokerPipe::receiveFrame(m_Pipe.get(), left, stopToken);
        if (!frame.has_value())
        {
            switch (frame.error())
            {
            case BrokerPipe::ReceiveEnd::Closed:
                return std::unexpected(std::string("the helper exited"));
            case BrokerPipe::ReceiveEnd::TimedOut:
                // Still to come: the helper answers every request, in order. The caller decides whether
                // to wait for it (callIfIdle(), #2406) or close the pipe.
                m_Unanswered = id;
                return std::unexpected(std::string(HELPER_TIMED_OUT));
            case BrokerPipe::ReceiveEnd::Stopped:
                return std::unexpected(std::string("cancelled"));
            case BrokerPipe::ReceiveEnd::Failed:
                break;
            }
            return std::unexpected(std::string("the helper's pipe failed"));
        }
        auto decoded = Broker::decodeResponse(*frame);
        if (!decoded.has_value())
        {
            return std::unexpected(std::format("the helper's answer didn't decode: {}", decoded.error()));
        }
        if (m_Unanswered != 0 && decoded->requestId == m_Unanswered)
        {
            m_Unanswered = 0; // the late reply to a read given up on (#2406): nobody waits for it now
            continue;
        }
        if (decoded->requestId != id)
        {
            return std::unexpected(std::string("the helper answered another request"));
        }
        return std::move(decoded->message);
    }
}

void PipeBroker::closeLocked() const noexcept
{
    m_Connected = false;
    m_Unanswered = 0;
    m_Pipe.reset();
    m_Helper.reset();
}

} // namespace Platform::Windows
