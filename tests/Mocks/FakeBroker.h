/// @file FakeBroker.h
/// @brief An IPrivilegedBroker that answers from a script and records what it was asked (#2306), for
/// the decorators that retry through the helper (#2308, #2309) without a helper process.

#pragma once

#include "Platform/BrokerProtocol.h"
#include "Platform/IPrivilegedBroker.h"

#include <atomic>
#include <chrono>
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

    [[nodiscard]] bool connected() const noexcept override
    {
        return m_Connected;
    }

    /// What connect() does: succeed (and connect) by default, or fail with this reason.
    void setConnectFailure(std::string reason)
    {
        const std::scoped_lock lock(m_Mutex);
        m_ConnectFailure = std::move(reason);
    }

    [[nodiscard]] std::expected<void, std::string> connect(const std::stop_token& /*stopToken*/) override
    {
        const std::scoped_lock lock(m_Mutex);
        ++m_ConnectCount;
        if (!m_ConnectFailure.empty())
        {
            return std::unexpected(m_ConnectFailure);
        }
        m_Connected = true;
        return {};
    }

    /// callIfIdle() as call(), counted: the sampler's reads must use it (#2369).
    [[nodiscard]] std::expected<Platform::Broker::Response, std::string> callIfIdle(const Platform::Broker::Request& request,
                                                                                    std::chrono::milliseconds /*timeout*/) override
    {
        {
            const std::scoped_lock lock(m_Mutex);
            ++m_IdleCallCount;
        }
        return call(request, std::stop_token{});
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

    [[nodiscard]] std::expected<Platform::Broker::Response, std::string> call(const Platform::Broker::Request& request,
                                                                              const std::stop_token& /*stopToken*/) override
    {
        const std::scoped_lock lock(m_Mutex);
        m_Requests.push_back(request);
        if (!m_Connected)
        {
            return std::unexpected(std::string("not connected"));
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
    int m_ConnectCount = 0;
    int m_IdleCallCount = 0;
    std::atomic<bool> m_Connected{true};
};

} // namespace Mocks
