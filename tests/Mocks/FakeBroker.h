/// @file FakeBroker.h
/// @brief An IPrivilegedBroker that answers from a script and records what it was asked (#2306), for
/// the decorators that retry through the helper (#2308, #2309) without a helper process.

#pragma once

#include "Platform/BrokerProtocol.h"
#include "Platform/IPrivilegedBroker.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <mutex>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace Mocks
{

class FakeBroker final : public Platform::IPrivilegedBroker
{
  public:
    using Answer = std::function<std::expected<Platform::Broker::Response, std::string>(const Platform::Broker::Request&)>;

    explicit FakeBroker(bool connected = true) : m_Connected(connected)
    {}

    /// How every call is answered; by default, a successful ActionResult.
    void setAnswer(Answer answer)
    {
        const std::scoped_lock lock(m_Mutex);
        m_Answer = std::move(answer);
    }

    void setConnected(bool connected) noexcept
    {
        m_Connected = connected;
    }

    /// Counted: the UI thread's reads must not call it, as a real broker's looks at the channel (#3182).
    [[nodiscard]] bool connected() const noexcept override
    {
        ++m_ConnectedCalls;
        return m_Connected;
    }

    /// As connected(), uncounted: the fake has no channel to look at, so the two never differ.
    [[nodiscard]] bool lastSeenConnected() const noexcept override
    {
        return m_Connected;
    }

    /// How many times connected() was called.
    [[nodiscard]] int connectedCallCount() const noexcept
    {
        return m_ConnectedCalls;
    }

    /// Whether the helper could read other users' processes (#2550): true unless a test says not.
    void setReadsOtherUsers(bool reads) noexcept
    {
        m_ReadsOtherUsers = reads;
    }

    [[nodiscard]] bool readsOtherUsers() const noexcept override
    {
        return m_ReadsOtherUsers;
    }

    /// One more each time connect() connects a fake that wasn't (#2993): a connected fake's first session is 0.
    [[nodiscard]] std::uint64_t sessionsStarted() const noexcept override
    {
        return m_SessionsStarted;
    }

    /// What connect() does: succeed (and connect) by default, or fail with this reason.
    void setConnectFailure(std::string reason)
    {
        const std::scoped_lock lock(m_Mutex);
        m_ConnectFailure = std::move(reason);
    }

    /// Run by each connect() that connects, after it does, as the time a user takes over the password prompt (#3606):
    /// what changes while the administrator is asked.
    void setOnConnect(std::function<void()> onConnect)
    {
        const std::scoped_lock lock(m_Mutex);
        m_OnConnect = std::move(onConnect);
    }

    [[nodiscard]] std::expected<void, std::string> connect(const std::stop_token& /*stopToken*/) override
    {
        std::function<void()> onConnect;
        {
            const std::scoped_lock lock(m_Mutex);
            ++m_ConnectCount;
            if (!m_ConnectFailure.empty())
            {
                return std::unexpected(m_ConnectFailure);
            }
            if (!m_Connected.exchange(true))
            {
                ++m_SessionsStarted;
            }
            onConnect = m_OnConnect;
        }
        if (onConnect)
        {
            onConnect();
        }
        return {};
    }

    /// callIfIdle() as call(), counted: the sampler's reads must use it (#2369).
    [[nodiscard]] std::expected<Platform::Broker::Response, std::string> callIfIdle(const Platform::Broker::Request& request,
                                                                                    std::chrono::milliseconds /*timeout*/) override
    {
        std::function<void()> after;
        {
            const std::scoped_lock lock(m_Mutex);
            ++m_IdleCallCount;
            after = m_AfterIdleCall;
        }
        auto response = call(request, std::stop_token{});
        if (after)
        {
            after(); // between the reply and what the caller does with it, as another worker could act there
        }
        return response;
    }

    /// Runs after each callIfIdle() has its response and before it returns, with no lock held (#3366).
    void setAfterIdleCall(std::function<void()> after)
    {
        const std::scoped_lock lock(m_Mutex);
        m_AfterIdleCall = std::move(after);
    }

    /// How many of the calls were callIfIdle().
    [[nodiscard]] int idleCallCount() const
    {
        const std::scoped_lock lock(m_Mutex);
        return m_IdleCallCount;
    }

    /// How many times connect() was called.
    [[nodiscard]] int connectCount() const
    {
        const std::scoped_lock lock(m_Mutex);
        return m_ConnectCount;
    }

    /// A stopped @p stopToken fails the call with "cancelled", as a transport's wait does.
    [[nodiscard]] std::expected<Platform::Broker::Response, std::string> call(const Platform::Broker::Request& request,
                                                                              const std::stop_token& stopToken) override
    {
        const std::scoped_lock lock(m_Mutex);
        m_Requests.push_back(request);
        if (!m_Connected)
        {
            return std::unexpected(std::string("not connected"));
        }
        if (stopToken.stop_requested())
        {
            return std::unexpected(std::string("cancelled"));
        }
        if (m_Answer)
        {
            return m_Answer(request);
        }
        return Platform::Broker::Response{Platform::Broker::ActionResult{.success = true, .errorMessage = {}, .successNote = {}}};
    }

    /// Every request made, in order.
    [[nodiscard]] std::vector<Platform::Broker::Request> requests() const
    {
        const std::scoped_lock lock(m_Mutex);
        return m_Requests;
    }

  private:
    mutable std::mutex m_Mutex;
    Answer m_Answer;
    std::vector<Platform::Broker::Request> m_Requests;
    std::string m_ConnectFailure;
    std::function<void()> m_AfterIdleCall;
    std::function<void()> m_OnConnect;
    int m_ConnectCount = 0;
    int m_IdleCallCount = 0;
    mutable std::atomic<int> m_ConnectedCalls{0};
    std::atomic<bool> m_Connected{true};
    std::atomic<std::uint64_t> m_SessionsStarted{0};
    std::atomic<bool> m_ReadsOtherUsers{true};
};

} // namespace Mocks
