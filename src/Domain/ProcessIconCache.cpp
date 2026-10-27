#include "ProcessIconCache.h"

#include "Domain/LruMap.h"
#include "Platform/AsciiCase.h"
#include "Platform/IProcessIconSource.h"
#include "Platform/ThreadName.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace Domain
{

namespace
{

constexpr std::size_t BYTES_PER_PIXEL = 4;

} // namespace

std::vector<std::uint8_t> scaleIconToSquare(const Platform::IconPixels& pixels, int sizePx)
{
    if (pixels.width <= 0 || pixels.height <= 0 || sizePx <= 0)
    {
        return {};
    }
    const auto srcWidth = static_cast<std::size_t>(pixels.width);
    const auto srcHeight = static_cast<std::size_t>(pixels.height);
    const auto size = static_cast<std::size_t>(sizePx);
    if (pixels.rgba.size() != srcWidth * srcHeight * BYTES_PER_PIXEL)
    {
        return {};
    }
    if (srcWidth == size && srcHeight == size)
    {
        return pixels.rgba;
    }

    std::vector<std::uint8_t> out(size * size * BYTES_PER_PIXEL);
    if (srcWidth >= size && srcHeight >= size)
    {
        // Shrinking: every source pixel falls in exactly one target pixel, whose colour is the
        // alpha-weighted average of its source pixels and whose alpha is their plain average.
        std::vector<std::uint64_t> sums(size * size * BYTES_PER_PIXEL, 0U);
        std::vector<std::uint32_t> counts(size * size, 0U);
        std::size_t sy = 0;
        while (sy < srcHeight)
        {
            const std::size_t ty = (sy * size) / srcHeight;
            std::size_t sx = 0;
            while (sx < srcWidth)
            {
                const std::size_t tx = (sx * size) / srcWidth;
                const std::size_t src = ((sy * srcWidth) + sx) * BYTES_PER_PIXEL;
                const std::size_t target = (ty * size) + tx;
                const std::uint64_t alpha = pixels.rgba[src + 3];
                sums[(target * BYTES_PER_PIXEL)] += pixels.rgba[src] * alpha;
                sums[(target * BYTES_PER_PIXEL) + 1] += pixels.rgba[src + 1] * alpha;
                sums[(target * BYTES_PER_PIXEL) + 2] += pixels.rgba[src + 2] * alpha;
                sums[(target * BYTES_PER_PIXEL) + 3] += alpha;
                ++counts[target];
                ++sx;
            }
            ++sy;
        }
        std::size_t target = 0;
        while (target < size * size)
        {
            const std::size_t base = target * BYTES_PER_PIXEL;
            const std::uint64_t alphaSum = sums[base + 3];
            const std::uint64_t count = std::max<std::uint64_t>(counts[target], 1U);
            if (alphaSum > 0U)
            {
                out[base] = static_cast<std::uint8_t>(sums[base] / alphaSum);
                out[base + 1] = static_cast<std::uint8_t>(sums[base + 1] / alphaSum);
                out[base + 2] = static_cast<std::uint8_t>(sums[base + 2] / alphaSum);
            }
            out[base + 3] = static_cast<std::uint8_t>(alphaSum / count);
            ++target;
        }
        return out;
    }

    // Growing (or one side shorter): the nearest source pixel.
    std::size_t ty = 0;
    while (ty < size)
    {
        const std::size_t sy = std::min((ty * srcHeight) / size, srcHeight - 1);
        std::size_t tx = 0;
        while (tx < size)
        {
            const std::size_t sx = std::min((tx * srcWidth) / size, srcWidth - 1);
            const std::size_t src = ((sy * srcWidth) + sx) * BYTES_PER_PIXEL;
            const std::size_t dst = ((ty * size) + tx) * BYTES_PER_PIXEL;
            std::copy_n(
                pixels.rgba.begin() + static_cast<std::ptrdiff_t>(src), BYTES_PER_PIXEL, out.begin() + static_cast<std::ptrdiff_t>(dst));
            ++tx;
        }
        ++ty;
    }
    return out;
}

ProcessIconCache::ProcessIconCache(std::unique_ptr<Platform::IProcessIconSource> source, ProcessIconCacheConfig config)
    : m_Source(std::move(source)),
      m_Config(config),
      m_FoldCase(m_Source != nullptr && m_Source->caseInsensitivePaths()),
      m_KeyByName(m_Source != nullptr && m_Source->findsIconsByName()),
      m_Processes(config.maxProcesses),
      m_Files(config.maxFiles),
      m_Icons(config.maxIcons),
      m_Misses(config.maxMisses)
{
    if (m_Source != nullptr && m_Config.startWorker)
    {
        m_Worker = std::jthread([this](const std::stop_token& stopToken) { workerLoop(stopToken); });
    }
}

ProcessIconCache::~ProcessIconCache()
{
    if (m_Worker.joinable())
    {
        m_Worker.request_stop(); // Wakes the wait through its stop_token
        m_Worker.join();
    }
}

std::shared_ptr<const ProcessIconImage>
ProcessIconCache::lookup(std::uint64_t uniqueKey, std::int32_t pid, std::string_view name, int sizePx)
{
    return lookupEntry(false, uniqueKey, pid, name, {}, sizePx);
}

std::shared_ptr<const ProcessIconImage>
ProcessIconCache::lookupFile(std::uint64_t fileKey, std::string_view path, std::string_view name, int sizePx)
{
    if (path.empty())
    {
        return nullptr; // Nothing to read: the row shows its generic icon
    }
    return lookupEntry(true, fileKey, 0, name, path, sizePx);
}

std::shared_ptr<const ProcessIconImage>
ProcessIconCache::lookupEntry(bool file, std::uint64_t key, std::int32_t pid, std::string_view name, std::string_view path, int sizePx)
{
    if (m_Source == nullptr || sizePx <= 0)
    {
        return nullptr;
    }
    {
        const std::scoped_lock lock(m_Mutex);
        LruMap<std::uint64_t, ProcessEntry>& map = entries(file);
        if (const ProcessEntry* entry = map.find(key); entry != nullptr && entry->sizePx == sizePx && entry->name == name)
        {
            return entry->icon; // nullptr while queued, or when the process (or file) has no icon
        }
        // First seen, seen at another size (the display scale changed), or under another name (it exec'd
        // another program, #2496): queue a load.
        map.put(key, ProcessEntry{.sizePx = sizePx, .icon = nullptr, .name = std::string(name)});
        if (m_Queue.size() >= m_Config.maxQueued)
        {
            // The oldest request is the row least likely to still be on screen: forget it, so a later
            // lookup asks again rather than waiting on a load that will never run.
            const Request& oldest = m_Queue.front();
            static_cast<void>(entries(oldest.file).erase(oldest.uniqueKey));
            m_Queue.pop_front();
        }
        m_Queue.push_back(
            Request{.uniqueKey = key, .pid = pid, .sizePx = sizePx, .name = std::string(name), .file = file, .path = std::string(path)});
    }
    m_Wake.notify_one();
    return nullptr;
}

std::size_t ProcessIconCache::loadPending()
{
    std::size_t ran = 0;
    while (true)
    {
        Request request;
        {
            const std::scoped_lock lock(m_Mutex);
            if (m_Queue.empty())
            {
                return ran;
            }
            request = std::move(m_Queue.back());
            m_Queue.pop_back();
        }
        load(request);
        ++ran;
    }
}

void ProcessIconCache::workerLoop(const std::stop_token& stopToken)
{
    static_cast<void>(Platform::setCurrentThreadName(Platform::ICON_READ_THREAD_NAME));
    while (!stopToken.stop_requested())
    {
        Request request;
        {
            std::unique_lock lock(m_Mutex);
            if (!m_Wake.wait(lock, stopToken, [this] { return !m_Queue.empty(); }))
            {
                return; // Stop requested
            }
            // Newest first: the rows on screen now, not the ones scrolled past.
            request = std::move(m_Queue.back());
            m_Queue.pop_back();
        }
        load(request);
    }
}

std::string ProcessIconCache::iconKey(std::string_view executablePath, std::string_view name, int sizePx) const
{
    // Without a path (unreadable) the source can only go by name, so the key is the name, marked so
    // it can't collide with a path. A source that also goes by name with one keys by both (#2462): every
    // python3 process shares its executable, and each script's icon is its own.
    std::string key;
    if (executablePath.empty())
    {
        key = std::format("{}|?{}", sizePx, name);
    }
    else if (m_KeyByName)
    {
        key = std::format("{}|{}|{}", sizePx, executablePath, name);
    }
    else
    {
        key = std::format("{}|{}", sizePx, executablePath);
    }
    if (m_FoldCase)
    {
        // ASCII letters only: a UTF-8 byte in a path is never folded, whatever the locale (#2566).
        std::ranges::transform(key, key.begin(), Platform::foldAscii);
    }
    return key;
}

void ProcessIconCache::load(const Request& request)
{
    try
    {
        const std::string path = request.file ? request.path : m_Source->executablePath(request.pid);
        const std::string key = iconKey(path, request.name, request.sizePx);
        {
            const std::scoped_lock lock(m_Mutex);
            if (const auto* icon = m_Icons.find(key); icon != nullptr)
            {
                std::shared_ptr<const ProcessIconImage> shared = *icon;
                if (ProcessEntry* entry = entries(request.file).find(request.uniqueKey);
                    entry != nullptr && entry->sizePx == request.sizePx && entry->name == request.name)
                {
                    entry->icon = std::move(shared);
                }
                return;
            }
            if (m_Misses.find(key) != nullptr)
            {
                return; // The process's entry already says "no icon"
            }
            ++m_Loads;
        }

        std::optional<Platform::IconPixels> pixels = m_Source->loadIcon(path, request.name, iconSourceSize(request.sizePx));
        std::shared_ptr<ProcessIconImage> image;
        if (pixels.has_value())
        {
            std::vector<std::uint8_t> rgba = scaleIconToSquare(*pixels, request.sizePx);
            if (!rgba.empty())
            {
                image = std::make_shared<ProcessIconImage>(ProcessIconImage{.id = 0, .sizePx = request.sizePx, .rgba = std::move(rgba)});
            }
        }

        const std::scoped_lock lock(m_Mutex);
        if (image != nullptr)
        {
            image->id = m_NextIconId++;
            m_Icons.put(key, image);
        }
        else
        {
            m_Misses.put(key, true);
        }
        // Not if it has since been asked for under another name: that load is queued and answers it.
        if (ProcessEntry* entry = entries(request.file).find(request.uniqueKey);
            entry != nullptr && entry->sizePx == request.sizePx && entry->name == request.name)
        {
            entry->icon = std::move(image);
        }
    }
    catch (const std::exception& e)
    {
        // An icon is decoration: a failed read leaves the generic icon, and is tried again the next
        // time the process is first seen.
        if (request.file)
        {
            spdlog::debug("Icon of {} not read: {}", request.path, e.what());
        }
        else
        {
            spdlog::debug("Process icon for PID {} not read: {}", request.pid, e.what());
        }
        forget(request.file, request.uniqueKey);
    }
    catch (...)
    {
        spdlog::debug("Icon for PID {} (or file {}) not read", request.pid, request.path);
        forget(request.file, request.uniqueKey);
    }
}

void ProcessIconCache::forget(bool file, std::uint64_t key)
{
    const std::scoped_lock lock(m_Mutex);
    static_cast<void>(entries(file).erase(key));
}

ProcessIconCache::Stats ProcessIconCache::stats() const
{
    const std::scoped_lock lock(m_Mutex);
    Stats stats{.processes = m_Processes.size(),
                .files = m_Files.size(),
                .icons = m_Icons.size(),
                .misses = m_Misses.size(),
                .queued = m_Queue.size(),
                .pixelBytes = 0,
                .loads = m_Loads};
    m_Icons.forEach([&stats](const std::string& /*key*/, const std::shared_ptr<const ProcessIconImage>& icon)
                    { stats.pixelBytes += icon != nullptr ? icon->rgba.size() : 0U; });
    return stats;
}

} // namespace Domain
