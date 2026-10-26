#include "PipeBroker.h"

#include "Platform/BrokerProtocol.h"
#include "Platform/ThreadName.h"
#include "Platform/Windows/BrokerHelper.h"
#include "Platform/Windows/BrokerPipe.h"
#include "Platform/Windows/WindowsHandles.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <expected>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>

namespace Platform::Windows
{

namespace
{

constexpr const char* HELPER_TIMED_OUT = "the helper didn't answer in time";
constexpr const char* CANCELLED = "cancelled";
/// A launch's result in place of a helper its connect had timed out on before the prompt was answered
/// (#2768): the helper is dismissed, so nothing adopts it, and the next connect asks again.
constexpr const char* APPROVED_TOO_LATE = "the administrator's consent came after TaskSmack had given up waiting for it";
/// How long a helper started for a launch given up on is given to make its pipe, so it can be
/// dismissed: what connect() gives one it uses. Its process ending ends the wait sooner.
constexpr std::chrono::milliseconds DISMISS_TIMEOUT = PipeBroker::CONNECT_TIMEOUT;

/// Ends the session of @p helper, started for a launch nobody waits for any more (#2768): opens the
/// pipe it serves and closes it again, which it takes as its client's normal close, so it exits at
/// once rather than wait out BrokerPipeServer::ACCEPT_TIMEOUT. Nothing is sent, so no protocol is
/// involved. Never throws: it runs on the launch's detached thread.
void dismissHelper(const std::wstring& pipeName, const UniqueHandle& helper) noexcept
{
    try
    {
        const DWORD helperPid = GetProcessId(helper.get());
        if (helperPid == 0)
        {
            return;
        }
        // The pipe, once open, closes with its handle on return; a helper that never makes it exits on
        // its own, and an error here (it exited first, or another process took the name) needs no more.
        [[maybe_unused]] const auto pipe =
            BrokerPipe::connectToServer(pipeName, helperPid, helper.get(), DISMISS_TIMEOUT, std::stop_token{});
    }
    catch (...) // NOLINT(bugprone-empty-catch) - nothing to tell: the helper then gives up on its own timeout
    {}
}

} // namespace

/// One run of the launcher, shared by its thread and the connect waiting for it: whichever is last lets
/// go of it, the helper's handle with it.
struct PipeBroker::Launch
{
    std::wstring pipeName; // the pipe the helper is told to serve
    std::mutex mutex;
    std::condition_variable_any done;
    bool abandoned = false; // its connect timed out (#2768): a helper it starts is dismissed, not recorded
    std::optional<std::expected<UniqueHandle, std::string>> result; // set once, when the launcher returns

    [[nodiscard]] bool finished()
    {
        const std::scoped_lock lock(mutex);
        return result.has_value();
    }

    /// The helper this launch started, if it returned one that is still running: a prompt approved after
    /// its connect was stopped, whose helper still waits for its client (#2572 review, finding 2). Taken
    /// once; nothing otherwise, a helper dismissed for a connect that timed out included (#2768).
    [[nodiscard]] UniqueHandle takeLiveHelper()
    {
        const std::scoped_lock lock(mutex);
        if (!result.has_value() || !result->has_value() || !(*result)->valid() || WaitForSingleObject((*result)->get(), 0) != WAIT_TIMEOUT)
        {
            return {};
        }
        return std::move(**result);
    }
};

PipeBroker::PipeBroker(Launcher launcher, bool readsOtherUsers)
    : m_ReadsOtherUsers(readsOtherUsers), m_Launcher(std::make_shared<const Launcher>(std::move(launcher)))
{}

PipeBroker::~PipeBroker()
{
    try
    {
        // A connect in progress gives up at once: every wait it makes ends on m_Closing, and a UAC prompt
        // it is waiting for is left on its own thread (launch()). Its end is waited for only so the
        // members outlive it; it holds nothing long but m_Mutex, which a call holds for CALL_TIMEOUT at most.
        m_Closing.request_stop();
        const std::shared_ptr<ConnectState> state = m_ConnectState;
        {
            std::unique_lock lock(state->mutex);
            state->done.wait(lock, [&state] { return !state->active; });
        }
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
    // than queue, deaf to its stop token, and raise another prompt after it. The flag is in memory this
    // connect shares, so the guard's last touch, which the destructor waits for, isn't of a member.
    const std::shared_ptr<ConnectState> state = m_ConnectState;
    {
        const std::scoped_lock lock(state->mutex);
        if (state->active)
        {
            return std::unexpected(std::string(ALREADY_ASKING));
        }
        state->active = true;
    }
    m_Connecting = true;
    struct ConnectingGuard
    {
        std::atomic<bool>& connecting;
        std::shared_ptr<ConnectState> state;
        ConnectingGuard(std::atomic<bool>& flag, std::shared_ptr<ConnectState> shared) noexcept : connecting(flag), state(std::move(shared))
        {}
        ConnectingGuard(const ConnectingGuard&) = delete;
        ConnectingGuard& operator=(const ConnectingGuard&) = delete;
        ConnectingGuard(ConnectingGuard&&) = delete;
        ConnectingGuard& operator=(ConnectingGuard&&) = delete;
        ~ConnectingGuard()
        {
            connecting = false; // the last touch of the broker: the destructor is waiting for what follows
            {
                const std::scoped_lock lock(state->mutex);
                state->active = false;
            }
            state->done.notify_all();
        }
    } const guard(m_Connecting, state);
    if (m_Connected.load())
    {
        return {}; // another connect finished as this one began
    }
    // A launch given up on earlier (its prompt still up) is waited out, not stacked with a second one.
    // Once it has returned, a helper it started that still waits for its client is used, not wasted for
    // a second prompt (#2572 review, finding 2); one its thread dismissed, since the connect had timed
    // out (#2768), leaves nothing to take, and the prompt is raised again.
    UniqueHandle adopted;
    std::wstring pipeName;
    if (m_Pending != nullptr)
    {
        if (!m_Pending->finished())
        {
            return std::unexpected(std::string(ALREADY_ASKING));
        }
        adopted = m_Pending->takeLiveHelper();
        if (adopted)
        {
            pipeName = m_Pending->pipeName;
        }
        m_Pending.reset();
    }
    {
        const std::scoped_lock lock(m_Mutex);
        closeLocked(); // what is left of a session that ended
    }

    // Ends this connect's waits on the caller's stop token, or when the broker is destroyed.
    std::stop_source giveUp;
    const std::stop_callback onStop(stopToken, [&giveUp] { giveUp.request_stop(); });
    const std::stop_callback onClose(m_Closing.get_token(), [&giveUp] { giveUp.request_stop(); });

    if (!adopted)
    {
        auto name = BrokerPipe::makePipeName();
        if (!name.has_value())
        {
            return std::unexpected(std::move(name.error()));
        }
        pipeName = std::move(*name);
        // Outside m_Mutex: the UAC prompt has no time limit (#2549).
        auto helper = launch(pipeName, giveUp.get_token(), timeout);
        if (!helper.has_value())
        {
            return std::unexpected(std::move(helper.error()));
        }
        if (!helper->valid())
        {
            return std::unexpected(std::string("the helper started without a process to check"));
        }
        adopted = std::move(*helper);
    }
    const std::scoped_lock lock(m_Mutex);
    // The helper's process stays open from here to closeLocked(), so its PID can't name another process
    // while the pipe's server is checked against it.
    m_Helper = std::move(adopted);
    if (auto connected = handshakeLocked(pipeName, giveUp.get_token(), timeout); !connected.has_value())
    {
        closeLocked();
        return connected;
    }
    // A new session, counted before it is connected: whatever a call gets from it, the call was made
    // after the count saw it (#2993).
    ++m_SessionsStarted;
    m_Connected = true;
    return {};
}

std::expected<UniqueHandle, std::string>
PipeBroker::launch(const std::wstring& pipeName, const std::stop_token& giveUp, std::chrono::milliseconds timeout)
{
    // This launch's own stop: stopped with @p giveUp, and whenever this stops waiting for it, so a
    // launcher that waits on its token is let go once nobody waits for it (#2572 review, finding 3).
    std::stop_source launchStop;
    const std::stop_callback link(giveUp, [&launchStop] { launchStop.request_stop(); });
    struct StopOnReturn
    {
        std::stop_source& source;
        explicit StopOnReturn(std::stop_source& stop) noexcept : source(stop)
        {}
        StopOnReturn(const StopOnReturn&) = delete;
        StopOnReturn& operator=(const StopOnReturn&) = delete;
        StopOnReturn(StopOnReturn&&) = delete;
        StopOnReturn& operator=(StopOnReturn&&) = delete;
        ~StopOnReturn()
        {
            source.request_stop();
        }
    } const stopOnReturn(launchStop);

    std::shared_ptr<Launch> pending;
    try
    {
        pending = std::make_shared<Launch>();
        pending->pipeName = pipeName;
        // Made before the thread, so recording a result there allocates nothing: a launch always ends
        // with one, or m_Pending would hold every later connect off.
        std::expected<UniqueHandle, std::string> fallback = std::unexpected(std::string("the helper couldn't be started"));
        std::expected<UniqueHandle, std::string> tooLate = std::unexpected(std::string(APPROVED_TOO_LATE));
        // Detached: a prompt can't be taken down, so nothing could join this thread in time. It owns all
        // it touches, the launcher included.
        std::thread(
            [pending,
             launcher = m_Launcher,
             token = launchStop.get_token(),
             result = std::move(fallback),
             tooLate = std::move(tooLate)]() mutable
            {
                static_cast<void>(setCurrentThreadName(HELPER_LAUNCH_THREAD_NAME));
                try
                {
                    result = (*launcher)(pending->pipeName, token);
                }
                catch (...) // NOLINT(bugprone-empty-catch) - the fallback says so
                {}
                // A helper for a launch whose connect timed out meanwhile (#2768) isn't recorded for the
                // next connect to adopt: its action has been reported as failed. It is dismissed instead,
                // once the result is in, so a connect waiting on it isn't held up by that.
                UniqueHandle unwanted;
                try
                {
                    const std::scoped_lock lock(pending->mutex);
                    if (pending->abandoned && result.has_value())
                    {
                        unwanted = std::move(*result);
                        pending->result = std::move(tooLate);
                    }
                    else
                    {
                        pending->result = std::move(result);
                    }
                }
                catch (...) // NOLINT(bugprone-empty-catch) - the mutex failed: the connect gives up at its timeout
                {}
                pending->done.notify_all();
                if (unwanted)
                {
                    dismissHelper(pending->pipeName, unwanted);
                }
            })
            .detach();
    }
    catch (const std::exception& error)
    {
        return std::unexpected(std::format("couldn't start the helper: {}", error.what()));
    }
    std::unique_lock lock(pending->mutex);
    if (!pending->done.wait_for(lock, giveUp, timeout, [&pending] { return pending->result.has_value(); }))
    {
        // Timed out: the action is reported as failed now, so a helper the prompt starts later is
        // dismissed by the launch's thread (#2768), not left waiting for a client. Stopped: the next
        // connect may still use it (#2572 review, finding 2). Decided under the lock, so the thread
        // either recorded its helper before this gave up, or sees the decision when it does.
        const bool stopped = giveUp.stop_requested();
        pending->abandoned = !stopped;
        lock.unlock();
        m_Pending = std::move(pending);
        return std::unexpected(std::string(stopped ? std::string_view(CANCELLED) : PROMPT_TIMED_OUT));
    }
    return std::move(*pending->result);
}

std::expected<void, std::string>
PipeBroker::handshakeLocked(const std::wstring& pipeName, const std::stop_token& giveUp, std::chrono::milliseconds timeout)
{
    const DWORD helperPid = GetProcessId(m_Helper.get());
    if (helperPid == 0)
    {
        return std::unexpected(std::string("couldn't tell the helper's process ID"));
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    auto pipe = BrokerPipe::connectToServer(pipeName, helperPid, m_Helper.get(), timeout, giveUp);
    if (!pipe.has_value())
    {
        // A helper that refused to serve says why in its exit status (BrokerHelper::Exit).
        DWORD code = 0;
        if (WaitForSingleObject(m_Helper.get(), 0) == WAIT_OBJECT_0 && GetExitCodeProcess(m_Helper.get(), &code) != FALSE)
        {
            return std::unexpected(std::format("{}: {}", pipe.error(), BrokerHelper::exitMeaning(code)));
        }
        return std::unexpected(std::move(pipe.error()));
    }
    m_Pipe = std::move(*pipe);

    const auto left = std::max(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()),
                               std::chrono::milliseconds{0});
    const auto reply = roundTrip(Broker::Hello{}, left, giveUp);
    if (!reply.has_value())
    {
        return std::unexpected(reply.error());
    }
    const auto* const hello = std::get_if<Broker::HelloReply>(&*reply);
    if (hello == nullptr || !hello->accepted)
    {
        return std::unexpected(
            hello == nullptr
                ? std::string("the helper didn't answer the handshake")
                : std::format("the helper speaks protocol version {}, TaskSmack {}", hello->version, Broker::PROTOCOL_VERSION));
    }
    return {};
}

void PipeBroker::disconnect()
{
    const std::scoped_lock lock(m_Mutex);
    closeLocked();
}

std::expected<Broker::Response, std::string> PipeBroker::call(const Broker::Request& request, const std::stop_token& stopToken)
{
    // Not connected (a connect may be waiting for its UAC prompt): said at once, never after waiting
    // for the lock (#2549).
    if (!m_Connected.load())
    {
        return std::unexpected(std::string("not connected"));
    }
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
    if (!m_Connected.load())
    {
        // A connect in progress is "busy", as PkexecBroker's, whose connect holds its lock, is.
        return std::unexpected(std::string(m_Connecting.load() ? "busy" : "not connected"));
    }
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
                return std::unexpected(std::string(CANCELLED));
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
