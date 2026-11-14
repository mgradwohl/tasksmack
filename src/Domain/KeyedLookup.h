#pragma once

// A slow fact looked up once per key on a worker thread of its own, and taken on a later call (#3718): the
// pattern the process probe's signature worker uses (#3070), for a lookup with no file stamp to check. find()
// queues an unknown key and answers nullopt; the worker runs the lookup, one key at a time; a later find()
// takes the answer. Nothing waits for it, so the sampler that asks never blocks on the lookup.
//
// - Each key is queued at most once at a time, however many callers ask for it.
// - Bounded: at most maxQueued keys wait at once (a key turned away is asked again by a later find()), and at
//   most maxEntries are kept; when full, the answers already taken are forgotten (asked again if needed).
// - A lookup that throws a std::exception is answered Value{} (none). Anything else that escapes ends the
//   Worker Failed rather than std::terminate; every key then answers Value{}, at once.
// - The thread is a Domain::Worker, started with the first key. beginDrain/endDrain run on it around each
//   pass over the queue (Windows: COM in the multithreaded apartment for the shell calls). The destructor
//   stops and joins it after the lookup in progress; the rest of the queue is dropped.
//
// Free of the OS; tested in tests/Domain/test_KeyedLookup.cpp.
//
// Locks: m_Mutex is taken before the Worker's own (find() submits under it). A drain takes m_Mutex on the
// worker's thread with no Worker lock held, and never calls the lookup under it.

#include "Domain/Worker.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <exception>
#include <functional>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace Domain
{

template<typename Value> class KeyedLookup
{
  public:
    using Lookup = std::function<Value(const std::string& key)>;

    struct Options
    {
        std::string_view threadName; ///< The worker thread's name (15 bytes at most); must outlive this
        std::size_t maxQueued = 64;  ///< The most keys waiting at once
        std::size_t maxEntries = 512;
        void (*beginDrain)() = nullptr; ///< On the worker's thread before each pass over the queue. Nullable.
        void (*endDrain)() = nullptr;   ///< And after it. Nullable.
    };

    KeyedLookup(Options options, Lookup lookup)
        : m_Options(options),
          m_Lookup(std::move(lookup)),
          m_Worker(
              {
                  .threadName = options.threadName,
                  .capacity = 1,
                  .admission = Admission::MergeByKey,
                  .sameKey = [](const Drain& /*queued*/, const Drain& /*incoming*/) noexcept { return true; },
              },
              [this](Drain& /*job*/, const std::stop_token& stop, LookupWorker::JobContext& /*context*/) { drain(stop); })
    {}

    /// Stops the worker and joins it (a lookup in progress finishes first): m_Worker, the last member, goes
    /// first, while the table it files into is alive.
    ~KeyedLookup() = default;

    KeyedLookup(const KeyedLookup&) = delete;
    KeyedLookup& operator=(const KeyedLookup&) = delete;
    KeyedLookup(KeyedLookup&&) = delete; // The thread holds `this`
    KeyedLookup& operator=(KeyedLookup&&) = delete;

    /// The answer for @p key once looked up; nullopt while it waits, with @p key queued for the worker if it
    /// isn't already (or not, when maxQueued keys wait: a later find() asks again). Never runs the lookup.
    [[nodiscard]] std::optional<Value> find(const std::string& key)
    {
        const std::scoped_lock lock(m_Mutex);
        if (const auto found = m_Entries.find(key); found != m_Entries.end())
        {
            if (found->second.has_value())
            {
                return found->second;
            }
            return std::nullopt; // Queued
        }
        if (m_Failed)
        {
            return Value{};
        }
        if (m_Queue.size() >= m_Options.maxQueued)
        {
            return std::nullopt; // Turned away: asked again later
        }
        if (m_Entries.size() >= m_Options.maxEntries)
        {
            std::erase_if(m_Entries, [](const auto& entry) { return entry.second.has_value(); }); // Keep what's queued
            if (m_Entries.size() >= m_Options.maxEntries)
            {
                return std::nullopt;
            }
        }
        m_Queue.push_back(key);
        try
        {
            m_Entries.emplace(key, std::nullopt);
        }
        catch (...)
        {
            m_Queue.pop_back();
            throw;
        }
        const SubmitResult result = m_Worker.submit(Drain{});
        if (result == SubmitResult::Refused)
        {
            // No drain queued (no memory), and the one running may have found the queue empty: take the key
            // back, so a later find() asks again rather than leave it waiting for good.
            m_Queue.pop_back();
            m_Entries.erase(key);
        }
        else if (result == SubmitResult::Closed)
        {
            failAllLocked(); // The worker has ended: every key answers none
            return Value{};
        }
        return std::nullopt;
    }

    /// Waits up to @p timeout for the queue to empty and the lookup in progress to finish (tests). True when
    /// it has.
    [[nodiscard]] bool waitUntilIdle(std::chrono::milliseconds timeout)
    {
        std::unique_lock lock(m_Mutex);
        return m_Idle.wait_for(lock, timeout, [this] { return (m_Queue.empty() && !m_LookingUp) || m_Failed; });
    }

    /// The keys held, answered or waiting (tests).
    [[nodiscard]] std::size_t size() const
    {
        const std::scoped_lock lock(m_Mutex);
        return m_Entries.size();
    }

  private:
    /// One drain of m_Queue, as the Worker's job; it carries nothing.
    struct Drain
    {};
    /// The Worker publishes nothing: a lookup files its answer in m_Entries.
    struct Drained
    {};
    using LookupWorker = Worker<Drain, Drained>;

    /// Ends every wait (under m_Mutex): each key still waiting answers none, as does every key asked after.
    void failAllLocked()
    {
        m_Failed = true;
        for (auto& entry : m_Entries)
        {
            if (!entry.second.has_value())
            {
                entry.second = Value{};
            }
        }
        m_Queue.clear();
        m_LookingUp = false;
        m_Idle.notify_all();
    }

    /// The Worker's job, on its thread: looks up the queued keys in order until the queue is empty (the next
    /// key queued submits the next drain) or @p stop (the destructor: what is still queued is dropped).
    void drain(const std::stop_token& stop)
    {
        if (m_Options.beginDrain != nullptr)
        {
            m_Options.beginDrain();
        }
        try
        {
            drainQueue(stop);
        }
        catch (...)
        {
            endDrain();
            {
                const std::scoped_lock lock(m_Mutex);
                failAllLocked();
            }
            throw; // The Worker records it and ends Failed
        }
        endDrain();
    }

    void endDrain() const
    {
        if (m_Options.endDrain != nullptr)
        {
            m_Options.endDrain();
        }
    }

    /// drain()'s loop: one key at a time, the lookup made with no lock held.
    void drainQueue(const std::stop_token& stop)
    {
        while (!stop.stop_requested())
        {
            std::string key;
            {
                const std::scoped_lock lock(m_Mutex);
                if (m_Queue.empty())
                {
                    m_Idle.notify_all();
                    return;
                }
                key = std::move(m_Queue.front());
                m_Queue.pop_front();
                m_LookingUp = true;
            }
            Value value{};
            try
            {
                value = m_Lookup(key);
            }
            catch (const std::exception&)
            {
                value = Value{}; // A lookup that failed names nothing
            }
            const std::scoped_lock lock(m_Mutex);
            if (const auto found = m_Entries.find(key); found != m_Entries.end())
            {
                found->second = std::move(value);
            }
            m_LookingUp = false;
            if (m_Queue.empty())
            {
                m_Idle.notify_all();
            }
        }
    }

    Options m_Options;
    Lookup m_Lookup;
    mutable std::mutex m_Mutex;
    std::condition_variable m_Idle;
    // Guarded by m_Mutex
    std::deque<std::string> m_Queue;                                 // Keys to look up, in order, each once
    std::unordered_map<std::string, std::optional<Value>> m_Entries; // nullopt: queued or being looked up
    bool m_LookingUp = false;
    bool m_Failed = false;
    LookupWorker m_Worker; // Last: stopped and joined first, while the rest is alive
};

} // namespace Domain
