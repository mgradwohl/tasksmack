#include "ProcessIconCache.h"

#include "Domain/ProcessIconCacheImpl.h" // IWYU pragma: keep - the member definitions the instantiation below needs
#include "Platform/IProcessIconSource.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
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

// The cache that ships, instantiated here once (ProcessIconCache.h declares it extern).
template class BasicProcessIconCache<NoIconCacheFaults>;

} // namespace Domain
