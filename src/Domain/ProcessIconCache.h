#pragma once

// Process icons, loaded off the UI thread and kept in bounded memory (#1800).
//
// The UI thread asks lookup() for each process row it draws. The first time a process is seen, a load
// is queued and lookup() returns nothing (the row shows the generic icon); a worker thread then reads
// the process's executable and that file's icon through Platform::IProcessIconSource, scales it to
// the size asked for, and files it. Later lookups of the process return the icon, or nothing for a
// program without one, from an O(1) map lookup that does not allocate.
//
// Three bounded LRU maps hold the state:
// - processes, by uniqueKey (pid + start time, so a reused PID is a new process): what each process
//   resolved to, at what size;
// - icons, by executable path (case-folded where the platform's paths are) and size: the decoded
//   pixels, shared by every process running that executable;
// - misses, the same keys: executables with no icon, so they are not read again on every new process.
// A process keeps its icon's pixels alive (shared_ptr) after the icon map drops them, so memory is
// bounded by the process map's capacity times the distinct icons among those processes.

#include "Domain/LruMap.h"
#include "Platform/IProcessIconSource.h"

#include <array>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace Domain
{

/// An icon ready to draw: sizePx x sizePx straight-alpha RGBA. Immutable once published.
struct ProcessIconImage
{
    std::uint64_t id = 0; ///< Unique per image this cache made, never reused: a texture cache's key
    int sizePx = 0;
    std::vector<std::uint8_t> rgba; ///< sizePx * sizePx * 4 bytes
};

struct ProcessIconCacheConfig
{
    std::size_t maxIcons = 512;      ///< Decoded icons kept by executable
    std::size_t maxMisses = 2048;    ///< Executables remembered as having no icon
    std::size_t maxProcesses = 4096; ///< Processes remembered
    std::size_t maxQueued = 256;     ///< Loads waiting; the oldest is dropped (and asked again later)
    bool startWorker = true;         ///< false: no thread; loadPending() runs the loads (tests, benchmarks)
};

/// @p pixels scaled to @p sizePx square: averaged over each target pixel's source pixels (alpha-weighted,
/// so transparent pixels don't darken edges) when shrinking, nearest pixel when growing. Empty when
/// @p pixels is not width * height * 4 bytes or a side is not positive.
[[nodiscard]] std::vector<std::uint8_t> scaleIconToSquare(const Platform::IconPixels& pixels, int sizePx);

/// The size an icon drawn @p sizePx square is read at: the smallest of the sizes icons are made in
/// that is at least as large, so it is shrunk (cleanly) rather than grown (blurred) -- a 13 px row's
/// icon is read at 16 px. 256 for anything larger.
[[nodiscard]] constexpr int iconSourceSize(int sizePx) noexcept
{
    constexpr std::array<int, 10> SIZES{16, 20, 24, 32, 40, 48, 64, 96, 128, 256};
    for (const int size : SIZES)
    {
        if (size >= sizePx)
        {
            return size;
        }
    }
    return SIZES.back();
}

class ProcessIconCache
{
  public:
    struct Stats
    {
        std::size_t processes = 0;  ///< Processes remembered
        std::size_t icons = 0;      ///< Decoded icons held by the icon map
        std::size_t misses = 0;     ///< Executables remembered as having no icon
        std::size_t queued = 0;     ///< Loads waiting
        std::size_t pixelBytes = 0; ///< Pixel bytes of the icons the icon map holds
        std::uint64_t loads = 0;    ///< Icon reads asked of the source (loadIcon calls)
    };

    explicit ProcessIconCache(std::unique_ptr<Platform::IProcessIconSource> source, ProcessIconCacheConfig config = {});
    ~ProcessIconCache();

    ProcessIconCache(const ProcessIconCache&) = delete;
    ProcessIconCache& operator=(const ProcessIconCache&) = delete;
    ProcessIconCache(ProcessIconCache&&) = delete;
    ProcessIconCache& operator=(ProcessIconCache&&) = delete;

    /// The icon of process @p uniqueKey (PID @p pid, named @p name) at @p sizePx, or nullptr while it
    /// loads and when it has none. Queues the load the first time the process is seen at that size.
    /// UI thread; allocates only then.
    [[nodiscard]] std::shared_ptr<const ProcessIconImage>
    lookup(std::uint64_t uniqueKey, std::int32_t pid, std::string_view name, int sizePx);

    /// Runs the queued loads on the calling thread, newest first; how many ran. For a cache made with
    /// startWorker false; with a worker it races it harmlessly.
    std::size_t loadPending();

    [[nodiscard]] Stats stats() const;

  private:
    struct Request
    {
        std::uint64_t uniqueKey = 0;
        std::int32_t pid = 0;
        int sizePx = 0;
        std::string name;
    };

    struct ProcessEntry
    {
        int sizePx = 0;
        std::shared_ptr<const ProcessIconImage> icon; ///< nullptr while queued, and for a process without one
    };

    void workerLoop(const std::stop_token& stopToken);
    /// Loads one request; the source is called without the lock held.
    void load(const Request& request);
    /// Drops the process, so its next lookup queues a load again (after a load threw).
    void forget(std::uint64_t uniqueKey);
    [[nodiscard]] std::string iconKey(std::string_view executablePath, std::string_view name, int sizePx) const;

    std::unique_ptr<Platform::IProcessIconSource> m_Source;
    ProcessIconCacheConfig m_Config;
    bool m_FoldCase = false;

    mutable std::mutex m_Mutex;
    std::condition_variable_any m_Wake;
    LruMap<std::uint64_t, ProcessEntry> m_Processes;
    LruMap<std::string, std::shared_ptr<const ProcessIconImage>> m_Icons;
    LruMap<std::string, bool> m_Misses;
    std::deque<Request> m_Queue;
    std::uint64_t m_NextIconId = 1;
    std::uint64_t m_Loads = 0;

    std::jthread m_Worker; // Last: stopped and joined before the state above is destroyed
};

} // namespace Domain
