#pragma once

// A slow fact looked up once per key on a worker thread of its own, and taken on a later call (#3718): the
// pattern the process probe's signature worker uses (#3070), for a lookup with no file stamp to check. find()
// queues an unknown key and answers nullopt; the worker runs the lookup, one key at a time; a later find()
// takes the answer. Nothing waits for it, so the sampler that asks never blocks on the lookup.
//
// - Each key is queued at most once at a time, however many callers ask for it.
// - Bounded: at most maxQueued keys wait at once (a key turned away is asked again by a later find()), and at
//   most maxEntries are kept; when full, the answers already taken are forgotten (asked again if needed).
// - A lookup that throws a std::exception keeps no answer: find() answers nullopt, as if it waited. One that
//   answers empty (Value{}) is kept and answered. Either is asked again (#3794), at most maxRetries times:
//   firstRetry after the first answer, then laterRetry after each retry's. An answer missing for a passing
//   reason (the shell not ready at sign-in) appears then; after the last retry the answer is kept for good
//   (none for a lookup that threw), since most keys simply have none and asking forever would be churn.
//   A retry is asked once however often find() is called. Anything else that escapes ends the Worker Failed
//   rather than std::terminate; every key then answers Value{}, at once, for good.
// - The thread is a Domain::DrainTrigger (#3790), started with the first key. beginDrain/endDrain run on it around each
//   pass over the queue (Windows: COM in the multithreaded apartment for the shell calls). The destructor
//   stops and joins it after the lookup in progress; the rest of the queue is dropped.
//
// Free of the OS; tested in tests/Domain/test_KeyedLookup.cpp.
//
// Locks: m_Mutex is taken before the Worker's own (find() submits under it). A drain takes m_Mutex on the
// worker's thread with no Worker lock held, and never calls the lookup under it.

#include "Domain/DrainTrigger.h"
#include "Domain/SamplingConfig.h"
#include "Domain/Worker.h"

#include <chrono>
#include <concepts>
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
        /// How many times a key whose lookup threw or answered empty is asked again (#3794), and after how long:
        /// firstRetry after its first answer, laterRetry after each retry's.
        int maxRetries = Sampling::KEYED_LOOKUP_EMPTY_RETRIES;
        std::chrono::milliseconds firstRetry = std::chrono::seconds(Sampling::KEYED_LOOKUP_FIRST_RETRY_SECONDS);
        std::chrono::milliseconds laterRetry = std::chrono::seconds(Sampling::KEYED_LOOKUP_LATER_RETRY_SECONDS);
        std::chrono::steady_clock::time_point (*now)() = nullptr; ///< The clock (tests); null: steady_clock
    };

    KeyedLookup(Options options, Lookup lookup)
        : m_Options(options), m_Lookup(std::move(lookup)), m_Drain(options.threadName, [this](const std::stop_token& stop) { drain(stop); })
    {}

    /// Stops the worker and joins it (a lookup in progress finishes first): m_Drain, the last member, goes
    /// first, while the table it files into is alive.
    ~KeyedLookup() = default;

    KeyedLookup(const KeyedLookup&) = delete;
    KeyedLookup& operator=(const KeyedLookup&) = delete;
    KeyedLookup(KeyedLookup&&) = delete; // The thread holds `this`
    KeyedLookup& operator=(KeyedLookup&&) = delete;

    /// The answer for @p key once looked up; nullopt while it waits, with @p key queued for the worker if it
    /// isn't already (or not, when maxQueued keys wait: a later find() asks again), and while a lookup that
    /// threw waits for its retry. An empty or failed answer due a retry is queued again. Never runs the lookup.
    [[nodiscard]] std::optional<Value> find(const std::string& key)
    {
        const std::scoped_lock lock(m_Mutex);
        if (const auto found = m_Entries.find(key); found != m_Entries.end())
        {
            Entry& entry = found->second;
            if (!entry.retryAt.has_value() || now() < *entry.retryAt || m_Queue.size() >= m_Options.maxQueued)
            {
                return entry.answer; // nullopt: queued, being looked up, or failed and not yet due (or no room)
            }
            return retryLocked(key, entry);
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
            std::erase_if(m_Entries, [](const auto& entry) { return !entry.second.inFlight(); }); // Keep what's queued
            if (m_Entries.size() >= m_Options.maxEntries)
            {
                return std::nullopt;
            }
        }
        m_Queue.push_back(key);
        try
        {
            m_Entries.emplace(key, Entry{});
        }
        catch (...)
        {
            m_Queue.pop_back();
            throw;
        }
        const SubmitResult result = m_Drain.wake();
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
    /// A key's place in the table.
    struct Entry
    {
        std::optional<Value> answer; ///< nullopt: queued, being looked up, or its lookup threw
        /// Set for a lookup that threw or answered empty with retries left: find() asks again from then (#3794)
        std::optional<std::chrono::steady_clock::time_point> retryAt;
        int retries = 0; ///< How many times it has been asked again

        /// Queued or being looked up: kept when a full table forgets answers.
        [[nodiscard]] bool inFlight() const noexcept
        {
            return !answer.has_value() && !retryAt.has_value();
        }
    };

    [[nodiscard]] std::chrono::steady_clock::time_point now() const
    {
        return (m_Options.now != nullptr) ? m_Options.now() : std::chrono::steady_clock::now();
    }

    /// Whether @p value names nothing (Value{}), so is asked for again later. A Value that can't be compared
    /// never is.
    [[nodiscard]] static bool isEmpty(const Value& value)
    {
        if constexpr (std::equality_comparable<Value>)
        {
            return value == Value{};
        }
        else
        {
            return false;
        }
    }

    /// find() for a key due a retry (under m_Mutex): queued again in place, so it keeps its count of retries.
    std::optional<Value> retryLocked(const std::string& key, Entry& entry)
    {
        m_Queue.push_back(key);
        const int retries = entry.retries + 1;
        Entry previous = std::exchange(entry, Entry{.answer = std::nullopt, .retryAt = std::nullopt, .retries = retries});
        const SubmitResult result = m_Drain.wake();
        if (result == SubmitResult::Refused)
        {
            // As for a new key: no drain queued, so take it back; still due, a later find() asks again.
            m_Queue.pop_back();
            entry = std::move(previous);
            return entry.answer;
        }
        if (result == SubmitResult::Closed)
        {
            failAllLocked(); // The worker has ended: every key answers none
            return Value{};
        }
        return std::nullopt;
    }

    /// Ends every wait (under m_Mutex): each key still waiting answers none, as does every key asked after.
    void failAllLocked()
    {
        m_Failed = true;
        for (auto& entry : m_Entries)
        {
            if (!entry.second.answer.has_value())
            {
                entry.second.answer = Value{};
            }
            entry.second.retryAt.reset(); // Nothing is asked again
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
            std::optional<Value> value;
            try
            {
                value = m_Lookup(key);
            }
            catch (const std::exception&)
            {
                value.reset(); // A lookup that failed keeps no answer while it has a retry left
            }
            const bool empty = !value.has_value() || isEmpty(*value);
            const std::chrono::steady_clock::time_point answeredAt = now();
            const std::scoped_lock lock(m_Mutex);
            if (const auto found = m_Entries.find(key); found != m_Entries.end())
            {
                Entry& entry = found->second;
                entry.retryAt.reset();
                if (empty && entry.retries < m_Options.maxRetries)
                {
                    entry.retryAt = answeredAt + ((entry.retries == 0) ? m_Options.firstRetry : m_Options.laterRetry);
                }
                else if (!value.has_value())
                {
                    value = Value{}; // Out of retries: a lookup that failed names nothing, for good
                }
                entry.answer = std::move(value);
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
    std::deque<std::string> m_Queue; // Keys to look up, in order, each once
    std::unordered_map<std::string, Entry> m_Entries;
    bool m_LookingUp = false;
    bool m_Failed = false;
    DrainTrigger m_Drain; // Last: stopped and joined first, while the rest is alive (#3790)
};

} // namespace Domain
