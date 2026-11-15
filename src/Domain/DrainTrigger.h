#pragma once

// A worker thread that drains its owner's queue, woken by wake() (#3790): the part KeyedLookup, ProcessIconCache and
// WindowsSignatureWorker had each repeated. The owner keeps its queue, its table and the mutex that guards both;
// the trigger holds at most one drain at a time, a capacity-1 Domain::Worker whose jobs all share one key, so a
// wake() while a drain waits merges into it, and a wake() while one runs queues the next.
//
// The rule each owner follows, in this order:
//   1. Queue the item under the owner's lock, with its table entry.
//   2. Call wake(), under that lock or after it (the trigger's own lock is taken after the owner's, never before).
//   3. On Refused (no memory to queue a drain), take the item back. The drain running now may already have found
//      the queue empty, so nothing is sure to run the item, and a table entry left for it would wait for good.
//      A later lookup asks again.
//   4. On Closed the worker has ended (Failed, or stopping): the owner applies its own failure policy. A drain
//      that threw after it was admitted fails the worker without a submit() seeing it, so an owner that answers
//      from its table checks failed() on its lookups too.
//
// The drain runs on the worker's thread with no trigger lock held. It takes the owner's lock to pop each item,
// never runs the slow work under it, and returns once the queue is empty (the next wake() starts the next drain)
// or @p stop is requested (the destructor: what is still queued is dropped). Destroying the trigger stops it and
// joins the thread after the drain in progress, so it is the owner's last member, destroyed while the queue and
// table it drains into are alive.
//
// Free of the OS; tested in tests/Domain/test_DrainTrigger.cpp.

#include "Domain/Worker.h"

#include <functional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>

namespace Domain
{

class DrainTrigger
{
  public:
    using Drain = std::function<void(const std::stop_token& stop)>;

    /// @p threadName names the thread (15 bytes at most) and must outlive the trigger. The thread starts with the
    /// first wake().
    DrainTrigger(std::string_view threadName, Drain drain)
        : m_Worker(
              {
                  .threadName = threadName,
                  .capacity = 1,
                  .admission = Admission::MergeByKey,
                  .sameKey = [](const Job& /*queued*/, const Job& /*incoming*/) noexcept { return true; },
              },
              [drain = std::move(drain)](Job& /*job*/, const std::stop_token& stop, TriggerWorker::JobContext& /*context*/)
              { drain(stop); })
    {}

    ~DrainTrigger() = default;
    DrainTrigger(const DrainTrigger&) = delete;
    DrainTrigger& operator=(const DrainTrigger&) = delete;
    DrainTrigger(DrainTrigger&&) = delete; // The thread runs the drain, which holds its owner
    DrainTrigger& operator=(DrainTrigger&&) = delete;

    /// Asks for a drain: Queued (one will run), Merged (the one waiting will see the new item), Refused (none was
    /// queued: take the item back) or Closed (the worker has ended). See the rule above.
    [[nodiscard]] SubmitResult wake()
    {
        return m_Worker.submit(Job{});
    }

    /// Whether the worker has ended Failed: a drain threw, or no thread could be started.
    [[nodiscard]] bool failed() const noexcept
    {
        return m_Worker.state() == WorkerState::Failed;
    }

    /// Why it failed (empty unless failed()).
    [[nodiscard]] std::string failure() const
    {
        return m_Worker.failure();
    }

  private:
    /// A drain request: it carries nothing, so every one is the same and a waiting one absorbs the next.
    struct Job
    {};
    /// The drain publishes nothing: it files into its owner's table.
    struct Done
    {};
    using TriggerWorker = Worker<Job, Done>;

    TriggerWorker m_Worker;
};

} // namespace Domain
