/// @file test_IconLoader.cpp
/// @brief Tests for UI::Texture RAII/move semantics and UI::loadTexture()'s early-return
/// error paths (#771).
///
/// UI::IconLoader.cpp only needs imgui.h for type declarations (ImTextureID/ImVec2) - it
/// makes no ImGui:: calls - and its GL calls resolve against glad_gl_core_33, which is
/// already linked into TaskSmackTests, so the real file is compiled and linked here (unlike
/// TitleBarLayer.cpp/ProcessesPanel.cpp, which need real ImGui/ImPlot library symbols this
/// binary doesn't have). Its success path (a real image + real glGenTextures/glTexImage2D)
/// is intentionally not covered here: that needs a live OpenGL context, which this test
/// binary doesn't create. Every case below returns before any GL call is made, so no context
/// is required.

#include "UI/IconLoader.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <system_error>
#include <utility>

namespace UI
{
namespace
{

TEST(IconLoaderTest, DefaultConstructedTextureIsInvalid)
{
    const Texture tex;
    EXPECT_FALSE(tex.valid());
    EXPECT_EQ(tex.textureId(), ImTextureID{});
    EXPECT_FLOAT_EQ(tex.size().x, 0.0F);
    EXPECT_FLOAT_EQ(tex.size().y, 0.0F);
}

TEST(IconLoaderTest, MoveConstructFromDefaultLeavesBothInvalid)
{
    Texture source;
    // NOLINTNEXTLINE(bugprone-use-after-move) - checking the moved-from object's public
    // post-move state (m_Id reset to 0) is exactly the point here.
    const Texture moved(std::move(source));
    EXPECT_FALSE(moved.valid());
    EXPECT_FALSE(source.valid());
}

TEST(IconLoaderTest, MoveAssignFromDefaultLeavesBothInvalid)
{
    Texture source;
    Texture target;
    // NOLINTNEXTLINE(bugprone-use-after-move) - see above.
    target = std::move(source);
    EXPECT_FALSE(target.valid());
    EXPECT_FALSE(source.valid());
}

TEST(IconLoaderTest, SelfMoveAssignIsSafe)
{
    // Texture::operator=(Texture&&) guards against self-assignment (this != &other); this
    // exercises a default-constructed (m_Id == 0) instance so no GL call happens either way.
    Texture tex;
    auto& ref = tex;
    tex = std::move(ref);
    EXPECT_FALSE(tex.valid());
}

TEST(IconLoaderTest, LoadTextureWithEmptyPathReturnsInvalidTexture)
{
    // loadTexture() checks path.empty() before any stbi_load/GL call.
    const Texture tex = loadTexture(std::filesystem::path{});
    EXPECT_FALSE(tex.valid());
}

TEST(IconLoaderTest, LoadTextureWithNonexistentFileReturnsInvalidTexture)
{
    // stbi_load() returns nullptr for a nonexistent file before any GL call is made. Assert
    // the path is actually absent first so this can't silently become the live-GL success
    // path (needing a context this test binary doesn't create) if it ever existed on some
    // machine/CI image.
    const std::filesystem::path nonexistent{"this/path/definitely/does/not/exist_12345.png"};
    ASSERT_FALSE(std::filesystem::exists(nonexistent));

    const Texture tex = loadTexture(nonexistent);
    EXPECT_FALSE(tex.valid());
}

namespace
{
// Best-effort RAII cleanup for the temp file below: removes it on scope exit regardless of
// whether an ASSERT_* macro returns early or an exception unwinds the stack, instead of only
// on the test's normal fall-through path.
struct ScopedTempFile
{
    std::filesystem::path path;

    ~ScopedTempFile()
    {
        std::error_code ec;
        std::filesystem::remove(path, ec); // best-effort; a failed cleanup isn't a test failure
    }
};
} // namespace

TEST(IconLoaderTest, LoadTextureWithNonImageFileReturnsInvalidTexture)
{
    // A file that exists but isn't a valid image also fails inside stbi_load(), still before
    // any GL call. Write a small guaranteed-non-image temp file rather than using __FILE__:
    // __FILE__ may be a relative path (compiler-flag dependent) and isn't guaranteed to still
    // be an on-disk path at test-run time, while temp_directory_path() keeps this hermetic
    // across toolchains and working directories. The name is suffixed with a timestamp plus a
    // per-process monotonic counter (mirroring ScopedTempDir's approach, without a Linux-only
    // getpid() dependency): collisions are only practically ruled out within this process, not
    // guaranteed impossible against a different process landing on the exact same tick+counter
    // pair, but that's astronomically unlikely for a single-shot test file.
    static std::atomic<std::uint64_t> counter{0};
    const auto uniqueSuffix =
        std::format("{}_{}", std::chrono::steady_clock::now().time_since_epoch().count(), counter.fetch_add(1, std::memory_order_relaxed));
    const ScopedTempFile notAnImage{.path = std::filesystem::temp_directory_path() /
                                            std::format("tasksmack_iconloader_test_not_an_image_{}.tmp", uniqueSuffix)};
    {
        std::ofstream out(notAnImage.path, std::ios::binary);
        out << "not an image";
    }
    ASSERT_TRUE(std::filesystem::exists(notAnImage.path));

    const Texture tex = loadTexture(notAnImage.path);
    EXPECT_FALSE(tex.valid());
}

} // namespace
} // namespace UI
