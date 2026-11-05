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
//
// The thread is a Domain::Worker (#3343). The path table stays the queue, as ProcessIconCache's load queue
// does (#3468): it is what keeps each path queued once, bounds it, takes turns and counts what is queued, and
// a Worker job per path would only restate that, with a scan of its queue on every request. The Worker's job
// is one drain of the table's queue, one queued at most (MergeByKey, every drain the same): a path queued
// while a drain waits merges into it, and one queued while a drain runs queues the next, which may have
// found the queue empty already.
// - The thread starts with the first request, and is stopped and joined by the destructor; a check in
//   progress finishes first (WinVerifyTrust can't be interrupted, so no cancel hook is armed), and the rest of
//   the queue is dropped. A pause between files ends at the stop.
// - A check that throws a std::exception is answered Unknown with its message. Anything else that escapes a
//   drain ends the Worker Failed rather than std::terminate; so does a thread that can't be started. A Failed
//   worker is reported once, by the next request() or find(): every path queued is answered Unknown with
//   the reason, and so is each path asked for after, so none stays "not checked yet" for good, and the
//   reason is logged once.
//
// Free of <windows.h>: the calls are SignatureFunctions, so the queueing, the caching and the hand-over are
// tested on every platform with fakes (tests/Platform/WindowsMath/test_WindowsSignatureWorker.cpp).
//
// Locks: m_Mutex is taken before the Worker's own (request() submits under it). The Worker never calls back
// into this class under its lock: a drain takes m_Mutex on the worker's thread with no Worker lock held.

#include "Domain/Worker.h"
#include "Platform/ImageSignature.h"
#include "Platform/ThreadName.h"
#include "WindowsImageSignature.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <mutex>
#include <new>
#include <optional>
#include <ranges>
#include <stop_token>
#include <string>
#include <string_view>
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
        : m_Fns(functions),
          m_Options(options),
          m_Cache(options.maxCachedFiles),
          m_Worker(
              {
                  .threadName = SIGNATURE_CHECK_THREAD_NAME,
                  .capacity = 1,
                  .admission = Domain::Admission::MergeByKey,
                  .sameKey = [](const Drain& /*queued*/, const Drain& /*incoming*/) noexcept { return true; },
              },
              [this](Drain& /*job*/, const std::stop_token& stop, CheckWorker::JobContext& /*context*/) { drain(stop); })
    {}

    /// Stops the worker and joins it (a check in progress finishes first): m_Worker, the last member, goes
    /// first, while the table it files into is alive.
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
        static_cast<void>(workerEndedLocked());
        requestLocked(path, keyLocked(path), Asker::NewProcess);
    }

    /// The answer for @p path once checked since the last request(); nullopt while queued or checking.
    /// A path the table has forgotten (it overflowed), or never admitted (it was full), is requested again
    /// on its turn (#3403).
    [[nodiscard]] std::optional<SignatureCheck> find(std::string_view path)
    {
        std::string key = pathKey(path);
        const std::scoped_lock lock(m_Mutex);
        // Before the answer: a worker that failed with paths queued answers them here, since a path already
        // in the table never reaches request() again (#3343).
        static_cast<void>(workerEndedLocked());
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

    /// Has the next drain submit() answer Refused without asking the Worker, as it does when there is no
    /// memory to queue one (tests): the one case a queued path could be left with no drain to run it. The
    /// take-back that follows runs with every key made under the lock throwing std::bad_alloc, as the memory
    /// that just ran out would: it must not need one.
    void refuseNextDrainForTesting()
    {
        const std::scoped_lock lock(m_Mutex);
        m_RefuseNextDrain = true;
    }

    /// Has the next answer made for a path queued after the worker ended throw std::bad_alloc, as copying
    /// the end note can when memory has run out (tests): the path stays queued, for the next call to answer.
    void failNextEndAnswerForTesting()
    {
        const std::scoped_lock lock(m_Mutex);
        m_EndAnswerFails = true;
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

    /// One drain of m_Queue, as the Worker's job; it carries nothing.
    struct Drain
    {};
    /// The Worker publishes nothing: a check files its answer in m_Paths.
    struct Drained
    {};
    using CheckWorker = Domain::Worker<Drain, Drained>;

    /// A path waiting in m_Queue, with its m_Paths key, made at admission: so taking it back, answering it
    /// and checking it make no key, and the take-back after a refused drain (no memory) allocates nothing.
    struct QueuedPath
    {
        std::string path;
        std::string key;
    };

    /// pathKey() under m_Mutex: every key made under the lock goes through here, so a test can have it throw
    /// where an allocation can't be allowed to (refuseNextDrainForTesting()).
    [[nodiscard]] std::string keyLocked(std::string_view path)
    {
        if (std::exchange(m_KeysFail, false))
        {
            throw std::bad_alloc();
        }
        return pathKey(path);
    }

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
        // Transactional: what allocates (the queue entry, its copies, the table's node) comes first, and a throw
        // there leaves the queue, the table and the count as they were.
        m_Queue.push_back(QueuedPath{.path = std::string(path), .key = key});
        PathState* state = nullptr;
        try
        {
            state = &m_Paths[std::move(key)];
        }
        catch (...)
        {
            m_Queue.pop_back();
            throw;
        }
        state->result.reset();
        state->queued = true;
        ++m_QueuedCount; // not queued before: a queued path returned at the top
        submitDrainLocked();
    }

    /// Has the Worker drain m_Queue (under m_Mutex), after a path was queued.
    void submitDrainLocked()
    {
        if (m_EndNote.has_value())
        {
            failQueuedLocked(*m_EndNote); // the worker has ended: answered at once, never left queued
            return;
        }
        const bool refuseForTesting = std::exchange(m_RefuseNextDrain, false);
        const Domain::SubmitResult result = refuseForTesting ? Domain::SubmitResult::Refused : m_Worker.submit(Drain{});
        if (result == Domain::SubmitResult::Refused)
        {
            // No drain queued (no memory to queue one; a queued drain would have merged), and the one running,
            // if any, may have found the queue empty already: nothing is sure to check these paths. Take them
            // back, so find() asks again on a later sample, rather than leave them "not checked yet" for good.
            // A drain that does run meanwhile finds less to do. Memory has just run out, so this allocates
            // nothing: each entry carries its key, and the lookups, erases and clear make none.
            m_KeysFail = refuseForTesting;
            for (const QueuedPath& queued : m_Queue)
            {
                if (const auto found = m_Paths.find(queued.key); found != m_Paths.end() && found->second.queued)
                {
                    --m_QueuedCount;
                    m_Paths.erase(found);
                }
            }
            m_Queue.clear();
            m_KeysFail = false;
            m_Idle.notify_all();
            return;
        }
        if (result == Domain::SubmitResult::Closed)
        {
            // The worker has ended (Closed answers only then): no thread could be started, or a drain failed
            // since the last request. Every queued path, this one too, is answered as a check that couldn't be
            // made.
            static_cast<void>(workerEndedLocked());
        }
    }

    /// Whether the Worker has ended (under m_Mutex). The first time it is seen ended, every path still
    /// queued, and the one being checked when a drain threw, is answered Unknown with the reason, which
    /// is logged; each path queued after is answered the same at once (submitDrainLocked()). A path whose
    /// answer couldn't be made then (no memory to copy the note) is still queued, and is answered here, by the
    /// next request() or find(), whichever path it asks for.
    bool workerEndedLocked()
    {
        if (m_EndNote.has_value())
        {
            if (!m_Queue.empty())
            {
                failQueuedLocked(*m_EndNote); // a path left queued when its answer couldn't be copied
            }
            return true;
        }
        const Domain::WorkerState state = m_Worker.state();
        if (state == Domain::WorkerState::Running)
        {
            return false;
        }
        // The reason first: if copying it throws, nothing has changed, and the next call reports it. A sweep
        // cut short (no memory) leaves the rest queued and counted, and the next call goes on with it.
        std::string note = state == Domain::WorkerState::Failed ? "The signature checks have stopped: " + m_Worker.failure()
                                                                : "The signature checks have stopped";
        for (PathState& entry : m_Paths | std::views::values)
        {
            if (entry.queued)
            {
                entry.result = SignatureCheck{.signature = ImageSignature::Unknown, .note = note, .signer = {}};
                markAnsweredLocked(entry);
            }
        }
        m_Queue.clear();
        m_Checking = false; // a drain that threw left its path behind; none runs any more
        spdlog::warn("SignatureWorker: {}; signatures read Unknown", note);
        m_EndNote = std::move(note);
        m_Idle.notify_all();
        return true;
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

    /// Answers every queued path as Unknown with @p note, and empties the queue (under m_Mutex). A copy of
    /// the note that throws (no memory) leaves the paths not yet answered queued, and the queue as it is, for
    /// workerEndedLocked() to answer on the next call.
    void failQueuedLocked(std::string_view note)
    {
        for (const QueuedPath& queued : m_Queue)
        {
            if (const auto found = m_Paths.find(queued.key); found != m_Paths.end())
            {
                if (std::exchange(m_EndAnswerFails, false))
                {
                    throw std::bad_alloc(); // failNextEndAnswerForTesting(): the note's copy, out of memory
                }
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

    /// The Worker's job, on its thread: checks the queued paths in request order, pausing after each, until
    /// the queue is empty (the next path queued submits the next drain) or @p stop, the job's, is signalled
    /// (the destructor: what is still queued is dropped with the worker). A std::exception from a check is
    /// its answer; anything else thrown here ends the Worker Failed (workerEndedLocked() reports it).
    void drain(const std::stop_token& stop)
    {
        if (!m_Started)
        {
            m_Started = true;
            if (m_Options.onWorkerStart != nullptr)
            {
                m_Options.onWorkerStart();
            }
        }
        while (!stop.stop_requested())
        {
            QueuedPath queued;
            {
                const std::scoped_lock lock(m_Mutex);
                if (m_Queue.empty())
                {
                    return;
                }
                queued = std::move(m_Queue.front());
                m_Queue.pop_front();
                m_Checking = true;
            }
            const std::string& key = queued.key;
            SignatureCheck check;
            try
            {
                check = checkSignature(m_Fns, &m_Cache, queued.path, key);
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
                // A gentle pace; the stop (the job's own token, signalled by the destructor) ends it at once.
                std::unique_lock lock(m_Mutex);
                static_cast<void>(m_Pause.wait_for(lock, stop, m_Options.pause, [] { return false; }));
            }
        }
    }

    SignatureFunctions m_Fns;
    WorkerOptions m_Options;
    SignatureCache m_Cache; // The worker's thread stores into it; it has its own lock
    mutable std::mutex m_Mutex;
    std::condition_variable_any m_Pause; // The pause between files waits on it, for the job's stop only
    std::condition_variable m_Idle;
    // Guarded by m_Mutex
    std::deque<QueuedPath> m_Queue;                     // Paths to check, in request order, each at most once
    std::unordered_map<std::string, PathState> m_Paths; // By pathKey()
    std::size_t m_QueuedCount = 0;                      // m_Paths' entries with queued set (#3418)
    std::optional<Turn> m_Turn;                         // Set while a path turned away waits its turn (#3403)
    std::uint64_t m_Sample = 0;                         // beginSample() calls: the probe's samples
    bool m_Checking = false;                            // The worker holds a path it has taken off the queue
    std::optional<std::string> m_EndNote;               // Set once the Worker is seen ended: why (#3343)
    bool m_RefuseNextDrain = false;                     // refuseNextDrainForTesting()
    bool m_KeysFail = false;                            // keyLocked() throws once: refuseNextDrainForTesting()
    bool m_EndAnswerFails = false;                      // failQueuedLocked() throws once: failNextEndAnswerForTesting()
    // The worker's thread's
    bool m_Started = false; // onWorkerStart has been called
    CheckWorker m_Worker;   // Last: stopped and joined first, while the rest is alive
};

} // namespace Platform::WindowsSignature
