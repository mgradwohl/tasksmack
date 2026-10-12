/// @file test_IconTextureCache.cpp
/// @brief UI::IconTextureCache (#1800): each icon's texture is made once, the least recently drawn is
/// deleted when the cache is full, a failed one is not retried every frame, and every texture is
/// deleted by clear() and by the destructor. A fake backend counts the calls; no GL context.

#include "UI/IconTextureCache.h"

#include <glad/gl.h>
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace UI
{
namespace
{

struct BackendLog
{
    std::vector<std::uint32_t> created;
    std::vector<std::uint32_t> destroyed;
    std::uint32_t next = 1;
    bool fail = false;
};

[[nodiscard]] IconTextureBackend fakeBackend(const std::shared_ptr<BackendLog>& log)
{
    return IconTextureBackend{
        .create = [log](int /*width*/, int /*height*/, std::span<const std::uint8_t> /*rgba*/) -> std::uint32_t
        {
            if (log->fail)
            {
                return 0U;
            }
            log->created.push_back(log->next);
            return log->next++;
        },
        .destroy = [log](std::uint32_t texture) { log->destroyed.push_back(texture); },
    };
}

/// A 4x4 icon's pixels.
[[nodiscard]] std::vector<std::uint8_t> pixels()
{
    constexpr std::size_t BYTES = static_cast<std::size_t>(4) * 4 * 4;
    std::vector<std::uint8_t> bytes(BYTES, 255U);
    return bytes;
}

TEST(IconTextureCacheTest, MakesEachIconsTextureOnce)
{
    const auto log = std::make_shared<BackendLog>();
    IconTextureCache cache(8, fakeBackend(log));
    const std::uint32_t first = cache.texture(100, 4, 4, pixels());
    EXPECT_NE(first, 0U);
    EXPECT_EQ(cache.texture(100, 4, 4, pixels()), first);
    EXPECT_EQ(log->created.size(), 1U);
    EXPECT_NE(cache.texture(101, 4, 4, pixels()), first);
    EXPECT_EQ(log->created.size(), 2U);
}

TEST(IconTextureCacheTest, DeletesTheLeastRecentlyDrawnWhenFull)
{
    const auto log = std::make_shared<BackendLog>();
    IconTextureCache cache(2, fakeBackend(log));
    const std::uint32_t a = cache.texture(1, 4, 4, pixels());
    static_cast<void>(cache.texture(2, 4, 4, pixels()));
    static_cast<void>(cache.texture(1, 4, 4, pixels())); // 1 drawn again; 2 is now the oldest
    const std::uint32_t c = cache.texture(3, 4, 4, pixels());
    ASSERT_EQ(log->destroyed.size(), 1U);
    EXPECT_EQ(log->destroyed[0], 2U);
    EXPECT_EQ(cache.size(), 2U);
    EXPECT_EQ(cache.texture(1, 4, 4, pixels()), a);
    EXPECT_EQ(cache.texture(3, 4, 4, pixels()), c);
}

TEST(IconTextureCacheTest, AFailedTextureIsNotRetriedEveryFrame)
{
    const auto log = std::make_shared<BackendLog>();
    log->fail = true;
    IconTextureCache cache(8, fakeBackend(log));
    EXPECT_EQ(cache.texture(1, 4, 4, pixels()), 0U);
    log->fail = false;
    EXPECT_EQ(cache.texture(1, 4, 4, pixels()), 0U); // Remembered
    EXPECT_TRUE(log->created.empty());
    cache.clear();
    EXPECT_TRUE(log->destroyed.empty());             // Nothing was made, so nothing is deleted
    EXPECT_NE(cache.texture(1, 4, 4, pixels()), 0U); // Cleared: tried again
}

TEST(IconTextureCacheTest, ClearAndTheDestructorDeleteEveryTexture)
{
    const auto log = std::make_shared<BackendLog>();
    {
        IconTextureCache cache(8, fakeBackend(log));
        static_cast<void>(cache.texture(1, 4, 4, pixels()));
        static_cast<void>(cache.texture(2, 4, 4, pixels()));
        cache.clear();
        EXPECT_EQ(log->destroyed.size(), 2U);
        EXPECT_EQ(cache.size(), 0U);
        static_cast<void>(cache.texture(3, 4, 4, pixels()));
    }
    EXPECT_EQ(log->destroyed.size(), 3U); // The destructor deleted the last one
    EXPECT_EQ(log->created.size(), log->destroyed.size());
}

TEST(IconTextureCacheTest, TheOpenGLBackendMakesNothingWithoutGL)
{
    // A headless panel test draws with the real backend and no GL loaded: it must refuse rather than
    // call a null function. (A test that opened a real window earlier in this run loaded GL.)
    if (glGenTextures != nullptr)
    {
        GTEST_SKIP() << "GL was loaded by an earlier test";
    }
    const IconTextureBackend gl = IconTextureBackend::openGL();
    EXPECT_EQ(gl.create(4, 4, pixels()), 0U);
    EXPECT_EQ(gl.create(0, 4, pixels()), 0U);
    gl.destroy(1U); // No-op, no crash
}

} // namespace
} // namespace UI
