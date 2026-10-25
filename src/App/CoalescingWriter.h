#pragma once

// Fire-and-forget writes of a snapshot, off the calling thread (#2916): the UI thread hands over a copy
// of what to write and goes on drawing, since it never does file I/O (#2731). UserConfig saves the
// settings through one, so a Settings click, a toggled CPU Details block or a remembered dump folder
// never writes config.toml on the UI thread.
//
//  - One-shot worker, as ts-battery-read is: submit() starts a std::jthread when none is running, and
//    that thread writes until nothing is pending, then ends. No thread sits idle between saves.
//  - Coalescing: at most one write is in flight. A snapshot submitted while one is being written
//    replaces any earlier snapshot not written yet, so rapid clicks cost one more write, of the last state.
//  - Failure: the sink returns false (or throws), and the failure report runs once for a run of failed
//    writes, not again until a write has succeeded; the sink logs each failure's own reason.
//  - flush() waits until everything submitted is written (the exit path, a load(), a test). The
//    destructor flushes too, so nothing submitted is lost.
//  - No thread to run on (std::system_error): the snapshot stays pending, and the next submit() or
//    flush() writes it.
//
// Free of ImGui; tested in tests/App/test_CoalescingWriter.cpp.

#include "Platform/ThreadName.h"

#include <spdlog/spdlog.h>

#include <exception>
#include <functional>
#include <mutex>
#include <optional>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

namespace App
{

/// @tparam Snapshot What one write writes: a copy, owned by the writer until it is written.
template<typename Snapshot> class CoalescingWriter
{
  public:
    /// Writes one snapshot, on the worker; true when it was written.
    using Sink = std::function<bool(const Snapshot&)>;
    /// Runs on the worker for the first failed write after a success (or after construction).
    using FailureReport = std::function<void()>;

    /// @param threadName     The worker's name (Platform/ThreadName.h, 15 bytes at most); must outlive this.
    /// @param sink           Writes a snapshot; it and everything it uses must outlive this.
    /// @param reportFailure  Reports a run of failed writes, once.
    CoalescingWriter(std::string_view threadName, Sink sink, FailureReport reportFailure)
        : m_ThreadName(threadName), m_Sink(std::move(sink)), m_ReportFailure(std::move(reportFailure))
    {}

    /// Writes whatever is still pending first (flush()).
    ~CoalescingWriter()
    {
        try
        {
            flush();
        }
        catch (...) // NOLINT(bugprone-empty-catch): join() throws only for a self-join, which flush() never is
        {}
    }

    CoalescingWriter(const CoalescingWriter&) = delete;
    CoalescingWriter& operator=(const CoalescingWriter&) = delete;
    CoalescingWriter(CoalescingWriter&&) = delete;
    CoalescingWriter& operator=(CoalescingWriter&&) = delete;

    /// Queues @p snapshot to be written, replacing one queued earlier and not written yet, and starts
    /// the worker if none is running. Never waits for a write.
    void submit(Snapshot snapshot)
    {
        const std::scoped_lock workerLock(m_WorkerMutex);
        {
            const std::scoped_lock lock(m_Mutex);
            m_Pending = std::move(snapshot);
            if (m_Running)
            {
                return; // the running worker takes it after its current write
            }
            m_Running = true;
        }
        startWorker();
    }

    /// Waits until every submitted snapshot is written: the running worker finishes (it writes the
    /// pending one before it ends), and one left pending because no thread could start is written here.
    void flush()
    {
        const std::scoped_lock workerLock(m_WorkerMutex);
        if (m_Worker.joinable())
        {
            m_Worker.join();
        }
        drain();
    }

    /// Whether a write is in flight or a snapshot is waiting for one.
    [[nodiscard]] bool busy() const
    {
        const std::scoped_lock lock(m_Mutex);
        return m_Running || m_Pending.has_value();
    }

  private:
    /// Called with m_WorkerMutex held and m_Running just set.
    void startWorker()
    {
        try
        {
            // Assigning joins the previous worker, which found nothing pending and is ending.
            m_Worker = std::jthread(
                [this]
                {
                    static_cast<void>(Platform::setCurrentThreadName(m_ThreadName));
                    drain();
                });
        }
        catch (const std::system_error& e)
        {
            spdlog::warn("No thread to write on ({}); the write waits for the next one", e.what());
            const std::scoped_lock lock(m_Mutex);
            m_Running = false;
        }
    }

    /// Writes snapshots until none is pending, then marks the writer idle.
    void drain()
    {
        for (;;)
        {
            std::optional<Snapshot> next;
            {
                const std::scoped_lock lock(m_Mutex);
                next.swap(m_Pending);
                if (!next.has_value())
                {
                    m_Running = false;
                    return;
                }
            }
            write(*next);
        }
    }

    // NOLINTNEXTLINE(bugprone-exception-escape) - spdlog catches its own formatting and sink errors inside log()
    void write(const Snapshot& snapshot) noexcept
    {
        bool written = false;
        try
        {
            written = m_Sink(snapshot);
        }
        catch (const std::exception& e)
        {
            spdlog::error("The write threw: {}", e.what());
        }
        catch (...)
        {
            spdlog::error("The write threw an unknown exception");
        }
        if (!written && !m_Failing)
        {
            try
            {
                m_ReportFailure();
            }
            catch (...) // NOLINT(bugprone-empty-catch): a report that throws is a report not made, nothing more
            {}
        }
        m_Failing = !written;
    }

    std::string_view m_ThreadName;
    Sink m_Sink;
    FailureReport m_ReportFailure;

    mutable std::mutex m_Mutex;        // guards m_Pending and m_Running, shared with the worker
    std::optional<Snapshot> m_Pending; // the next snapshot to write; a newer one replaces it
    bool m_Running = false;            // a worker was started and has not found m_Pending empty yet
    bool m_Failing = false;            // the last write failed (only the writing thread touches it; join() orders flush()'s)

    std::mutex m_WorkerMutex; // serialises submit() and flush(), the only places m_Worker changes
    std::jthread m_Worker;    // last: joined (by flush()) before anything it uses is destroyed
};

} // namespace App
