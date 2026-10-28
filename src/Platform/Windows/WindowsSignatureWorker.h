#pragma once

// The process probe's signature worker (#3070): the Processes table's Signature column checks each process
// image's Authenticode signature, which costs milliseconds a file (more for a large one, whose whole file
// is hashed for the catalog lookup), far too much for a sample, let alone the UI thread. So a request only
// queues the image's path; this worker, its own thread (ts-sig-check), checks one file at a time through
// WindowsImageSignature.h's checkSignature(), pausing between files, and the probe takes each result on a
// later sample with find(). Nothing waits for it: a process is published at once, its signature "not
// checked yet" until the answer is in.
//
// - Cached by {path, size, last write time} (SignatureCache, bounded): a second process of the same image
//   costs a file stamp read, not another check, and a file replaced in place is checked anew.
// - Each path is queued at most once at a time, so thirty svchost.exe processes are one check.
// - The path table is bounded too: when full, the answers already taken are forgotten (a path still being
//   polled is requested again by find()).
// - The thread starts with the first request and is stopped and joined by the destructor; a check in
//   progress finishes first (WinVerifyTrust can't be interrupted), the rest of the queue is dropped.
//
// Free of <windows.h>: the calls are SignatureFunctions, so the queueing, the caching and the hand-over are
// tested on every platform with fakes (tests/Platform/WindowsMath/test_WindowsSignatureWorker.cpp).

#include "Platform/ImageSignature.h"
#include "Platform/ThreadName.h"
#include "WindowsImageSignature.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <exception>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>

namespace Platform::WindowsSignature
{

/// How the worker paces and bounds itself.
struct WorkerOptions
{
    /// The pause after each file, so a burst of new images (the first sample) is checked at a gentle pace.
    std::chrono::milliseconds pause{0};
    std::size_t maxCachedFiles = 4096; ///< SignatureCache's bound: distinct {path, stamp} answers kept
    std::size_t maxPaths = 4096;       ///< The path table's bound
    /// Called first on the worker's thread (Windows: background priority, so the checks' reads yield to
    /// everything else). Nullable.
    void (*onWorkerStart)() = nullptr;
};

class SignatureWorker
{
  public:
    SignatureWorker(const SignatureFunctions& functions, const WorkerOptions& options)
        : m_Fns(functions), m_Options(options), m_Cache(options.maxCachedFiles)
    {}

    /// Stops the worker and joins it (a check in progress finishes first).
    ~SignatureWorker() = default;

    SignatureWorker(const SignatureWorker&) = delete;
    SignatureWorker& operator=(const SignatureWorker&) = delete;
    // Not movable: the thread holds `this`
    SignatureWorker(SignatureWorker&&) = delete;
    SignatureWorker& operator=(SignatureWorker&&) = delete;

    /// Queues a check of @p path (UTF-8) for a process just seen, unless one is queued already. Its last
    /// answer is withdrawn until the check is done: the file may have been replaced since, and the check
    /// (from the cache when the file's stamp is unchanged) says so. Never blocks on a check.
    void request(std::string_view path)
    {
        const std::scoped_lock lock(m_Mutex);
        requestLocked(path, pathKey(path));
    }

    /// The answer for @p path once checked since the last request(); nullopt while queued or checking.
    /// A path the table has forgotten (it overflowed) is requested again.
    [[nodiscard]] std::optional<SignatureCheck> find(std::string_view path)
    {
        std::string key = pathKey(path);
        const std::scoped_lock lock(m_Mutex);
        const auto found = m_Paths.find(key);
        if (found == m_Paths.end())
        {
            requestLocked(path, std::move(key));
            return std::nullopt;
        }
        return found->second.result;
    }

    /// Waits up to @p timeout for the queue to empty and the check in progress to finish (tests). True
    /// when it has.
    [[nodiscard]] bool waitUntilIdle(std::chrono::milliseconds timeout)
    {
        std::unique_lock lock(m_Mutex);
        return m_Idle.wait_for(lock, timeout, [this] { return m_Queue.empty() && !m_Checking; });
    }

    /// The distinct files whose answers the cache holds (tests).
    [[nodiscard]] std::size_t cachedFileCount() const
    {
        return m_Cache.size();
    }

    /// The paths the table holds, answered or queued (tests).
    [[nodiscard]] std::size_t pathCount() const
    {
        const std::scoped_lock lock(m_Mutex);
        return m_Paths.size();
    }

  private:
    struct PathState
    {
        std::optional<SignatureCheck> result; ///< The last answer; nullopt while queued
        bool queued = false;
    };

    /// request() under m_Mutex.
    void requestLocked(std::string_view path, std::string key)
    {
        if (const auto found = m_Paths.find(key); found != m_Paths.end() && found->second.queued)
        {
            return;
        }
        if (m_Paths.size() >= m_Options.maxPaths && !m_Paths.contains(key))
        {
            // Full: forget what has been answered and taken; what is queued stays, its check to come.
            std::erase_if(m_Paths, [](const auto& entry) { return !entry.second.queued; });
        }
        PathState& state = m_Paths[std::move(key)];
        state.result.reset();
        state.queued = true;
        m_Queue.emplace_back(path);
        if (!m_Thread.joinable())
        {
            try
            {
                m_Thread = std::jthread([this](const std::stop_token& stop) { run(stop); });
            }
            catch (const std::system_error&)
            {
                // No thread: the request is answered as a check that couldn't be made, never left queued.
                failQueuedLocked("no thread could be started for the check");
                return;
            }
        }
        m_Wake.notify_one();
    }

    /// Answers every queued path as Unknown with @p note, and empties the queue (under m_Mutex).
    void failQueuedLocked(std::string_view note)
    {
        for (const std::string& queued : m_Queue)
        {
            if (const auto found = m_Paths.find(pathKey(queued)); found != m_Paths.end())
            {
                found->second.result = SignatureCheck{.signature = ImageSignature::Unknown, .note = std::string(note), .signer = {}};
                found->second.queued = false;
            }
        }
        m_Queue.clear();
        m_Idle.notify_all();
    }

    void run(const std::stop_token& stop)
    {
        setCurrentThreadName(SIGNATURE_CHECK_THREAD_NAME);
        if (m_Options.onWorkerStart != nullptr)
        {
            m_Options.onWorkerStart();
        }
        while (true)
        {
            std::string path;
            {
                std::unique_lock lock(m_Mutex);
                if (!m_Wake.wait(lock, stop, [this] { return !m_Queue.empty(); }) || stop.stop_requested())
                {
                    return; // stopped: what is still queued is dropped with the worker
                }
                path = std::move(m_Queue.front());
                m_Queue.pop_front();
                m_Checking = true;
            }
            std::string key = pathKey(path);
            SignatureCheck check;
            try
            {
                check = checkSignature(m_Fns, &m_Cache, path, key);
            }
            catch (const std::exception& e)
            {
                check = SignatureCheck{.signature = ImageSignature::Unknown, .note = e.what(), .signer = {}};
            }
            {
                const std::scoped_lock lock(m_Mutex);
                if (const auto found = m_Paths.find(key); found != m_Paths.end())
                {
                    found->second.result = std::move(check);
                    found->second.queued = false;
                }
                m_Checking = false;
                if (m_Queue.empty())
                {
                    m_Idle.notify_all();
                }
            }
            if (m_Options.pause.count() > 0)
            {
                // A gentle pace; a stop request ends the pause at once.
                std::unique_lock lock(m_Mutex);
                (void) m_Wake.wait_for(lock, stop, m_Options.pause, [] { return false; });
            }
        }
    }

    SignatureFunctions m_Fns;
    WorkerOptions m_Options;
    SignatureCache m_Cache; // The worker's thread stores into it; it has its own lock
    mutable std::mutex m_Mutex;
    std::condition_variable_any m_Wake;
    std::condition_variable m_Idle;
    // Guarded by m_Mutex
    std::deque<std::string> m_Queue;                    // Paths to check, in request order, each at most once
    std::unordered_map<std::string, PathState> m_Paths; // By pathKey()
    bool m_Checking = false;                            // The worker holds a path it has taken off the queue
    std::jthread m_Thread;                              // Last: stopped and joined first, while the rest is alive
};

} // namespace Platform::WindowsSignature
