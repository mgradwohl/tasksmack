#pragma once

// The process probe's signature worker (#3070): the Processes table's Signature column checks each process
// image's Authenticode signature, which costs milliseconds a file (more for a large one, whose whole file
// is hashed for the catalog lookup), far too much for a sample, let alone the UI thread. So a request only
// queues the image's path; this worker, its own thread (ts-sig-check), checks one file at a time through
// WindowsImageSignature.h's checkSignature(), pausing between files, and the probe takes each result on a
// later sample with find(). Nothing waits for it: a process is published at once, its signature "not
// checked yet" until the answer is in.
//
// - Cached by path and {file ID, change time, size} (SignatureCache, bounded; #3292): a second process of
//   the same image costs a file open and stamp read, not another check, and a file replaced or changed is
//   checked anew. A file with no ID (FAT, some network shares) isn't cached: it is checked every time.
// - Each path is queued at most once at a time, so thirty svchost.exe processes are one check.
// - The path table, and so the queue, is bounded too: when full, the answers already taken are forgotten
//   (a path still being polled is requested again by find()). When every path in it is still queued, a new
//   path isn't admitted (#3340): it stays "not checked yet" and find() requests it again on a later sample.
//   A count of the queued paths says so at once (#3418), so a path turned away costs no sweep of the table.
// - Those later samples take turns (#3403): the first path turned away is where admission resumes, so a
//   probe asking for more images than the table holds, in the same order each sample, gets a table's worth
//   further each sample instead of the first ones over and over. The probe marks each sample with
//   beginSample(), and a turn nobody asks for through a whole sample is over.
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
#include <cstdint>
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
    std::size_t maxCachedFiles = 4096; ///< SignatureCache's bound: the paths whose answers are kept
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

    /// Queues a check of @p path (UTF-8) for a process just seen, unless one is queued already or the table
    /// is full of queued paths (then it isn't admitted, and find() asks again later). Its last
    /// answer is withdrawn until the check is done: the file may have been replaced since, and the check
    /// (from the cache when the file's stamp is unchanged) says so. Never blocks on a check.
    void request(std::string_view path)
    {
        const std::scoped_lock lock(m_Mutex);
        requestLocked(path, pathKey(path), Asker::NewProcess);
    }

    /// The answer for @p path once checked since the last request(); nullopt while queued or checking.
    /// A path the table has forgotten (it overflowed), or never admitted (it was full), is requested again
    /// on its turn (#3403).
    [[nodiscard]] std::optional<SignatureCheck> find(std::string_view path)
    {
        std::string key = pathKey(path);
        const std::scoped_lock lock(m_Mutex);
        const auto found = m_Paths.find(key);
        if (found == m_Paths.end())
        {
            requestLocked(path, std::move(key), Asker::LaterSample);
            return std::nullopt;
        }
        return found->second.result;
    }

    /// Marks the start of the probe's next sample (#3403): a path waiting its turn keeps it until the end of
    /// the sample after the last one that asked for it, so a turn whose process has gone expires on time,
    /// whichever paths are still asked for, and a second process of one image can't pass for a new sample.
    void beginSample()
    {
        const std::scoped_lock lock(m_Mutex);
        ++m_Sample;
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

    /// The paths the table holds queued or being checked, by the worker's count of them (tests, #3418).
    [[nodiscard]] std::size_t queuedPathCount() const
    {
        const std::scoped_lock lock(m_Mutex);
        return m_QueuedCount;
    }

  private:
    struct PathState
    {
        std::optional<SignatureCheck> result; ///< The last answer; nullopt while queued
        bool queued = false;
    };

    /// Who asks for a path: request(), for a process just seen, or find(), asking again on a later sample.
    enum class Asker : std::uint8_t
    {
        NewProcess,
        LaterSample,
    };

    /// Where admission resumes after the table turned a path away (#3403). One key, so O(1) memory.
    struct Turn
    {
        std::string resumeAt;          ///< The first path turned away: the next one admitted
        std::uint64_t askedInSample{}; ///< The last sample (beginSample()) that asked for it
    };

    /// request() under m_Mutex.
    void requestLocked(std::string_view path, std::string key, Asker asker)
    {
        if (const auto found = m_Paths.find(key); found != m_Paths.end() && found->second.queued)
        {
            return;
        }
        if (!m_Paths.contains(key))
        {
            if (asker == Asker::LaterSample && !isTurnLocked(key))
            {
                // Not its turn (#3403): the table turned a path away, and that one is admitted first, then
                // those after it. The probe asks in the same order each sample, so admitting this one now
                // would hand the table to the first paths in that order every sample, and the later ones
                // would never be checked. It stays "not checked yet", and comes round again.
                return;
            }
            if (m_Paths.size() >= m_Options.maxPaths)
            {
                // Full: forget what has been answered and taken; what is queued stays, its check to come.
                // When every path in it is queued there is nothing to forget, and the count says so without
                // a sweep of the whole table under the lock (#3418).
                if (m_QueuedCount < m_Paths.size())
                {
                    std::erase_if(m_Paths, [](const auto& entry) { return !entry.second.queued; });
                }
                if (m_Paths.size() >= m_Options.maxPaths)
                {
                    // Still full, every path queued (#3340): this one isn't admitted, so neither the table
                    // nor the queue grows past the bound. Nothing queued is evicted either (its check still
                    // comes). The path stays "not checked yet", and find() asks again on a later sample,
                    // once checks have drained the queue; admission resumes with it (#3403).
                    if (!m_Turn.has_value())
                    {
                        m_Turn = Turn{.resumeAt = key, .askedInSample = m_Sample};
                    }
                    else if (m_Turn->resumeAt == key)
                    {
                        m_Turn->askedInSample = m_Sample;
                    }
                    return;
                }
            }
        }
        if (m_Turn.has_value() && m_Turn->resumeAt == key)
        {
            m_Turn.reset(); // its turn has come: those after it are admitted as there's room
        }
        PathState& state = m_Paths[std::move(key)];
        state.result.reset();
        state.queued = true;
        ++m_QueuedCount; // not queued before: a queued path returned at the top
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

    /// Whether a later sample's ask for @p key, not in the table, may be admitted now (under m_Mutex): yes
    /// when no path is waiting its turn, or this is the one; no for the others until it has been. A whole
    /// sample without an ask for the one waiting (that process has gone) ends its turn, so admission never
    /// waits on a path nobody asks for.
    [[nodiscard]] bool isTurnLocked(const std::string& key)
    {
        if (!m_Turn.has_value())
        {
            return true;
        }
        if (m_Turn->resumeAt == key)
        {
            m_Turn->askedInSample = m_Sample;
            return true;
        }
        if (m_Sample > m_Turn->askedInSample + 1)
        {
            m_Turn.reset(); // the sample after its last ask went by without it
            return true;
        }
        return false;
    }

    /// Answers every queued path as Unknown with @p note, and empties the queue (under m_Mutex).
    void failQueuedLocked(std::string_view note)
    {
        for (const std::string& queued : m_Queue)
        {
            if (const auto found = m_Paths.find(pathKey(queued)); found != m_Paths.end())
            {
                found->second.result = SignatureCheck{.signature = ImageSignature::Unknown, .note = std::string(note), .signer = {}};
                markAnsweredLocked(found->second);
            }
        }
        m_Queue.clear();
        m_Idle.notify_all();
    }

    /// @p state's check is done (under m_Mutex): it is no longer queued, and the count follows.
    void markAnsweredLocked(PathState& state)
    {
        if (state.queued)
        {
            state.queued = false;
            --m_QueuedCount;
        }
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
            const std::string key = pathKey(path);
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
                    markAnsweredLocked(found->second);
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
    std::size_t m_QueuedCount = 0;                      // m_Paths' entries with queued set (#3418)
    std::optional<Turn> m_Turn;                         // Set while a path turned away waits its turn (#3403)
    std::uint64_t m_Sample = 0;                         // beginSample() calls: the probe's samples
    bool m_Checking = false;                            // The worker holds a path it has taken off the queue
    std::jthread m_Thread;                              // Last: stopped and joined first, while the rest is alive
};

} // namespace Platform::WindowsSignature
