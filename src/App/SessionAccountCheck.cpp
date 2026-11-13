#include "App/SessionAccountCheck.h"

#include "Domain/Worker.h"
#include "Platform/SessionAccount.h"
#include "Platform/ThreadName.h"

#include <spdlog/spdlog.h>

#include <exception>
#include <memory>
#include <optional>
#include <stop_token>
#include <utility>

namespace App
{

SessionAccountCheck::SessionAccountCheck(ReadFn read)
    : m_Worker(std::make_unique<ReadWorker>(
          ReadWorker::Options{.threadName = Platform::SESSION_ACCOUNT_THREAD_NAME, .capacity = 1, .admission = Domain::Admission::Refuse},
          [read = std::move(read)](Read& /*job*/, const std::stop_token& /*stop*/, ReadWorker::JobContext& context)
          {
              Platform::SessionAccount account;
              try
              {
                  account = read();
              }
              catch (const std::exception& e)
              {
                  // As a read that can't tell: the same account.
                  spdlog::warn("Session account: the read failed: {}", e.what());
              }
              context.finish(std::optional<Platform::SessionAccount>(std::move(account)));
          }))
{
    if (m_Worker->submit(Read{}) != Domain::SubmitResult::Queued)
    {
        spdlog::warn("Session account: the read couldn't be started; treating this TaskSmack as the signed-in user");
    }
}

SessionAccountCheck::SessionAccountCheck(KnownTag /*tag*/, Platform::SessionAccount account) : m_Known(std::move(account))
{}

std::shared_ptr<SessionAccountCheck> SessionAccountCheck::known(Platform::SessionAccount account)
{
    return std::make_shared<SessionAccountCheck>(KnownTag{}, std::move(account));
}

std::optional<Platform::SessionAccount> SessionAccountCheck::result() const
{
    if (m_Worker == nullptr)
    {
        return m_Known;
    }
    if (const auto published = m_Worker->latest(); published->isFinal && published->value.has_value())
    {
        return published->value;
    }
    // A worker that failed, or never started, has no answer to come: the same account, as an unreadable one is.
    if (m_Worker->state() != Domain::WorkerState::Running)
    {
        return Platform::SessionAccount{};
    }
    return std::nullopt;
}

Platform::SessionAccount SessionAccountCheck::wait()
{
    if (m_Worker != nullptr && !m_Worker->waitUntilIdle(WAIT_LIMIT))
    {
        spdlog::warn("Session account: no answer yet; treating this TaskSmack as the signed-in user");
    }
    return result().value_or(Platform::SessionAccount{});
}

} // namespace App
